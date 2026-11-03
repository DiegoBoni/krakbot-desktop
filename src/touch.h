#pragma once
// CST816 capacitive touch, shares the IMU I2C bus (SDA 42 / SCL 41).
void touchInit();

enum TouchKind { TOUCH_NONE, TOUCH_TAP, TOUCH_DRAG, TOUCH_DRAG_END };
struct TouchEvent {
  TouchKind kind;
  int x, y;      // current finger position, in the panel's native (rotation 0) frame
  int dx, dy;    // displacement since finger-down, same frame
};
// TAP fires when the finger lifts without having moved; DRAG repeats while it moves; DRAG_END on lift.
TouchEvent touchPoll();
