#pragma once
void backlightInit();               // PWM on the backlight pin, restores the saved level
void backlightCycle();              // one key sweeps up to max, then back down to min
void backlightBlank(bool blank);    // temporary off/on for alert flashes
void backlightSleep(bool sleep);        // screen-off mode: stays dark until woken
