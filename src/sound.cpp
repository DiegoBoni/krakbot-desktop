#include "sound.h"
#include <Arduino.h>
#include <Wire.h>
#include <ESP_I2S.h>
#include "es8311.h"
#include "es7210.h"

#define PA_CTRL 7
#define I2S_MCLK 8
#define I2S_BCLK 9
#define I2S_DOUT 12
#define I2S_DIN 11
#define MIC_GAIN ES7210_MIC_GAIN_24DB
#define I2S_LRC 10
#define RATE 16000
#define VOLUME 70           // codec volume 0-100
#define AMPLITUDE 9000      // square-wave peak, of 32767

static I2SClass i2s;
static QueueHandle_t q;
static int amp = AMPLITUDE;
static volatile bool streaming = false;

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
      if (!streaming) { digitalWrite(PA_CTRL, LOW); paOn = false; }   // idle: power the amp down
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

  es7210_dev_handle_t adc = nullptr;
  es7210_i2c_config_t adcI2c = {};
  adcI2c.i2c_addr = ES7210_ADDRRES_00;
  es7210_codec_config_t adcCfg = {};
  adcCfg.sample_rate_hz = RATE;
  adcCfg.mclk_ratio = 256;
  adcCfg.i2s_format = ES7210_I2S_FMT_I2S;
  adcCfg.bit_width = ES7210_I2S_BITS_16B;
  adcCfg.mic_bias = ES7210_MIC_BIAS_2V87;
  adcCfg.mic_gain = MIC_GAIN;
  if (es7210_new_codec(&adcI2c, &adc) == ESP_OK) es7210_config_codec(adc, &adcCfg);

  i2s.setPins(I2S_BCLK, I2S_LRC, I2S_DOUT, I2S_DIN, I2S_MCLK);
  i2s.setTimeout(500);
  i2s.begin(I2S_MODE_STD, RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH);
  q = xQueueCreate(8, sizeof(Sound));
  xTaskCreate(soundTask, "sound", 4096, nullptr, 2, nullptr);
}

void soundPlay(Sound s) { xQueueSend(q, &s, 0); }

size_t micRead(int16_t *stereo, size_t frames) {
  return i2s.readBytes((char *)stereo, frames * 4) / 4;
}

void streamBegin() { streaming = true; digitalWrite(PA_CTRL, HIGH); delay(20); }

void streamWrite(const int16_t *mono, size_t samples) {
  static int16_t buf[2 * 512];
  while (samples) {
    size_t c = min(samples, (size_t)512);
    for (size_t i = 0; i < c; i++) buf[2 * i] = buf[2 * i + 1] = mono[i];
    i2s.write((uint8_t *)buf, c * 4);
    mono += c;
    samples -= c;
  }
}

void streamEnd() {
  int16_t z[2 * 256] = {0};
  i2s.write((uint8_t *)z, sizeof(z));   // flush the tail with silence
  streaming = false;
}
