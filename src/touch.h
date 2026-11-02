#pragma once
// CST816 capacitive touch, shares the IMU I2C bus (SDA 42 / SCL 41).
void touchInit();
// True once per finger-down; x/y are in the panel's native (rotation 0) frame.
bool touchPressed(int &x, int &y);
