#include "agent.h"
#include "hw.h"
#include "net.h"
#include "sound.h"
#include "secrets.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "avatar_data.h"

#ifndef GROQ_API_KEY
#define GROQ_API_KEY ""
#endif
#ifndef OPENAI_API_KEY
#define OPENAI_API_KEY ""
#endif
#ifndef N8N_WEBHOOK_URL
#define N8N_WEBHOOK_URL ""
#endif
#ifndef N8N_WEBHOOK_URL_FRIDA
#define N8N_WEBHOOK_URL_FRIDA ""
#endif

#define STT_URL "https://api.groq.com/openai/v1/audio/transcriptions"
#define STT_MODEL "whisper-large-v3-turbo"
#define TTS_URL "https://api.openai.com/v1/audio/speech"
#define TTS_MODEL "gpt-4o-mini-tts"
#define TTS_GAIN 0.4f      // the speaker amplifier distorts near full scale
#define ROBOT_HZ 80.0f

#define RATE 16000
#define MAX_REC_S 15
#define MIN_REC_FRAMES (RATE / 2)
#define SKIP_FRAMES (RATE / 12)          // drop the key-click at the start
#define RADIO_IDLE_MS 60000UL
#define SLEEP_AFTER_MS 45000UL

extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

// Each skin is a look plus a voice. Skin 0 is the boyish robot octopus, skin 1 is Frida.
struct Skin {
  const char *name, *voice, *style;
  float robotMix;      // ring-modulation depth (0 = clean voice)
  int crush;           // low bits dropped for 8-bit grit
  uint16_t accent;
};
static const Skin SKINS[] = {
  {"KRAKEN", "echo",
   "Sos un pulpito robot varón de dibujos animados: voz de nene, aguda, chistosa y juguetona, con un tono levemente metálico y robótico, pausas cortas y entusiasmo exagerado, en español rioplatense.",
   0.4f, 6, 0xE28A},
  {"FRIDA", "nova",
   "Hablá en español rioplatense con voz de chica joven, simpática, pícara y coqueta, con una sonrisita traviesa, tono canchero, cálido y juguetón.",
   0.0f, 0, 0xFB56},
};
#define SKIN_COUNT 2
static volatile int skinIdx = 0;

// Each character can have its own n8n flow (and so its own personality); Frida falls back to the main one.
static const char *hookUrl() {
  return (skinIdx == 1 && strlen(N8N_WEBHOOK_URL_FRIDA)) ? N8N_WEBHOOK_URL_FRIDA : N8N_WEBHOOK_URL;
}

enum AgState { AG_IDLE, AG_CONNECTING, AG_LISTENING, AG_TRANSCRIBING, AG_THINKING, AG_SPEAKING, AG_ERROR, AG_SLEEP, AG_HAPPY };

static volatile AgState state = AG_IDLE;
static volatile bool active = false, held = false, abortReq = false;
static volatile int level = 0;
static volatile uint32_t lastActivity = 0, speakStart = 0, pokeUntil = 0;
static bool histMode = false;
static uint32_t histVerSeen = ~0u;
static int histIdx = 0;
static TaskHandle_t task;
static bool radioOn = false;
static uint32_t lastUse = 0;

enum { B_ENTER = 1, B_REC = 2 };

// ---- text shown under the kraken (written by the worker, read by the UI) -----------------------

static char shown[512];
static uint16_t shownCol = COL_TEXT;
static volatile uint32_t shownVer = 0;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// The 8-bit font only has plain ASCII, so fold accents and typographic marks.
static void foldAscii(const char *in, char *out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 1 < cap; i++) {
    uint8_t c = in[i];
    char r = 0;
    if (c < 0x80) r = c;
    else if (c == 0xC3 && in[i + 1]) {
      uint8_t d = in[++i];
      const char *from = "\xA1\xA9\xAD\xB3\xBA\xB1\xBC\x81\x89\x8D\x93\x9A\x91\x9C";
      // á é í ó ú ñ ü  Á É Í Ó Ú Ñ Ü
      const char *p = strchr(from, d);
      r = p ? "aeiounuAEIOUNU"[p - from] : '?';
    } else if (c == 0xC2 && in[i + 1]) {
      uint8_t d = in[++i];
      r = d == 0xBF ? '?' : d == 0xA1 ? '!' : ' ';
    } else if (c == 0xE2 && in[i + 1] && in[i + 2]) {
      uint8_t d = in[i + 1], e = in[i + 2];
      i += 2;
      if (d == 0x80 && (e == 0x9C || e == 0x9D)) r = '"';
      else if (d == 0x80 && (e == 0x98 || e == 0x99)) r = '\'';
      else if (d == 0x80 && (e == 0x93 || e == 0x94)) r = '-';
      else if (d == 0x80 && e == 0xA6) { if (o + 3 < cap) { out[o++] = '.'; out[o++] = '.'; } r = '.'; }
      else r = ' ';
    } else if (c >= 0xC0) {   // other multi-byte sequences: skip the continuation bytes
      while (((uint8_t)in[i + 1] & 0xC0) == 0x80) i++;
      r = ' ';
    } else continue;
    if (r) out[o++] = r;
  }
  out[o] = 0;
}

