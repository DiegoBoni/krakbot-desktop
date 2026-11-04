#include "net.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "secrets.h"

// Rosario, Argentina (UTC-3, no DST).
#define LAT "-32.95"
#define LON "-60.64"
#define TZ_OFFSET_S (-3 * 3600)
#define REFRESH_OK_MS (10UL * 60000)
#define REFRESH_FAIL_MS (60UL * 1000)
#define NTP_RESYNC_MS (60UL * 60000)

static volatile NetStatus status = NET_NOCFG;
static WeatherData weather;
static bool timeSynced = false;
static SemaphoreHandle_t radio;   // owner has the right to touch the Wi-Fi state

static bool connectWifi() {
  setCpuFrequencyMhz(160);   // TLS and the radio need it; everything else idles at 80 MHz
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 200 && WiFi.status() != WL_CONNECTED; i++) vTaskDelay(pdMS_TO_TICKS(100));
  return WiFi.status() == WL_CONNECTED;
}

static bool syncTime() {
  configTime(TZ_OFFSET_S, 0, "pool.ntp.org", "time.google.com");
  struct tm t;
  bool ok = getLocalTime(&t, 8000);
  if (ok) timeSynced = true;
  return ok;
}

static bool fetchWeather() {
  WiFiClientSecure client;
  client.setInsecure();  // public read-only weather data; no secrets travel on this link
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, "https://api.open-meteo.com/v1/forecast?latitude=" LAT "&longitude=" LON
                          "&current=temperature_2m,relative_humidity_2m,weather_code&timezone=auto"))
    return false;
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err) return false;
  JsonObject cur = doc["current"];
  if (cur.isNull()) return false;
  WeatherData w;
  w.valid = true;
  w.tempC = cur["temperature_2m"] | 0.0f;
  w.humidity = cur["relative_humidity_2m"] | 0;
  w.code = cur["weather_code"] | 0;
  w.version = weather.version + 1;
  weather = w;
  return true;
}

static void netTask(void *) {
  uint32_t lastNtp = 0;
  for (;;) {
    if (strlen(WIFI_SSID) == 0) { status = NET_NOCFG; vTaskDelay(pdMS_TO_TICKS(30000)); continue; }
    if (xSemaphoreTake(radio, pdMS_TO_TICKS(30000)) != pdTRUE) continue;   // agent is using it
    status = NET_CONNECTING;
    bool ok = false;
    if (connectWifi()) {
      if (!timeSynced || millis() - lastNtp > NTP_RESYNC_MS) { if (syncTime()) lastNtp = millis(); }
      ok = fetchWeather();
    }
    WiFi.disconnect(true, false);   // radio off between refreshes to save battery
    WiFi.mode(WIFI_OFF);
    setCpuFrequencyMhz(80);
    status = ok ? NET_OK : NET_FAIL;
    xSemaphoreGive(radio);
    vTaskDelay(pdMS_TO_TICKS(ok ? REFRESH_OK_MS : REFRESH_FAIL_MS));
  }
}

void netInit() { radio = xSemaphoreCreateMutex(); xTaskCreate(netTask, "net", 8192, nullptr, 1, nullptr); }
NetStatus netStatus() { return status; }
WeatherData netWeather() { return weather; }
bool netTime(struct tm &t) { return timeSynced && getLocalTime(&t, 0); }

bool netAcquire(uint32_t timeoutMs) {
  if (strlen(WIFI_SSID) == 0) return false;
  if (xSemaphoreTake(radio, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) return false;
  if (!connectWifi()) { WiFi.mode(WIFI_OFF); setCpuFrequencyMhz(80); xSemaphoreGive(radio); return false; }
  if (!timeSynced) syncTime();   // certificate checks need a valid clock
  return true;
}

void netRelease() {
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  setCpuFrequencyMhz(80);
  xSemaphoreGive(radio);
}

bool netConnected() { return WiFi.status() == WL_CONNECTED; }
