#include "clock.h"
#include "hw.h"
#include "net.h"
#include "sound.h"
#include <Preferences.h>
#include <time.h>

static const char *DAYS[7] = {"DOMINGO", "LUNES", "MARTES", "MIERCOLES", "JUEVES", "VIERNES", "SABADO"};

static uint32_t drawnKey = 0xFFFFFFFF;

// Same 3x5 pixel digits as the Pomodoro, kept local so each app stays self-contained.
static const uint8_t GLYPH[11][5] = {
  {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
  {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,1,1}, {7,5,7,5,7}, {7,5,7,1,7}, {0,2,0,2,0}};

static void drawGlyph(int g, int x, int y, int s, uint16_t col) {
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 3; c++)
      if (GLYPH[g][r] & (4 >> c)) gfx->fillRect(x + c * s, y + r * s, s, s, col);
}

static void textCentered(const char *t, int cx, int baseline, uint8_t size, uint16_t col) {
  gfx->setFont(u8g2_font_pressstart2p_8r);
  gfx->setTextSize(size);
  gfx->setTextColor(col);
  gfx->setCursor(cx - (int)strlen(t) * 4 * size, baseline);
  gfx->print(t);
  gfx->setTextSize(1);
}

static const char *conditionText(int code) {
  if (code == 0) return "DESPEJADO";
  if (code <= 3) return "NUBLADO";
  if (code == 45 || code == 48) return "NIEBLA";
  if (code >= 51 && code <= 67) return "LLUVIA";
  if (code >= 71 && code <= 77) return "NIEVE";
  if (code >= 80 && code <= 86) return "CHUBASCOS";
  if (code >= 95) return "TORMENTA";
  return "";
}

static int batteryPercent() {
  uint32_t mv = 0;
  for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(1);   // average out ADC noise
  float v = mv / 16 * 3 / 1000.0f;                              // GPIO1, 1:3 divider
  return constrain((int)((v - 3.3f) / (4.2f - 3.3f) * 100), 0, 100);
}

// Each element is cached and repainted on its own, so nothing flashes when only one value changes.
static char cDay[12], cTime[8], cDate[12], cStatus[18], cExt[8], cHum[8];
static int cLit = -1;
static bool cLow = false, needClear = true, force = true;

static bool changed(char *cache, size_t n, const char *now) {
  if (!force && strcmp(cache, now) == 0) return false;
  strlcpy(cache, now, n);
  return true;
}

static void drawCellFrame(int i, const char *label) {
  int x = 4 + i * 118, y = 168, w = 114, h = 52;
  gfx->drawRect(x, y, w, h, COL_DIM);
  gfx->drawRect(x + 1, y + 1, w - 2, h - 2, COL_DIM);
  textCentered(label, x + w / 2, y + 17, 1, COL_TEXT);
}

static void drawCellValue(int i, const char *value, uint16_t col) {
  int x = 4 + i * 118, y = 168, w = 114;
  gfx->fillRect(x + 2, y + 20, w - 4, 30, COL_BG);
  textCentered(value, x + w / 2, y + 44, 3, col);
}

static void drawTimeDigits(const char *buf, uint16_t col) {
  gfx->fillRect(0, 48, 240, 58, COL_BG);
  int x = 35;
  for (int i = 0; i < 5; i++) {
    bool colon = buf[i] == ':';
    int g = buf[i] == '-' ? -1 : buf[i] - '0';
    if (!colon) {
      if (g >= 0) drawGlyph(g, x, 52, 10, col);
      else gfx->fillRect(x + 5, 52 + 20, 20, 10, col);   // dash placeholder
    }
    x += (colon ? 10 : 30) + 10;
  }
}

// The colon blinks once per second; it lives in the gap between the hour and minute digits.
static int cColon = -1;
static void drawColon(bool on, uint16_t col) {
  gfx->fillRect(105, 52, 30, 50, COL_BG);
  if (on) {
    gfx->fillRect(115, 62, 10, 10, col);
    gfx->fillRect(115, 82, 10, 10, col);
  }
}

// Hourly chime: setting is saved, bell icon shows its state (top right).
static bool chimeOn = false;
static int cChime = -1;
static void drawBell(bool on) {
  static const uint8_t BELL[7] = {0x08, 0x1C, 0x3E, 0x3E, 0x3E, 0x7F, 0x08};
  gfx->fillRect(204, 4, 34, 32, COL_BG);
  uint16_t col = on ? gfx->color565(255, 190, 60) : COL_DIM;
  for (int r = 0; r < 7; r++)
    for (int c = 0; c < 7; c++)
      if (BELL[r] & (0x40 >> c)) gfx->fillRect(210 + c * 3, 8 + r * 3, 3, 3, col);
  if (!on) for (int i = 0; i < 24; i++) gfx->fillRect(208 + i, 7 + i, 3, 3, gfx->color565(255, 82, 64));  // slash
}

