#include "pomodoro.h"
#include "hw.h"
#include "sound.h"
#include "brightness.h"

// Same visual language as the clock: title strip, big 3x5 pixel digits, outlined cells,
// thin segmented bar along the bottom edge.

enum Mode { FOCUS, SHORT_BREAK, LONG_BREAK };
enum Run { IDLE, RUNNING, PAUSED };

static const char *MODE_NAME[3] = {"FOCO", "DESCANSO", "PAUSA LARGA"};
static const int MIN_DUR[3] = {5, 1, 5}, MAX_DUR[3] = {90, 30, 60}, STEP[3] = {5, 1, 5};

static Mode mode = FOCUS;
static Run run = IDLE;
static int durMin[3] = {25, 5, 15};
static int doneCount = 0;           // focus blocks finished in this set of 4
static uint32_t endAt = 0;          // millis() when RUNNING ends
static uint32_t remainMs = 25UL * 60000;
static int shownSec = -1;
static char shownDigits[6] = "";    // per-digit cache: only changed digits are repainted

static const int CX[3] = {4, 84, 164}, CY = 168, CW = 72, CH = 52;

static uint16_t modeColor() {
  return mode == FOCUS ? gfx->color565(170, 110, 255)
       : mode == SHORT_BREAK ? gfx->color565(80, 220, 120)
       : gfx->color565(90, 200, 255);
}

// 3x5 pixel digits, one row per 3 bits.
static const uint8_t GLYPH[10][5] = {
  {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
  {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,1,1}, {7,5,7,5,7}, {7,5,7,1,7}};

// 7x7 pixel icons for the touch buttons.
static const uint8_t ICON_PLAY[7]  = {0x40, 0x60, 0x70, 0x78, 0x70, 0x60, 0x40};
static const uint8_t ICON_PAUSE[7] = {0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36};
static const uint8_t ICON_STOP[7]  = {0x00, 0x3E, 0x3E, 0x3E, 0x3E, 0x3E, 0x00};
static const uint8_t ICON_SKIP[7]  = {0x42, 0x62, 0x72, 0x7A, 0x72, 0x62, 0x42};

static void textCentered(const char *t, int cx, int baseline, uint8_t size, uint16_t col) {
  gfx->setFont(u8g2_font_pressstart2p_8r);
  gfx->setTextSize(size);
  gfx->setTextColor(col);
  gfx->setCursor(cx - (int)strlen(t) * 4 * size, baseline);
  gfx->print(t);
  gfx->setTextSize(1);
}

static void drawGlyph(int g, int x, int y, int s, uint16_t col) {
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 3; c++)
      if (GLYPH[g][r] & (4 >> c)) gfx->fillRect(x + c * s, y + r * s, s, s, col);
}

static void drawTime(int sec, bool force) {
  char buf[6];
  snprintf(buf, sizeof buf, "%02d:%02d", sec / 60, sec % 60);
  int x = 35;
  for (int i = 0; i < 5; i++) {
    bool colon = buf[i] == ':';
    if (force || buf[i] != shownDigits[i]) {
      if (colon) {
        gfx->fillRect(105, 62, 30, 50, COL_BG);
        gfx->fillRect(115, 72, 10, 10, modeColor());   // dots centered in the gap
        gfx->fillRect(115, 92, 10, 10, modeColor());
      } else {
        gfx->fillRect(x, 62, 30, 50, COL_BG);
        drawGlyph(buf[i] - '0', x, 62, 10, modeColor());
      }
    }
    x += (colon ? 10 : 30) + 10;
  }
  memcpy(shownDigits, buf, sizeof buf);
}

static void drawTitle() {
  gfx->fillRect(0, 0, 240, 37, COL_BG);
  textCentered(MODE_NAME[mode], 120, 28, 2, COL_TEXT);
}

static void drawStatus() {
  gfx->fillRect(0, 118, 240, 40, COL_BG);
  const char *t = run == RUNNING ? "EN CURSO" : run == PAUSED ? "EN PAUSA" : "LISTO";
  textCentered(t, 120, 148, 2, COL_TEXT);
}

static void drawIcon(const uint8_t *rows, int x, int y, int s, uint16_t col) {
  for (int r = 0; r < 7; r++)
    for (int c = 0; c < 7; c++)
      if (rows[r] & (0x40 >> c)) gfx->fillRect(x + c * s, y + r * s, s, s, col);
}

