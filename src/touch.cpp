#include "touch.h"
#include <Wire.h>

#define TP_ADDR 0x15
#define TP_RST 47

void touchInit() {
  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, LOW);
  delay(10);
  digitalWrite(TP_RST, HIGH);
  delay(60);
  Wire.beginTransmission(TP_ADDR);  // 0xFE = 0xFF: disable auto standby so it never goes deaf
  Wire.write(0xFE);
  Wire.write(0xFF);
  Wire.endTransmission();
}

TouchEvent touchPoll() {
  static uint32_t lastPoll = 0;
  static bool down = false, dragging = false;
  static int x0, y0, cx, cy;
  TouchEvent ev = {TOUCH_NONE, cx, cy, 0, 0};
  uint32_t now = millis();
  if (now - lastPoll < 15) return ev;
  lastPoll = now;

  Wire.beginTransmission(TP_ADDR);
  Wire.write(0x01);
  if (Wire.endTransmission(false) != 0) return ev;
  if (Wire.requestFrom(TP_ADDR, 6) != 6) return ev;
  Wire.read();                       // gesture id
  uint8_t fingers = Wire.read();
  uint8_t xh = Wire.read(), xl = Wire.read(), yh = Wire.read(), yl = Wire.read();

  if (fingers == 0) {
    if (!down) return ev;
    down = false;
    ev.kind = dragging ? TOUCH_DRAG_END : TOUCH_TAP;
    ev.x = dragging ? cx : x0;
    ev.y = dragging ? cy : y0;
    ev.dx = cx - x0;
    ev.dy = cy - y0;
    dragging = false;
    return ev;
  }
  cx = ((xh & 0x0F) << 8) | xl;
  cy = ((yh & 0x0F) << 8) | yl;
  if (!down) { down = true; dragging = false; x0 = cx; y0 = cy; }
  ev.x = cx; ev.y = cy; ev.dx = cx - x0; ev.dy = cy - y0;
  if (!dragging && (abs(ev.dx) > 14 || abs(ev.dy) > 14)) dragging = true;
  if (dragging) ev.kind = TOUCH_DRAG;
  return ev;
}
