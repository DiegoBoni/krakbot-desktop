#include "fx.h"
#include "hw.h"
#include "brightness.h"
#include "sound.h"
#include "buttons.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"

#define BAT_HOLD 2
#define PWR_PIN 5

static const uint16_t ACCENT[3] = {0xFB20, 0x07FF, 0xF81F};   // tomato, cyan, magenta
static const char *LABEL[3] = {"POMODORO", "RELOJ", "AGENTE"};

static void icon(FxApp a, int cx, int cy, uint16_t c) {
  switch (a) {
    case FX_POMODORO:    // tomato with a leaf
      gfx->fillCircle(cx, cy + 4, 22, c);
      gfx->fillRect(cx - 2, cy - 24, 4, 8, 0x07E0);
      gfx->fillTriangle(cx, cy - 18, cx + 14, cy - 22, cx + 6, cy - 12, 0x07E0);
      break;
    case FX_CLOCK:       // clock face
      gfx->drawCircle(cx, cy, 24, c);
      gfx->drawCircle(cx, cy, 23, c);
      gfx->fillRect(cx - 1, cy - 16, 3, 17, c);
      gfx->fillRect(cx, cy - 1, 12, 3, c);
      break;
    case FX_AGENT:       // speech bubble with three dots
      gfx->fillRoundRect(cx - 26, cy - 20, 52, 34, 8, c);
      gfx->fillTriangle(cx - 12, cy + 12, cx - 4, cy + 12, cx - 14, cy + 24, c);
      for (int i = -1; i <= 1; i++) gfx->fillCircle(cx + i * 12, cy - 3, 3, COL_BG);
      break;
  }
}

void fxCloseOld() {
  const int H = gfx->height(), W = gfx->width();
  for (int i = 1; i <= 8; i++) {        // two black bands close in from top and bottom
    int h = (H / 2) * i / 8;
    gfx->fillRect(0, h - H / 16, W, H / 16, COL_BG);
    gfx->fillRect(0, H - h, W, H / 16, COL_BG);
    delay(9);
  }
  gfx->fillScreen(COL_BG);
}

void fxSplash(FxApp app) {
  const uint16_t c = ACCENT[app];
  const int W = gfx->width(), H = gfx->height(), mid = H / 2;
  gfx->fillScreen(COL_BG);
  gfx->setFont((const GFXfont *)nullptr);
  gfx->setTextSize(3);
  for (int i = 0; i <= 7; i++) {
    int half = (W / 2 - 20) * i / 7;
    gfx->fillRect(W / 2 - half, mid + 44, 2 * half, 3, c);          // underline wipes out from the centre
    if (i == 2) icon(app, W / 2, mid - 22, c);
    if (i == 4) {
      int tw = strlen(LABEL[app]) * 18;
      gfx->setTextColor(c);
      gfx->setCursor((W - tw) / 2, mid + 18);
      gfx->print(LABEL[app]);
    }
    delay(26);
  }
  delay(110);
}

void fxPowerOff() {
  fxCloseOld();
  gfx->setFont((const GFXfont *)nullptr);
  gfx->setTextSize(2);
  gfx->setTextColor(0xF800);
  gfx->setCursor((gfx->width() - 7 * 12) / 2, gfx->height() / 2 - 8);
  gfx->print("CHAU...");
  soundPlay(SND_PAUSE);
  delay(900);
  backlightSleep(true);
  gfx->displayOff();
  digitalWrite(BAT_HOLD, LOW);          // on battery this cuts our own supply
  delay(400);
  // Still alive: USB is feeding the board. Deep sleep instead, PWR wakes it.
  uint32_t t = millis();
  while (digitalRead(PWR_PIN) == LOW && millis() - t < 3000) delay(10);   // let go of the key first
  rtc_gpio_pullup_en((gpio_num_t)PWR_PIN);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PWR_PIN, 0);
  esp_deep_sleep_start();
}

// ---- volume overlay ----
static const int PX = 184, PY = 22, PW = 48, PH = 196;       // panel, display frame
static const int BX = PX + 14, BY = PY + 40, BW = 20, BH = 120;
static bool shown = false;
static uint32_t hideAt = 0;
static int lastPct = -1;

static void speaker(int x, int y, uint16_t c, bool mute) {
  gfx->fillRect(x, y + 5, 6, 8, c);
  gfx->fillTriangle(x + 6, y + 5, x + 6, y + 13, x + 14, y + 17, c);
  gfx->fillTriangle(x + 6, y + 5, x + 14, y + 1, x + 14, y + 17, c);
  if (mute) {
    gfx->drawLine(x + 17, y + 4, x + 25, y + 14, 0xF800);
    gfx->drawLine(x + 25, y + 4, x + 17, y + 14, 0xF800);
  } else {
    gfx->drawFastVLine(x + 18, y + 6, 6, c);
    gfx->drawFastVLine(x + 21, y + 3, 12, c);
  }
}

void fxVolumeShow(int pct) {
  pct = constrain(pct, 0, 100);
  if (!shown) {
    gfx->fillRoundRect(PX, PY, PW, PH, 8, 0x2104);
    gfx->drawRoundRect(PX, PY, PW, PH, 8, COL_DIM);
    gfx->drawRect(BX - 1, BY - 1, BW + 2, BH + 2, COL_DIM);
    shown = true;
    lastPct = -1;
  }
  hideAt = millis() + 1100;
  if (pct == lastPct) return;
  lastPct = pct;
  uint16_t col = pct == 0 ? 0xF800 : pct > 85 ? 0xFD20 : 0x07E0;
  int fill = BH * pct / 100;
  gfx->fillRect(BX, BY, BW, BH - fill, 0x0000);
  gfx->fillRect(BX, BY + BH - fill, BW, fill, col);
  gfx->fillRect(PX + 4, PY + 8, PW - 8, 20, 0x2104);
  speaker(PX + 11, PY + 10, COL_TEXT, pct == 0);
  gfx->setFont((const GFXfont *)nullptr);
  gfx->setTextSize(1);
  gfx->setTextColor(COL_TEXT, 0x2104);
  gfx->fillRect(PX + 2, PY + PH - 24, PW - 4, 12, 0x2104);
  char b[8];
  snprintf(b, sizeof b, "%d%%", pct);
  gfx->setCursor(PX + (PW - strlen(b) * 6) / 2, PY + PH - 22);
  gfx->print(b);
}

bool fxOverlayActive() { return shown; }

bool fxOverlayTick() {
  if (!shown || (int32_t)(millis() - hideAt) < 0) return false;
  shown = false;
  return true;
}