static void showText(const char *s, uint16_t col) {
  char tmp[sizeof(shown)];
  foldAscii(s, tmp, sizeof(tmp));
  portENTER_CRITICAL(&mux);
  strlcpy(shown, tmp, sizeof(shown));
  shownCol = col;
  shownVer = shownVer + 1;
  portEXIT_CRITICAL(&mux);
}

struct Turn { char q[116]; char a[272]; };
#define HIST_N 8
static Turn hist[HIST_N];
static int histCount = 0, histHead = 0;   // head = next slot to write
static volatile uint32_t histVer = 0;

static void pushHistory(const String &q, const String &a) {
  Turn t;
  foldAscii(q.c_str(), t.q, sizeof(t.q));
  foldAscii(a.c_str(), t.a, sizeof(t.a));
  portENTER_CRITICAL(&mux);
  hist[histHead] = t;
  histHead = (histHead + 1) % HIST_N;
  histCount = min(histCount + 1, HIST_N);
  histVer = histVer + 1;
  portEXIT_CRITICAL(&mux);
}

static void setState(AgState s) {
  state = s;
  lastActivity = millis();
  if (s == AG_SPEAKING) speakStart = millis();
}

static void fail(const char *msg) {
  if (Serial) Serial.printf("agent: %s\n", msg);
  showText(msg, 0xF800);
  setState(AG_ERROR);
  for (int i = 0; i < 30 && !abortReq; i++) delay(100);
  setState(AG_IDLE);
}

// ---- network helpers ---------------------------------------------------------------------------

static void tlsSetup(WiFiClientSecure &c) {
  c.setCACertBundle(rootca_crt_bundle_start, rootca_crt_bundle_end - rootca_crt_bundle_start);
}

static bool ensureRadio() {
  if (radioOn) return true;
  setState(AG_CONNECTING);
  radioOn = netAcquire(25000);
  lastUse = millis();
  if (!radioOn) { fail("SIN WIFI"); return false; }
  return true;
}

static void dropRadio() {
  if (!radioOn) return;
  netRelease();
  radioOn = false;
}

static void putLE32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }

static String transcribe(const int16_t *pcm, size_t n) {
  static const char *B = "----krakbot7e3a91";
  String head;
  const char *fields[][2] = {{"model", STT_MODEL}, {"language", "es"}, {"temperature", "0"}, {"response_format", "json"}};
  for (auto &f : fields)
    head += String("--") + B + "\r\nContent-Disposition: form-data; name=\"" + f[0] + "\"\r\n\r\n" + f[1] + "\r\n";
  head += String("--") + B + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
  String tail = String("\r\n--") + B + "--\r\n";

  size_t dataBytes = n * 2, total = head.length() + 44 + dataBytes + tail.length();
  uint8_t *body = (uint8_t *)ps_malloc(total);
  if (!body) return "";
  uint8_t *p = body;
  memcpy(p, head.c_str(), head.length()); p += head.length();
  memcpy(p, "RIFF", 4); putLE32(p + 4, 36 + dataBytes); memcpy(p + 8, "WAVEfmt ", 8);
  putLE32(p + 16, 16); p[20] = 1; p[21] = 0; p[22] = 1; p[23] = 0;
  putLE32(p + 24, RATE); putLE32(p + 28, RATE * 2); p[32] = 2; p[33] = 0; p[34] = 16; p[35] = 0;
  memcpy(p + 36, "data", 4); putLE32(p + 40, dataBytes);
  p += 44;
  memcpy(p, pcm, dataBytes); p += dataBytes;
  memcpy(p, tail.c_str(), tail.length());

  WiFiClientSecure client;
  tlsSetup(client);
  HTTPClient http;
  http.setTimeout(25000);
  String text;
  if (http.begin(client, STT_URL)) {
    http.addHeader("Authorization", String("Bearer ") + GROQ_API_KEY);
    http.addHeader("Content-Type", String("multipart/form-data; boundary=") + B);
    int code = http.POST(body, total);
    String resp = http.getString();
    if (Serial) Serial.printf("stt %d %s\n", code, resp.substring(0, 160).c_str());
    if (code == 200) {
      JsonDocument doc;
      if (!deserializeJson(doc, resp)) text = String(doc["text"] | "");
    }
    http.end();
  }
  free(body);
  text.trim();
  return text;
}

