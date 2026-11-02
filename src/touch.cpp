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

bool touchPressed(int &x, int &y) {
  static uint32_t lastPoll = 0, lastTap = 0;
  static bool down = false;
  uint32_t now = millis();
  if (now - lastPoll < 15) return false;
  lastPoll = now;

  Wire.beginTransmission(TP_ADDR);
  Wire.write(0x01);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(TP_ADDR, 6) != 6) return false;
  Wire.read();                       // gesture id
  uint8_t fingers = Wire.read();
  uint8_t xh = Wire.read(), xl = Wire.read(), yh = Wire.read(), yl = Wire.read();

  if (fingers == 0) { down = false; return false; }
  if (down || now - lastTap < 150) return false;
  down = true;
  lastTap = now;
  x = ((xh & 0x0F) << 8) | xl;
  y = ((yh & 0x0F) << 8) | yl;
  return true;
}
