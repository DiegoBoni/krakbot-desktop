#include "buttons.h"
#include <Arduino.h>

static const uint8_t PIN[BTN_COUNT] = {0, 4, 5};
static bool down[BTN_COUNT], longSent[BTN_COUNT];
static uint32_t since[BTN_COUNT];

void buttonsInit() {
  for (int i = 0; i < BTN_COUNT; i++) pinMode(PIN[i], INPUT_PULLUP);
}

BtnEvent buttonsPoll(Btn b) {
  bool pressed = digitalRead(PIN[b]) == LOW;
  uint32_t now = millis();
  if (pressed && !down[b]) { down[b] = true; longSent[b] = false; since[b] = now; return EV_NONE; }
  if (pressed && down[b] && !longSent[b] && now - since[b] >= 700) { longSent[b] = true; return EV_LONG; }
  if (!pressed && down[b]) {
    down[b] = false;
    if (!longSent[b] && now - since[b] >= 30) return EV_CLICK;   // 30 ms debounce
  }
  return EV_NONE;
}

bool buttonsHeld(Btn b) { return digitalRead(PIN[b]) == LOW; }
