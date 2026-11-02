#include "brightness.h"
#include <Arduino.h>
#include <Preferences.h>

#define BL_PIN 46
static const uint8_t LEVELS[] = {10, 25, 45, 70, 100};   // percent
static const int N = sizeof LEVELS;
static int idx = N - 1, dir = -1;
static bool asleep = false;

static void apply(int percent) { ledcWrite(BL_PIN, asleep ? 0 : percent * 255 / 100); }

void backlightInit() {
  Preferences p;
  p.begin("orient", true);
  idx = constrain((int)p.getUChar("bri", N - 1), 0, N - 1);
  p.end();
  dir = idx == 0 ? 1 : -1;
  ledcAttach(BL_PIN, 5000, 8);
  apply(LEVELS[idx]);
}

void backlightCycle() {
  if (idx + dir < 0 || idx + dir >= N) dir = -dir;   // bounce at the ends
  idx += dir;
  apply(LEVELS[idx]);
  Preferences p;
  p.begin("orient", false);
  p.putUChar("bri", idx);
  p.end();
}

void backlightBlank(bool blank) { apply(blank ? 0 : LEVELS[idx]); }

void backlightSleep(bool sleep) { asleep = sleep; apply(LEVELS[idx]); }