static String askAgent(const String &msg) {
  WiFiClientSecure client;
  tlsSetup(client);
  HTTPClient http;
  http.setTimeout(60000);
  String out;
  if (!http.begin(client, hookUrl())) return "";
  http.addHeader("Content-Type", "application/json");
  JsonDocument doc;
  doc["message"] = msg;
  String body;
  serializeJson(doc, body);
  int code = http.POST(body);
  String resp = http.getString();
  if (Serial) Serial.printf("n8n %d %s\n", code, resp.substring(0, 200).c_str());
  if (code == 200) {
    JsonDocument r;
    if (!deserializeJson(r, resp)) {
      for (const char *k : {"text", "output", "response", "message", "reply"})
        if (r[k].is<const char *>()) { out = r[k].as<String>(); break; }
    } else out = resp;   // plain-text webhook reply
  }
  http.end();
  out.trim();
  return out;
}

// Speech is read aloud without the *stage directions*.
static String spokenText(const String &t) {
  String o;
  bool in = false;
  for (size_t i = 0; i < t.length(); i++) {
    char c = t[i];
    if (c == '*') { in = !in; continue; }
    if (!in) o += c;
  }
  o.trim();
  return o;
}

// Collects the HTTP body (HTTPClient already strips any chunked framing) into PSRAM.
struct PcmSink : public Stream {
  uint8_t *buf;
  size_t cap, len = 0;
  PcmSink(uint8_t *b, size_t c) : buf(b), cap(c) {}
  size_t write(const uint8_t *d, size_t n) override {
    size_t c = min(n, cap - len);
    memcpy(buf + len, d, c);
    len += c;
    return n;
  }
  size_t write(uint8_t b) override { return write(&b, 1); }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
};

// The whole reply is downloaded first and played afterwards: no network jitter in the audio
// (gaps sounded like crackle) and the Wi-Fi radio is quiet while the amplifier is running.
static bool speak(const String &text) {
  String say = spokenText(text);
  if (say.isEmpty()) return true;
  const size_t CAP = 24000UL * 2 * 40;   // up to 40 s of 24 kHz mono
  static uint8_t *pcm;
  if (!pcm) pcm = (uint8_t *)ps_malloc(CAP);
  if (!pcm) return false;

  WiFiClientSecure client;
  tlsSetup(client);
  HTTPClient http;
  http.setTimeout(20000);
  if (!http.begin(client, TTS_URL)) return false;
  http.addHeader("Authorization", String("Bearer ") + OPENAI_API_KEY);
  http.addHeader("Content-Type", "application/json");
  JsonDocument doc;
  doc["model"] = TTS_MODEL;
  doc["input"] = say;
  const Skin &sk = SKINS[skinIdx];
  doc["voice"] = sk.voice;
  doc["instructions"] = sk.style;
  doc["response_format"] = "pcm";   // 24 kHz, 16-bit, little-endian, mono
  String body;
  serializeJson(doc, body);
  int code = http.POST(body);
  if (code != 200) {
    if (Serial) Serial.printf("tts %d %s\n", code, http.getString().substring(0, 160).c_str());
    http.end();
    return false;
  }
  PcmSink sink(pcm, CAP);
  http.writeToStream(&sink);
  http.end();
  size_t n = sink.len / 2;
  if (Serial) Serial.printf("tts %u samples\n", (unsigned)n);
  if (abortReq || n < 480) return !abortReq ? false : true;

  setState(AG_SPEAKING);
  const int16_t *in = (const int16_t *)pcm;
  const uint32_t total = (uint32_t)((n - 1) * 2 / 3);   // 24 kHz -> 16 kHz
  const int FADE = 240;
  int16_t out[512];
  int fill = 0;
  streamBegin();
  for (uint32_t k = 0; k < total && !abortReq; k++) {
    uint32_t pos2 = 3 * k, i = pos2 >> 1;   // exact integer phase, no float drift
    float v = in[i] + (in[i + 1] - in[i]) * ((pos2 & 1) ? 0.5f : 0.0f);
    if (sk.robotMix > 0) v *= (1.0f - sk.robotMix) + sk.robotMix * sinf(6.2831853f * ROBOT_HZ * (float)(k % RATE) / RATE);
    int q = ((int)v >> sk.crush) << sk.crush;
    float g = TTS_GAIN;
    if (k < FADE) g *= (float)k / FADE;                     // fade in/out so the amplifier never pops
    else if (total - k < FADE) g *= (float)(total - k) / FADE;
    out[fill++] = (int16_t)constrain((int)(q * g), -32768, 32767);
    if (fill == 512) { streamWrite(out, fill); fill = 0; }
  }
  if (fill && !abortReq) streamWrite(out, fill);
  streamEnd();
  return true;
}

// ---- recording ---------------------------------------------------------------------------------

static int16_t *rec;
static size_t recSamples;

