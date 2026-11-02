#include "sound.h"
#include <Arduino.h>
#include <Wire.h>
#include <ESP_I2S.h>
#include "es8311.h"

#define PA_CTRL 7
#define I2S_MCLK 8
#define I2S_BCLK 9
#define I2S_DOUT 12
#define I2S_LRC 10
#define RATE 16000
#define VOLUME 70           // codec volume 0-100
#define AMPLITUDE 9000      // square-wave peak, of 32767

static I2SClass i2s;
static QueueHandle_t q;
static int amp = AMPLITUDE;

struct Note { uint16_t hz, ms; };   // hz 0 = silence

static void beepNote(uint16_t hz, uint16_t ms) {
  int n = RATE * ms / 1000, period = hz ? RATE / hz : 0;
  static int16_t buf[2 * RATE / 4];  // up to 250 ms stereo per chunk
  while (n > 0) {
    int c = min(n, RATE / 4);
    for (int i = 0; i < c; i++) {
      int16_t v = 0;
      if (period) {
        v = ((i % period) < period / 2) ? amp : -amp;  // 8-bit style square
        int tail = c - i;                                          // short fade-out avoids clicks
        if (n - i < 160) v = v * (n - i) / 160;
        (void)tail;
      }
      buf[2 * i] = buf[2 * i + 1] = v;
    }
    i2s.write((uint8_t *)buf, c * 4);
    n -= c;
  }
}

static void play(const Note *s, int len) {
  for (int i = 0; i < len; i++) beepNote(s[i].hz, s[i].ms);
}

static void soundTask(void *) {
  Sound s;
  bool paOn = false;
  for (;;) {
    if (xQueueReceive(q, &s, paOn ? pdMS_TO_TICKS(2000) : portMAX_DELAY) != pdTRUE) {
      digitalWrite(PA_CTRL, LOW);   // idle: power the amp down
      paOn = false;
      continue;
    }
    if (!paOn) { digitalWrite(PA_CTRL, HIGH); paOn = true; delay(20); }
    switch (s) {
      case SND_CLICK: { Note n[] = {{1900, 18}}; play(n, 1); break; }
      case SND_START: { Note n[] = {{660, 60}, {990, 90}}; play(n, 2); break; }
      case SND_PAUSE: { Note n[] = {{990, 60}, {660, 90}}; play(n, 2); break; }
      case SND_ALARM: {   // victory arpeggio, twice
        Note n[] = {{523, 110}, {659, 110}, {784, 110}, {1046, 260}, {0, 120},
                    {523, 110}, {659, 110}, {784, 110}, {1046, 420}};
        play(n, 9); break; }
      case SND_HOUR: {   // delicate two-note chime, quieter than the rest
        amp = 3200;
        Note n[] = {{1319, 70}, {0, 40}, {1976, 150}};
        play(n, 3);
        amp = AMPLITUDE;
        break; }
      case SND_BREAK_OVER: { Note n[] = {{784, 110}, {659, 110}, {523, 260}, {0, 80}, {784, 110}, {523, 300}}; play(n, 6); break; }
    }
    beepNote(0, 30);
  }
}

void soundInit() {
  pinMode(PA_CTRL, OUTPUT);
  digitalWrite(PA_CTRL, HIGH);
  es8311_handle_t es = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
  const es8311_clock_config_t clk = {false, false, true, RATE * 256, RATE};
  es8311_init(es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
  es8311_sample_frequency_config(es, RATE * 256, RATE);
  es8311_voice_volume_set(es, VOLUME, NULL);
  es8311_microphone_config(es, false);

  i2s.setPins(I2S_BCLK, I2S_LRC, I2S_DOUT, -1, I2S_MCLK);
  i2s.begin(I2S_MODE_STD, RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH);
  q = xQueueCreate(8, sizeof(Sound));
  xTaskCreate(soundTask, "sound", 4096, nullptr, 2, nullptr);
}

void soundPlay(Sound s) { xQueueSend(q, &s, 0); }