void clockInit() {
  Preferences p;
  p.begin("orient", true);
  chimeOn = p.getBool("chime", false);
  p.end();
}

void clockToggleChime() {
  chimeOn = !chimeOn;
  Preferences p;
  p.begin("orient", false);
  p.putBool("chime", chimeOn);
  p.end();
  soundPlay(chimeOn ? SND_HOUR : SND_CLICK);   // preview the chime when turning it on
  cChime = -1;
}

void clockBackground() {
  static uint32_t lastCheck = 0;
  static int lastKey = -1;
  if (millis() - lastCheck < 500) return;
  lastCheck = millis();
  struct tm t;
  if (!chimeOn || !netTime(t)) return;
  int key = t.tm_yday * 24 + t.tm_hour;
  if (t.tm_min == 0 && t.tm_sec < 5 && key != lastKey) { lastKey = key; soundPlay(SND_HOUR); }
}

// Minimal battery: five thin segments along the bottom edge.
static void drawBattery(int lit, bool low) {
  uint16_t col = low ? gfx->color565(255, 82, 64) : gfx->color565(80, 220, 120);
  for (int i = 0; i < 5; i++) gfx->fillRect(8 + i * 46, 228, 40, 6, i < lit ? col : COL_DIM);
}

void clockEnter() { needClear = true; }

void clockTick(bool visible) {
  if (!visible) return;
  uint16_t amber = gfx->color565(255, 190, 60), cyan = gfx->color565(90, 200, 255);

  if (needClear) {
    gfx->fillScreen(COL_BG);
    gfx->drawFastHLine(0, 38, 240, COL_DIM);
    drawCellFrame(0, "EXT");
    drawCellFrame(1, "HUM");
    needClear = false;
    force = true;
    cLit = -1;
    cColon = -1;
    cChime = -1;
  }

  struct tm t = {};
  bool haveTime = netTime(t);
  WeatherData w = netWeather();
  NetStatus st = netStatus();
  char buf[20];

  if (changed(cDay, sizeof cDay, haveTime ? DAYS[t.tm_wday] : "SIN HORA")) {
    gfx->fillRect(0, 0, 240, 37, COL_BG);
    textCentered(cDay, 120, 28, 2, COL_TEXT);
    cChime = -1;   // the bell shares this strip
  }

  if (haveTime) snprintf(buf, sizeof buf, "%02d:%02d", t.tm_hour, t.tm_min);
  else snprintf(buf, sizeof buf, "--:--");
  if (changed(cTime, sizeof cTime, buf)) { drawTimeDigits(cTime, amber); cColon = -1; }
  int colonOn = haveTime ? (t.tm_sec % 2 == 0) : 1;
  if (colonOn != cColon) { cColon = colonOn; drawColon(colonOn, amber); }
  if ((int)chimeOn != cChime) { cChime = chimeOn; drawBell(chimeOn); }

  if (haveTime) snprintf(buf, sizeof buf, "%02d/%02d/%02d", t.tm_mday, t.tm_mon + 1, (t.tm_year + 1900) % 100);
  else buf[0] = 0;
  if (changed(cDate, sizeof cDate, buf)) {
    gfx->fillRect(0, 118, 240, 34, COL_BG);
    textCentered(cDate, 120, 148, 3, COL_TEXT);
  }

  const char *status = w.valid ? "" : st == NET_NOCFG ? "SIN WIFI CONFIG" : st == NET_FAIL ? "SIN CONEXION" : "CONECTANDO";
  if (changed(cStatus, sizeof cStatus, status)) {
    gfx->fillRect(0, 152, 240, 14, COL_BG);
    textCentered(cStatus, 120, 162, 1, cyan);
  }

  if (w.valid) snprintf(buf, sizeof buf, "%dC", (int)lroundf(w.tempC)); else snprintf(buf, sizeof buf, "--");
  if (changed(cExt, sizeof cExt, buf)) drawCellValue(0, cExt, amber);
  if (w.valid) snprintf(buf, sizeof buf, "%d%%", w.humidity); else snprintf(buf, sizeof buf, "--");
  if (changed(cHum, sizeof cHum, buf)) drawCellValue(1, cHum, cyan);

  static uint32_t lastBat = 0;
  if (cLit < 0 || millis() - lastBat > 5000) {
    lastBat = millis();
    int pct = batteryPercent();
    int lit = pct <= 0 ? 0 : (pct + 19) / 20;
    bool low = pct <= 20;
    if (lit != cLit || low != cLow) { cLit = lit; cLow = low; drawBattery(lit, low); }
  }
  force = false;
}
