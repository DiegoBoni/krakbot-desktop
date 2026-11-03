#pragma once
// Screen effects shared by every app: pose-change transition, volume overlay, power-off.
enum FxApp { FX_POMODORO, FX_CLOCK, FX_AGENT };

void fxCloseOld();                          // CRT-style collapse of whatever is on screen (current rotation)
void fxSplash(FxApp app);                   // app icon + name wiping in (call after setRotation)
void fxPowerOff();                          // goodbye animation, then cut the battery latch (never returns)

// Volume overlay. Show while dragging; fxOverlayTick() reports true once, when it times out and the
// app underneath has to be repainted.
void fxVolumeShow(int percent);
bool fxOverlayActive();
bool fxOverlayTick();
