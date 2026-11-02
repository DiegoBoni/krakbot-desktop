#pragma once
// Physical buttons on top of the board: BOOT (GPIO0), PLUS (GPIO4), PWR (GPIO5). Active low.
enum Btn { BTN_BOOT, BTN_PLUS, BTN_PWR, BTN_COUNT };
enum BtnEvent { EV_NONE, EV_CLICK, EV_LONG };
void buttonsInit();
BtnEvent buttonsPoll(Btn b);   // CLICK on short release, LONG once after ~0.7 s held