// Fills rec with a mono, normalised take. false = nothing usable (error already shown).
static bool record() {
  const size_t maxFrames = (size_t)RATE * MAX_REC_S;
  if (!rec) rec = (int16_t *)ps_malloc(maxFrames * 4);
  if (!rec) { fail("SIN MEMORIA"); return false; }
  setState(AG_LISTENING);
  showText("", COL_TEXT);
  size_t frames = 0;
  int dry = 0;
  while (held && !abortReq && frames + 256 <= maxFrames) {
    size_t n = micRead(rec + frames * 2, 256);
    if (n == 0) { if (++dry > 6) { fail("MIC SIN DATOS"); return false; } continue; }
    dry = 0;
    int peak = 0;
    for (size_t i = 0; i < n * 2; i += 2) peak = max(peak, abs((int)rec[frames * 2 + i]));
    level = max(peak * 100 / 12000, (int)level * 7 / 10);
    frames += n;
  }
  level = 0;
  if (abortReq) return false;
  if (frames < MIN_REC_FRAMES) { fail("MUY CORTO"); return false; }

  // The two mics land in L and R; keep the louder one, remove DC, normalise.
  uint64_t e[2] = {0, 0};
  for (size_t i = SKIP_FRAMES; i < frames; i++) for (int c = 0; c < 2; c++) e[c] += (uint32_t)abs((int)rec[2 * i + c]);
  int ch = e[1] > e[0];
  size_t n = frames - SKIP_FRAMES;
  int64_t sum = 0;
  for (size_t i = 0; i < n; i++) sum += rec[2 * (i + SKIP_FRAMES) + ch];
  int dc = sum / (int64_t)n, peak = 1;
  for (size_t i = 0; i < n; i++) {
    int v = rec[2 * (i + SKIP_FRAMES) + ch] - dc;
    rec[i] = (int16_t)constrain(v, -32768, 32767);
    peak = max(peak, abs(v));
  }
  if (Serial) Serial.printf("rec %u samples ch=%d peak=%d\n", (unsigned)n, ch, peak);
  if (peak < 150) { fail("SIN AUDIO"); return false; }
  float gain = min(8.0f, 22000.0f / peak);
  for (size_t i = 0; i < n; i++) rec[i] = (int16_t)constrain((int)(rec[i] * gain), -32768, 32767);
  recSamples = n;
  return true;
}

static void runTurn() {
  abortReq = false;
  if (!record()) return;
  if (!ensureRadio()) return;
  lastUse = millis();
  if (abortReq) { setState(AG_IDLE); return; }

  setState(AG_TRANSCRIBING);
  String q = transcribe(rec, recSamples);
  if (abortReq) { setState(AG_IDLE); return; }
  if (q.length() < 2) { fail("NO TE ENTENDI"); return; }

  showText(q.c_str(), COL_DIM + 0x2104);
  setState(AG_THINKING);
  String a = askAgent(q);
  if (abortReq) { setState(AG_IDLE); return; }
  if (a.isEmpty()) { fail("SIN RESPUESTA"); return; }

  pushHistory(q, a);
  showText(a.c_str(), COL_TEXT);
  bool ok = speak(a);
  lastUse = millis();
  if (!ok && !abortReq) { setState(AG_IDLE); if (Serial) Serial.println("agent: tts failed"); return; }
  setState(AG_IDLE);
}

static void agentTask(void *) {
  for (;;) {
    uint32_t bits = 0;
    xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(1000));
    if (!active) { dropRadio(); if (state != AG_IDLE) state = AG_IDLE; continue; }
    if (bits & B_ENTER) {
      abortReq = false;
      if (ensureRadio()) setState(AG_IDLE);
    }
    if (bits & B_REC) runTurn();
    if (radioOn && state == AG_IDLE && millis() - lastUse > RADIO_IDLE_MS) dropRadio();
  }
}

// ---- UI ----------------------------------------------------------------------------------------

#define PIX 16
#define KX 24
#define KY 4
#define CANVAS_H 140
#define CORAL 0xE28A
#define YELLOW 0xFFE0

static Arduino_Canvas *cv;
static int offX, offY;

static void blk(int gx, int gy, int gw, int gh, uint16_t c) {
  cv->fillRect(KX + offX + gx * PIX, KY + offY + gy * PIX, gw * PIX, gh * PIX, c);
}

static uint32_t frameMs(AgState s) {
  if (skinIdx == 1) {   // Frida: blink-friendly timing
    switch (s) {
      case AG_LISTENING: return 130;
      case AG_SPEAKING: return 130;
      case AG_HAPPY: return 110;
      case AG_SLEEP: return 700;
      case AG_ERROR: return 120;
      case AG_IDLE: return 300;
      default: return 200;
    }
  }
  switch (s) {
    case AG_THINKING: return 180;
    case AG_CONNECTING: return 180;
    case AG_TRANSCRIBING: return 180;
    case AG_SPEAKING: return 130;
    case AG_LISTENING: return 240;
    case AG_SLEEP: return 900;
    case AG_HAPPY: return 110;
    case AG_ERROR: return 120;
    default: return 500;
  }
}

