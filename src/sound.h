#pragma once
#include <Arduino.h>
enum Sound { SND_CLICK, SND_START, SND_PAUSE, SND_ALARM, SND_BREAK_OVER, SND_HOUR };
void soundInit();             // call after Wire.begin; codec + I2S + playback task
void soundPlay(Sound s);      // non-blocking, queued

// Microphone (ES7210) and raw playback on the same I2S bus, used by the voice agent.
size_t micRead(int16_t *stereo, size_t frames);          // blocking; returns frames read (L,R interleaved)
void streamBegin();                                      // powers the amp and keeps it on
void streamWrite(const int16_t *mono, size_t samples);   // 16 kHz mono, blocks until queued
void streamEnd();
