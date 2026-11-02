#pragma once
#include <Arduino.h>
#include <U8g2lib.h>
#include <Arduino_GFX_Library.h>

extern Arduino_GFX *gfx;

// Palette shared by all apps (8-bit look).
#define COL_BG    0x0000
#define COL_TEXT  0xFFFF
#define COL_DIM   0x4208