static void drawKraken(AgState s, int frame) {
  int sway = 0, bob = 0;
  switch (s) {
    case AG_HAPPY: sway = sinf(frame * 1.2f) * 10; bob = (frame % 2) ? -10 : 0; break;
    case AG_IDLE: sway = sinf(frame * 0.18f) * 14; bob = sinf(frame * 0.36f) * 3; break;
    case AG_LISTENING: sway = sinf(frame * 0.45f) * 9; bob = cosf(frame * 0.45f) * 5; break;
    case AG_SPEAKING: sway = sinf(frame * 0.6f) * 7; bob = (frame % 2) ? 3 : -3; break;
    case AG_THINKING: case AG_TRANSCRIBING: case AG_CONNECTING: sway = sinf(frame * 0.1f) * 5; break;
    default: break;
  }
  offX = sway;
  offY = bob + ((frame % 4 == 0) ? PIX / 4 : 0);
  bool sleeping = s == AG_SLEEP;
  bool blinking = sleeping || s == AG_HAPPY || (s == AG_IDLE && frame % 7 == 6);

  cv->fillRect(0, 0, 240, CANVAS_H, COL_BG);
  blk(3, 2, 6, 4, CORAL);
  blk(2, 3, 8, 2, CORAL);
  blk(4, 1, 4, 1, CORAL);

  for (int e = 0; e < 2; e++) {
    int gx = e ? 7 : 4;
    if (blinking) blk(gx, 3, 2, 1, COL_TEXT);
    else {
      blk(gx, 3, 2, 2, COL_TEXT);
      blk(gx + 1, 3 + ((frame / 2) % 2), 1, 1, 0x0000);
    }
  }
  if ((s == AG_SPEAKING || s == AG_HAPPY) && frame % 2 == 0) blk(5, 5, 2, 2, COL_TEXT);
  else blk(5, 5, 2, 1, COL_TEXT);

  int t = frame % 2;
  blk(2, 6, 1, 1, CORAL);
  blk(3, 7 - t, 1, 1, CORAL);
  blk(4, 6 + t, 1, 1, CORAL);
  blk(5, 7, 1, 1, CORAL);
  blk(6, 6 + t, 1, 1, CORAL);
  blk(7, 7 - t, 1, 1, CORAL);
  blk(8, 6, 1, 1, CORAL);
  blk(9, 7, 1, 1, CORAL);

  if (s == AG_THINKING || s == AG_TRANSCRIBING || s == AG_CONNECTING) {
    blk(4 + (frame % 3) * 2, 0, 1, 1, YELLOW);
    blk(4 + ((frame + 1) % 3) * 2, 0, 1, 1, 0x8C40);
  }
  if (s == AG_LISTENING) {   // sound waves on both sides
    int w = frame % 2;
    blk(1 - w, 3, 1, 2, YELLOW);
    blk(10 + w, 3, 1, 2, YELLOW);
  }
  if (sleeping) {
    int z = (frame / 1) % 2;
    blk(10, 1 - z, 1, 1, 0x7BEF);
    blk(11, 0, 1, 1, 0x7BEF);
  }
  if (s == AG_HAPPY) {   // sparkles
    int w = frame % 2;
    blk(1 + w, 1, 1, 1, YELLOW);
    blk(10 - w, 1, 1, 1, YELLOW);
    blk(0 + w, 4, 1, 1, 0xFD20);
    blk(11 - w, 4, 1, 1, 0xFD20);
  }
  if (s == AG_ERROR) {
    blk(10, 0, 1, 2, 0xF800);
    blk(10, 3, 1, 1, 0xF800);
  }
  if (s == AG_LISTENING) {   // input level bar under the kraken
    cv->drawFastHLine(KX, CANVAS_H - 3, 192, COL_DIM);
    cv->fillRect(KX, CANVAS_H - 5, 192 * min((int)level, 100) / 100, 4, YELLOW);
  }
}


// ---- skin 1: Frida --------------------------------------------------------------------------
// 69x69 portrait (tools/make_avatar.py) shown at 2x. Eyes and mouth swap between prebuilt frames.

static const uint8_t HEART[6] = {0x1B, 0x3F, 0x3F, 0x1E, 0x0C, 0x04};   // 7 wide, bit 5 = left

static void bar(int x, int cy, int h, uint16_t c) { cv->fillRect(x, cy - h / 2, 4, h, c); }

