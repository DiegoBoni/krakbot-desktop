#pragma once
// Clock + outdoor weather screen.
void clockEnter();
void clockTick(bool visible);
void clockInit();            // load saved settings
void clockBackground();      // hourly chime; runs in every pose
void clockToggleChime();
