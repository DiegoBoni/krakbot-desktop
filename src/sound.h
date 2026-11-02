#pragma once
enum Sound { SND_CLICK, SND_START, SND_PAUSE, SND_ALARM, SND_BREAK_OVER, SND_HOUR };
void soundInit();             // call after Wire.begin; codec + I2S + playback task
void soundPlay(Sound s);      // non-blocking, queued