static void drawAvatar(AgState s, int frame) {
  bool sleep = s == AG_SLEEP, err = s == AG_ERROR, happy = s == AG_HAPPY;
  bool eq = s == AG_LISTENING || s == AG_SPEAKING;
  bool busy = s == AG_THINKING || s == AG_TRANSCRIBING || s == AG_CONNECTING;
  bool cheeky = s == AG_IDLE && frame % 26 >= 21 && frame % 26 <= 23;   // an occasional wink
  int eye = sleep ? 1 : (happy || cheeky) ? 2 : ((!err && frame % 14 == 13) ? 1 : 0);
  static const uint8_t TALK[6] = {1, 2, 1, 0, 2, 1};
  int mouth = s == AG_SPEAKING ? TALK[frame % 6] : err ? 2 : happy ? 1 : 0;
  const uint8_t *fr = AV_FRAMES[eye * 3 + mouth];

  int bob = sleep ? ((frame % 6) < 3 ? 0 : 3) : happy ? ((frame % 2) ? -4 : 0) : eq ? ((frame % 2) ? 0 : 3) : ((frame % 10) < 5 ? 0 : 3);
  int shake = err ? ((frame % 2) ? 3 : -3) : 0;
  const int S = (CANVAS_H - 2) / AV_N, X0 = (240 - AV_N * S) / 2 + shake, Y0 = 1 + (bob > 0 ? bob : 0);

  uint16_t pal[sizeof(AV_PAL) / sizeof(AV_PAL[0])];
  for (size_t i = 0; i < sizeof(pal) / sizeof(pal[0]); i++) {
    uint16_t c = AV_PAL[i];
    if (sleep) {
      int r = (c >> 11) * 40 / 100, g = ((c >> 5) & 63) * 40 / 100, b = (c & 31) * 40 / 100;
      c = (r << 11) | (g << 5) | b;
    }
    pal[i] = c;
  }
  cv->fillRect(0, 0, 240, CANVAS_H, COL_BG);
  for (int y = 0; y < AV_N; y++) {
    int py = Y0 + y * S;
    if (py + S > CANVAS_H) break;
    for (int x = 0; x < AV_N;) {
      uint8_t c = fr[y * AV_N + x];
      if (c == AV_TRANSPARENT) { x++; continue; }
      int e = x + 1;
      while (e < AV_N && fr[y * AV_N + e] == c) e++;
      cv->fillRect(X0 + x * S, py, (e - x) * S, S, pal[c]);
      x = e;
    }
  }

  if (eq) {   // sound bars on both sides
    float lv = s == AG_LISTENING ? 0.15f + min((int)level, 100) / 100.0f * 0.85f : 0.4f + 0.6f * fabsf(sinf(frame * 0.9f));
    uint16_t c = s == AG_LISTENING ? YELLOW : 0xFB56;
    for (int i = 0; i < 3; i++) {
      int h = (int)((14 + i * 12) * lv * (0.6f + 0.4f * fabsf(sinf(frame * 0.8f + i * 1.3f))));
      bar(38 - i * 12, 70, max(h, 4), c);
      bar(198 + i * 12, 70, max(h, 4), c);
    }
  }
  if (busy) {   // thinking dots
    for (int i = 0; i < 3; i++) cv->fillRect(196 + i * 12, 22, 6, 6, i == frame % 3 ? YELLOW : COL_DIM);
  }
  if (happy) {   // hearts floating up
    for (int k = 0; k < 2; k++) {
      int hx = k ? 200 : 14, hy = 100 - ((frame * 5 + k * 9) % 70);
      for (int r = 0; r < 6; r++)
        for (int c = 0; c < 7; c++)
          if (HEART[r] & (0x40 >> c)) cv->fillRect(hx + c * 3, hy + r * 3, 3, 3, 0xF81F);
    }
  }
  if (sleep) {   // z z Z drifting up
    for (int k = 0; k < 3; k++) {
      int z = 2 + k, zx = 190 + k * 12, zy = 70 - k * 18 - ((frame + k * 2) % 4) * 2;
      cv->fillRect(zx, zy, z * 3, 2, 0x7BEF);
      cv->drawLine(zx + z * 3 - 1, zy + 2, zx, zy + z * 3, 0x7BEF);
      cv->fillRect(zx, zy + z * 3, z * 3, 2, 0x7BEF);
    }
  }
  if (err) {   // red exclamation mark
    cv->fillRect(206, 40, 6, 26, 0xF800);
    cv->fillRect(206, 72, 6, 6, 0xF800);
  }
}

static void textCentered(const char *t, int cx, int baseline, uint8_t size, uint16_t col) {
  gfx->setFont(u8g2_font_pressstart2p_8r);
  gfx->setTextSize(size);
  gfx->setTextColor(col);
  gfx->setCursor(cx - (int)strlen(t) * 4 * size, baseline);
  gfx->print(t);
  gfx->setTextSize(1);
}

