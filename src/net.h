#pragma once
#include <Arduino.h>

enum NetStatus { NET_NOCFG, NET_CONNECTING, NET_OK, NET_FAIL };

struct WeatherData {
  bool valid = false;
  float tempC = 0;
  int humidity = 0;
  int code = 0;          // WMO weather code
  uint32_t version = 0;  // bumps on every successful fetch
};

void netInit();                    // starts the background Wi-Fi/NTP/weather task
NetStatus netStatus();
WeatherData netWeather();
bool netTime(struct tm &t);        // false until NTP has set the clock

// The agent holds the radio for its whole session; the background task waits its turn.
bool netAcquire(uint32_t timeoutMs);   // takes the radio and connects; false on timeout/failure
void netRelease();                      // radio off, background task may run again
bool netConnected();
