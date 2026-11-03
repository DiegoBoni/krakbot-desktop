#include "agent.h"
#include "hw.h"
#include "net.h"
#include "sound.h"
#include "secrets.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#ifndef GROQ_API_KEY
#define GROQ_API_KEY ""
#endif
#ifndef OPENAI_API_KEY
#define OPENAI_API_KEY ""
#endif
#ifndef N8N_WEBHOOK_URL
#define N8N_WEBHOOK_URL ""
#endif

#define STT_URL "https://api.groq.com/openai/v1/audio/transcriptions"
#define STT_MODEL "whisper-large-v3-turbo"
#define TTS_URL "https://api.openai.com/v1/audio/speech"
#define TTS_MODEL "gpt-4o-mini-tts"
#define TTS_GAIN 0.8f
#define ROBOT_HZ 80.0f

#define RATE 16000
#define MAX_REC_S 15
#define MIN_REC_FRAMES (RATE / 2)
#define SKIP_FRAMES (RATE / 12)          // drop the key-click at the start
#define RADIO_IDLE_MS 60000UL
#define SLEEP_AFTER_MS 45000UL

extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

// Each skin is a look plus a voice. Skin 0 is the boyish robot octopus, skin 1 an elegant pixel orb.
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
  {"SOLARPUNK", "nova",
   "Hablá en español rioplatense con voz de mujer elegante, cálida y serena, pausada, refinada y con un toque de misterio.",
   0.0f, 0, 0xF81F},
};
#define SKIN_COUNT 2
static volatile int skinIdx = 0;

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
  if (!http.begin(client, N8N_WEBHOOK_URL)) return "";
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

struct Resampler {   // 24 kHz -> 16 kHz, linear interpolation, state kept across chunks
  float t = 0;
  long n = 0;
  int16_t prev = 0;
  int16_t out[512];
  int fill = 0;
  uint32_t outCount = 0;
  float mix = 0;
  int crush = 0;
  void flush() {
    if (fill) streamWrite(out, fill);
    fill = 0;
  }
  void push(int16_t x) {
    while (t < n) {
      float v = prev + (x - prev) * (t - (n - 1));
      if (mix > 0) v *= (1.0f - mix) + mix * sinf(6.2831853f * ROBOT_HZ * (float)(outCount++) / RATE);
      v = ((int)v >> crush << crush) * TTS_GAIN;
      out[fill++] = (int16_t)constrain((int)v, -32768, 32767);
      if (fill == 512) flush();
      t += 1.5f;
    }
    prev = x;
    n++;
  }
};