static const char *labelFor(AgState s, uint16_t &col) {
  col = COL_TEXT;
  switch (s) {
    case AG_CONNECTING: col = 0x07FF; return "CONECTANDO";
    case AG_LISTENING: col = YELLOW; return "ESCUCHANDO";
    case AG_TRANSCRIBING: col = 0x07FF; return "TRANSCRIBIENDO";
    case AG_THINKING: col = 0xAB7F; return "PENSANDO";
    case AG_SPEAKING: col = SKINS[skinIdx].accent; return "HABLANDO";
    case AG_ERROR: col = 0xF800; return "ERROR";
    case AG_SLEEP: col = COL_DIM; return "ZZZ";
    case AG_HAPPY: col = YELLOW; return "HOLA!";
    default: col = 0x07E0; return "LISTO";
  }
}

static bool needRepaint = true;
#define LINE_CH 28
#define LINES_PER_PAGE 6
static char lines[24][LINE_CH + 1];
static int nLines;

static int wrapInto(char (*lines)[LINE_CH + 1], int maxLines, const char *t) {
  int nLines = 0;
  const char *p = t;
  while (*p && nLines < maxLines) {
    while (*p == ' ') p++;
    int len = strlen(p);
    int take = min(len, LINE_CH);
    if (len > LINE_CH) {
      int cut = take;
      while (cut > 0 && p[cut] != ' ') cut--;
      if (cut > 0) take = cut;
    }
    memcpy(lines[nLines], p, take);
    lines[nLines][take] = 0;
    nLines++;
    p += take;
  }
  return nLines;
}

static void wrapText(const char *t) { nLines = wrapInto(lines, 24, t); }

static void drawHistory() {
  Turn t;
  int total, n;
  portENTER_CRITICAL(&mux);
  total = histCount;
  n = (histHead - 1 - histIdx + 2 * HIST_N) % HIST_N;
  t = hist[n];
  portEXIT_CRITICAL(&mux);
  gfx->fillScreen(COL_BG);
  gfx->setFont(u8g2_font_pressstart2p_8r);
  gfx->setTextSize(1);
  char head[24];
  snprintf(head, sizeof(head), "HISTORIAL %d/%d", histIdx + 1, total);
  gfx->setTextColor(CORAL);
  gfx->setCursor(8, 16);
  gfx->print(head);
  gfx->drawFastHLine(0, 22, 240, COL_DIM);
  static char ql[6][LINE_CH + 1], al[12][LINE_CH + 1];
  int nq = wrapInto(ql, 5, t.q), na = wrapInto(al, 10, t.a);
  int y = 38;
  gfx->setTextColor(YELLOW);
  gfx->setCursor(8, y); gfx->print("TU:"); y += 11;
  gfx->setTextColor(0xBDF7);
  for (int i = 0; i < nq; i++, y += 11) { gfx->setCursor(8, y); gfx->print(ql[i]); }
  y += 6;
  gfx->setTextColor(CORAL);
  gfx->setCursor(8, y); gfx->print("KRAKBOT:"); y += 11;
  gfx->setTextColor(COL_TEXT);
  for (int i = 0; i < na; i++, y += 11) { gfx->setCursor(8, y); gfx->print(al[i]); }
  gfx->drawFastHLine(0, 220, 240, COL_DIM);
  gfx->setTextColor(COL_DIM);
  gfx->setCursor(8, 234);
  gfx->print("BOOT:ANTERIOR  +:VOLVER");
}

static void exitHistory() {
  histMode = false;
  gfx->fillScreen(COL_BG);
  needRepaint = true;
}


void agentEnter() {
  if (!cv) {
    cv = new Arduino_Canvas(240, CANVAS_H, gfx, 0, 0);
    cv->begin(GFX_SKIP_OUTPUT_BEGIN);
  }
  held = false;
  abortReq = false;
  active = true;
  histMode = false;
  needRepaint = true;
  lastActivity = millis();
  bool keys = strlen(GROQ_API_KEY) && strlen(OPENAI_API_KEY) && strlen(hookUrl());
  showText(keys ? "MANTEN [+] PARA HABLAR" : "FALTAN CLAVES EN SECRETS.H", keys ? COL_DIM : 0xF800);
  gfx->fillScreen(COL_BG);
  if (task) xTaskNotify(task, B_ENTER, eSetBits);
}

void agentRepaint() {
  if (!active) return;
  gfx->fillScreen(COL_BG);
  needRepaint = true;
  histVerSeen = ~0u;
}

void agentLeave() {
  active = false;
  held = false;
  abortReq = true;
  if (task) xTaskNotify(task, 0, eNoAction);
}

void agentPress() {
  lastActivity = millis();
  if (!active) return;
  if (histMode) exitHistory();
  if (state == AG_SPEAKING) { abortReq = true; return; }
  if (state != AG_IDLE && state != AG_ERROR && state != AG_SLEEP) return;
  if (!strlen(GROQ_API_KEY) || !strlen(OPENAI_API_KEY) || !strlen(hookUrl())) return;
  held = true;
  abortReq = false;
  state = AG_LISTENING;   // instant feedback; the worker takes over from here
  if (task) xTaskNotify(task, B_REC, eSetBits);
}