// kind: text label, or an icon when icon != nullptr. The center cell is the main action.
static void drawCell(int i, const char *label, const uint8_t *icon) {
  int x = CX[i];
  uint16_t border = i == 1 ? modeColor() : COL_DIM;
  gfx->fillRect(x, CY, CW, CH, COL_BG);
  gfx->drawRect(x, CY, CW, CH, border);
  gfx->drawRect(x + 1, CY + 1, CW - 2, CH - 2, border);
  if (icon) drawIcon(icon, x + (CW - 28) / 2, CY + (CH - 28) / 2, 4, i == 1 ? modeColor() : COL_TEXT);
  else textCentered(label, x + CW / 2, CY + 38, 3, COL_TEXT);
}

static void drawButtons() {
  if (run == IDLE) {
    char a[6], b[6];
    snprintf(a, sizeof a, "-%d", STEP[mode]);
    snprintf(b, sizeof b, "+%d", STEP[mode]);
    drawCell(0, a, nullptr);
    drawCell(1, nullptr, ICON_PLAY);
    drawCell(2, b, nullptr);
  } else {
    drawCell(0, nullptr, ICON_STOP);
    drawCell(1, nullptr, run == RUNNING ? ICON_PAUSE : ICON_PLAY);
    drawCell(2, nullptr, ICON_SKIP);
  }
}

// Four thin segments along the bottom: focus blocks finished in this set.
static void drawBlocks() {
  for (int i = 0; i < 4; i++)
    gfx->fillRect(8 + i * 58, 228, 50, 6, i < doneCount ? gfx->color565(170, 110, 255) : COL_DIM);
}

static int remainingSec() {
  uint32_t ms = run == RUNNING ? (endAt > millis() ? endAt - millis() : 0) : remainMs;
  return (ms + 999) / 1000;
}

static void drawAll(bool clear) {
  if (clear) {
    gfx->fillScreen(COL_BG);
    gfx->drawFastHLine(0, 38, 240, COL_DIM);
  }
  drawTitle();
  shownSec = remainingSec();
  drawTime(shownSec, true);
  drawStatus();
  drawButtons();
  drawBlocks();
}

static void setMode(Mode m) {
  mode = m;
  run = IDLE;
  remainMs = durMin[mode] * 60000UL;
}

static void alertFlash() {
  for (int i = 0; i < 6; i++) {
    backlightBlank(true);
    delay(120);
    backlightBlank(false);
    delay(120);
  }
}

static void finishBlock(bool alarm) {
  if (alarm) {
    soundPlay(mode == FOCUS ? SND_ALARM : SND_BREAK_OVER);
    alertFlash();
  }
  if (mode == FOCUS) {
    doneCount++;
    if (doneCount >= 4) { doneCount = 0; setMode(LONG_BREAK); }
    else setMode(SHORT_BREAK);
  } else {
    setMode(FOCUS);
  }
}

void pomodoroEnter() { drawAll(true); }

void pomodoroTick(bool visible) {
  if (run == RUNNING && millis() >= endAt) {
    finishBlock(true);
    if (visible) drawAll(false);
    return;
  }
  if (!visible) return;
  int sec = remainingSec();
  if (sec != shownSec) {
    shownSec = sec;
    drawTime(sec, false);
  }
}

void pomodoroAction(PomoAction a) {
  switch (a) {
    case POMO_MAIN:
      if (run == RUNNING) { remainMs = endAt > millis() ? endAt - millis() : 0; run = PAUSED; soundPlay(SND_PAUSE); }
      else { endAt = millis() + remainMs; run = RUNNING; soundPlay(SND_START); }
      break;
    case POMO_RESET:
      setMode(mode); soundPlay(SND_CLICK);
      break;
    case POMO_NEXT:   // idle: cycle mode; otherwise skip the current block
      soundPlay(SND_CLICK);
      if (run == IDLE) setMode((Mode)((mode + 1) % 3)); else finishBlock(false);
      break;
    case POMO_UP:
    case POMO_DOWN:
      if (run != IDLE) return;
      durMin[mode] = constrain(durMin[mode] + (a == POMO_UP ? STEP[mode] : -STEP[mode]), MIN_DUR[mode], MAX_DUR[mode]);
      remainMs = durMin[mode] * 60000UL;
      soundPlay(SND_CLICK);
      break;
  }
  drawAll(false);
}

void pomodoroTouch(int x, int y) {
  if (y < 40) { pomodoroAction(POMO_NEXT); return; }
  if (y < CY - 8 || y > CY + CH + 8) return;
  int b = -1;
  for (int i = 0; i < 3; i++) if (x >= CX[i] - 4 && x < CX[i] + CW + 4) b = i;
  if (b < 0) return;
  if (b == 1) pomodoroAction(POMO_MAIN);
  else if (run == IDLE) pomodoroAction(b == 0 ? POMO_DOWN : POMO_UP);
  else pomodoroAction(b == 0 ? POMO_RESET : POMO_NEXT);
}