static bool speak(const String &text) {
  String say = spokenText(text);
  if (say.isEmpty()) return true;
  WiFiClientSecure client;
  tlsSetup(client);
  HTTPClient http;
  http.useHTTP10(true);   // no chunked framing, the PCM comes straight off the socket
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
  doc["response_format"] = "pcm";   // 24 kHz, 16-bit, mono
  String body;
  serializeJson(doc, body);
  int code = http.POST(body);
  if (code != 200) {
    if (Serial) Serial.printf("tts %d %s\n", code, http.getString().substring(0, 160).c_str());
    http.end();
    return false;
  }
  auto *s = http.getStreamPtr();
  static Resampler rs;
  rs = Resampler();
  rs.mix = sk.robotMix;
  rs.crush = sk.crush;
  streamBegin();
  uint8_t buf[2048];
  uint8_t odd = 0;
  bool haveOdd = false, started = false;
  size_t pre = 0;
  uint8_t *prebuf = (uint8_t *)ps_malloc(24000);   // half a second of head start against Wi-Fi jitter
  uint32_t lastData = millis();
  auto feed = [&](const uint8_t *d, size_t len) {
    for (size_t i = 0; i < len; i++) {
      if (!haveOdd) { odd = d[i]; haveOdd = true; }
      else { rs.push((int16_t)(odd | (d[i] << 8))); haveOdd = false; }
    }
  };
  while (!abortReq && (http.connected() || s->available()) && millis() - lastData < 8000) {
    int avail = s->available();
    if (avail <= 0) { delay(2); continue; }
    int got = s->readBytes(buf, min(avail, (int)sizeof(buf)));
    if (got <= 0) continue;
    lastData = millis();
    if (!started && prebuf && pre + got <= 24000) {
      memcpy(prebuf + pre, buf, got);
      pre += got;
      if (pre < 24000 - sizeof(buf)) continue;
    }
    if (!started) { started = true; if (pre) { feed(prebuf, pre); pre = 0; } }
    if (!abortReq) feed(buf, got);
  }
  if (!started && !abortReq && pre) feed(prebuf, pre);   // short reply: never reached the head start
  if (!abortReq) rs.flush();
  streamEnd();
  free(prebuf);
  http.end();
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
  setState(AG_SPEAKING);
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
  if (skinIdx == 1) {   // the orb animates faster
    switch (s) {
      case AG_LISTENING: return 110;
      case AG_SPEAKING: return 90;
      case AG_HAPPY: return 80;
      case AG_SLEEP: return 500;
      case AG_ERROR: return 140;
      case AG_IDLE: return 200;
      default: return 120;
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


// ---- skin 1: solarpunk landscape -------------------------------------------------------------------
// A low sun behind layered hills of pixel waves, with blooming flowers and drifting pollen.
// The hills swell with your voice while listening and with the reply while talking.

#define OP 6          // pixel size
#define OGX 40
#define OGY 23
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

static uint16_t dim565(uint16_t c, int pct) {
  int r = (c >> 11) * pct / 100, g = ((c >> 5) & 63) * pct / 100, b = (c & 31) * pct / 100;
  return (r << 11) | (g << 5) | b;
}

static void cell(int x, int y, uint16_t c) {
  if (x < 0 || x >= OGX || y < 0 || y >= OGY) return;
  cv->fillRect(x * OP, y * OP, OP, OP, c);
}

static void drawOrb(AgState s, int frame) {
  const float TAU = 6.28318f;
  bool sleep = s == AG_SLEEP, err = s == AG_ERROR, happy = s == AG_HAPPY;
  bool eq = s == AG_LISTENING || s == AG_SPEAKING;
  bool busy = s == AG_THINKING || s == AG_TRANSCRIBING || s == AG_CONNECTING;
  float spin = busy ? 0.30f : happy ? 0.40f : sleep ? 0.03f : eq ? 0.20f : 0.10f;
  float pulse = s == AG_SPEAKING ? 0.5f + 0.5f * fabsf(sinf(frame * 0.9f)) : 0.5f + 0.5f * sinf(frame * 0.35f);
  float amp = s == AG_LISTENING ? 0.15f + min((int)level, 100) / 100.0f * 0.85f : s == AG_SPEAKING ? 0.4f + 0.6f * pulse : 0;
  int pct = sleep ? 35 : 100;

  static const uint16_t BODY[4] = {RGB(20, 90, 110), RGB(20, 125, 95), RGB(60, 165, 50), RGB(120, 195, 40)};
  static const uint16_t EDGE[4] = {RGB(70, 175, 185), RGB(90, 205, 145), RGB(165, 235, 75), RGB(245, 225, 75)};
  static const uint16_t WILT_BODY[4] = {RGB(90, 25, 25), RGB(120, 40, 25), RGB(150, 70, 25), RGB(170, 90, 30)};
  static const uint16_t WILT_EDGE[4] = {RGB(200, 60, 50), RGB(230, 90, 50), RGB(240, 140, 50), RGB(250, 190, 70)};
  static const float BASE[4] = {13.5f, 15.0f, 16.8f, 18.8f};
  static const float AMPL[4] = {1.3f, 1.7f, 2.0f, 2.2f};
  static const float FREQ[4] = {0.30f, 0.38f, 0.26f, 0.45f};
  static const float SPD[4] = {1.0f, -1.3f, 1.7f, -2.1f};

  cv->fillRect(0, 0, 240, CANVAS_H, COL_BG);

  // sky: twinkling stars at night, a faint dithered glow by day
  if (sleep) {
    for (int i = 0; i < 9; i++)
      if ((frame + i * 3) % 7 < 5) cell((i * 17 + 3) % OGX, (i * 5) % 9, RGB(170, 190, 220));
  }

  // sun (or moon), half hidden by the hills
  float sx = 20, sy = 12.5f, R = 5.2f + (s == AG_SPEAKING ? 1.3f * pulse : 0.5f * pulse) + (s == AG_LISTENING ? amp * 1.5f : 0) + (happy ? 1.5f : 0);
  if (sleep) R = 3.6f;
  for (int y = 0; y < OGY; y++)
    for (int x = 0; x < OGX; x++) {
      float dx = x - sx, dy = y - sy, d = sqrtf(dx * dx + dy * dy);
      uint16_t c = 0;
      if (d <= R) c = sleep ? RGB(150, 160, 190) : err ? RGB(230, 90, 40) : (d < R * 0.55f ? RGB(255, 245, 140) : (d < R * 0.8f ? RGB(255, 205, 60) : RGB(250, 150, 40)));
      else if (d <= R + 1.6f && !sleep && ((x + y) % 2 == 0)) c = err ? RGB(110, 40, 30) : RGB(120, 90, 30);   // dithered halo
      if (c) cell(x, y, c);
    }
  if (!sleep) {   // stepped rays
    int n = 14;
    for (int i = 0; i < n; i++) {
      float ang = -3.14159f + (i + 0.5f) * 3.14159f / n + (frame % 2 ? 0.04f : 0);
      if (!happy && !busy && (i + frame / 2) % 2) continue;   // rays shimmer on and off
      for (int k = 0; k < 2; k++) {
        float rr = R + 2.4f + k * 1.6f;
        cell((int)(sx + rr * cosf(ang) + 0.5f), (int)(sy + rr * sinf(ang) + 0.5f), err ? RGB(200, 90, 40) : RGB(255, 215, 80));
      }
    }
  }

  // hills, back to front
  int topAt[4][OGX];
  for (int k = 0; k < 4; k++) {
    float a = AMPL[k] + amp * (1.0f + k * 0.45f) * 2.2f;
    uint16_t body = dim565(err ? WILT_BODY[k] : BODY[k], pct), edge = dim565(err ? WILT_EDGE[k] : EDGE[k], pct);
    for (int x = 0; x < OGX; x++) {
      float y = BASE[k] + a * sinf(x * FREQ[k] + frame * spin * SPD[k]);
      if (eq) y -= amp * 1.2f * (1 - k * 0.2f) * fabsf(sinf(x * 0.7f + frame * 0.5f + k));
      int yt = (int)(y + 0.5f);
      topAt[k][x] = yt;
      if (yt < OGY) cv->fillRect(x * OP, yt * OP, OP, (OGY - yt) * OP, body);
      cell(x, yt, edge);
    }
  }

  // flowers on the front hill: a bud while asleep, a bloom otherwise
  static const int FX[5] = {4, 11, 20, 29, 36};
  static const uint16_t PET[5] = {RGB(255, 90, 200), RGB(80, 220, 255), RGB(255, 140, 60), RGB(255, 90, 200), RGB(120, 240, 130)};
  for (int i = 0; i < 5; i++) {
    int x = FX[i], y0 = topAt[3][x];
    int sway = (int)lroundf(sinf(frame * spin * 1.5f + i) * (happy ? 1.4f : 0.7f));
    int h = 3 + (i % 2);
    uint16_t stem = dim565(err ? RGB(130, 90, 40) : RGB(70, 190, 70), pct);
    for (int k = 1; k <= h; k++) cell(x + (k > h - 1 ? sway : 0), y0 - k, stem);
    int hx = x + sway, hy = y0 - h - 1;
    uint16_t pet = dim565(err ? RGB(120, 80, 40) : PET[i], pct);
    if (sleep || err) cell(hx, hy, pet);
    else {
      cell(hx, hy, RGB(255, 235, 90));
      cell(hx - 1, hy, pet); cell(hx + 1, hy, pet); cell(hx, hy - 1, pet);
      if (happy || (frame + i) % 6 < 3) cell(hx, hy + 1, pet);
    }
    if (i % 2 == 0) cell(x - 1, y0 - 2, stem);   // a leaf
  }

  // pollen drifting upwards
  if (!sleep) {
    int motes = (eq || happy) ? 10 : 6;
    for (int i = 0; i < motes; i++) {
      int px = (i * 13 + frame * (1 + i % 2) + (int)(sinf(frame * 0.3f + i) * 2)) % OGX;
      int py = 21 - ((frame * (1 + i % 3) + i * 7) % 19);
      cell(px, py, (i % 3 == 0) ? RGB(255, 245, 160) : (i % 3 == 1) ? RGB(160, 255, 150) : RGB(150, 240, 255));
    }
  }
  if (s == AG_LISTENING) {
    cv->drawFastHLine(KX, CANVAS_H - 3, 192, COL_DIM);
    cv->fillRect(KX, CANVAS_H - 5, 192 * min((int)level, 100) / 100, 4, YELLOW);
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
  bool keys = strlen(GROQ_API_KEY) && strlen(OPENAI_API_KEY) && strlen(N8N_WEBHOOK_URL);
  showText(keys ? "MANTEN [+] PARA HABLAR" : "FALTAN CLAVES EN SECRETS.H", keys ? COL_DIM : 0xF800);
  gfx->fillScreen(COL_BG);
  if (task) xTaskNotify(task, B_ENTER, eSetBits);
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
  if (!strlen(GROQ_API_KEY) || !strlen(OPENAI_API_KEY) || !strlen(N8N_WEBHOOK_URL)) return;
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
    if (skinIdx == 1) drawOrb(s, frame); else drawKraken(s, frame);
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