void agentRelease() { held = false; }

void agentBoot() {
  if (!active) return;
  lastActivity = millis();
  if (histMode) {
    if (++histIdx >= histCount) exitHistory(); else histVerSeen = ~0u;
    return;
  }
  if (state == AG_IDLE || state == AG_ERROR) {
    if (histCount == 0) { pokeUntil = millis() + 900; return; }
    histMode = true;
    histIdx = 0;
    histVerSeen = ~0u;
    return;
  }
  abortReq = true;   // busy: drop the current turn
}

void agentTouch() {
  if (!active) return;
  lastActivity = millis();
  if (histMode) { exitHistory(); return; }
  if (state == AG_IDLE || state == AG_ERROR) pokeUntil = millis() + 1400;
}

static void skinSave() {
  Preferences p;
  if (p.begin("orient", false)) { p.putUChar("skin", skinIdx); p.end(); }
}

void agentSkinNext() {
  if (!active || histMode || (state != AG_IDLE && state != AG_ERROR)) return;
  skinIdx = (skinIdx + 1) % SKIN_COUNT;
  skinSave();
  lastActivity = millis();
  pokeUntil = millis() + 1400;
  char msg[24];
  snprintf(msg, sizeof(msg), "SKIN: %s", SKINS[skinIdx].name);
  showText(msg, SKINS[skinIdx].accent);
  needRepaint = true;
  soundPlay(SND_START);
}

void agentInit() {
  Preferences p;
  if (p.begin("orient", true)) { skinIdx = p.getUChar("skin", 0) % SKIN_COUNT; p.end(); }
  xTaskCreate(agentTask, "agent", 16384, nullptr, 1, &task);
}

void agentTick(bool visible) {
  if (!visible || !active || !cv) return;
  uint32_t now = millis();
  if (histMode) {
    if (histVerSeen != 0u + (uint32_t)histIdx + 1000u * (uint32_t)histVer) { histVerSeen = (uint32_t)histIdx + 1000u * (uint32_t)histVer; drawHistory(); }
    return;
  }
  AgState s = state;
  if (s == AG_IDLE && now - lastActivity > SLEEP_AFTER_MS) s = AG_SLEEP;
  if ((s == AG_IDLE || s == AG_SLEEP) && now < pokeUntil) s = AG_HAPPY;

  static AgState cSt = AG_ERROR;
  static int cFrame = -1, cLevel = -1;
  static uint32_t cVer = 0;
  static int cPage = -1;
  static const char *cLabel = nullptr;
  static uint32_t lastDraw = 0;

  if (needRepaint) { cFrame = cLevel = cPage = -1; cLabel = nullptr; cVer = ~shownVer; needRepaint = false; }

  int frame = now / frameMs(s);
  int lv = s == AG_LISTENING ? level / 4 : 0;
  if ((frame != cFrame || s != cSt || lv != cLevel) && now - lastDraw >= 40) {
    lastDraw = now;
    cFrame = frame; cSt = s; cLevel = lv;
    if (skinIdx == 1) drawAvatar(s, frame); else drawKraken(s, frame);
    cv->flush();
  }

  uint16_t col;
  const char *lab = labelFor(s, col);
  if (lab != cLabel) {
    cLabel = lab;
    gfx->fillRect(0, CANVAS_H + 2, 240, 26, COL_BG);
    textCentered(lab, 120, CANVAS_H + 22, 2, col);
  }

  if (cVer != shownVer) {
    cVer = shownVer;
    char local[sizeof(shown)];
    portENTER_CRITICAL(&mux);
    strlcpy(local, shown, sizeof(local));
    portEXIT_CRITICAL(&mux);
    wrapText(local);
    cPage = -1;
  }
  int pages = max(1, (nLines + LINES_PER_PAGE - 1) / LINES_PER_PAGE);
  int page = s == AG_SPEAKING ? min(pages - 1, (int)((now - speakStart) / 9000)) : (cPage >= 0 ? cPage : 0);
  if (page != cPage) {
    cPage = page;
    gfx->fillRect(0, CANVAS_H + 30, 240, 240 - CANVAS_H - 30, COL_BG);
    gfx->setFont(u8g2_font_pressstart2p_8r);
    gfx->setTextSize(1);
    gfx->setTextColor(shownCol);
    for (int i = 0; i < LINES_PER_PAGE; i++) {
      int li = page * LINES_PER_PAGE + i;
      if (li >= nLines) break;
      int x = (nLines == 1 && strlen(lines[0]) < 24) ? 120 - (int)strlen(lines[li]) * 4 : 8;
      gfx->setCursor(x, CANVAS_H + 44 + i * 11);
      gfx->print(lines[li]);
    }
  }
}
