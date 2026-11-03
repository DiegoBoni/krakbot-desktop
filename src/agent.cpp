#include "agent.h"
#include "hw.h"
#include "net.h"
#include "sound.h"
#include "secrets.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

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
#define TTS_VOICE "ash"
#define TTS_STYLE "Hablá en español rioplatense, con voz grave, pausada y un poco dramática, como un viejo lobo de mar."
#define TTS_GAIN 0.8f

#define RATE 16000
#define MAX_REC_S 15
#define MIN_REC_FRAMES (RATE / 2)
#define SKIP_FRAMES (RATE / 12)          // drop the key-click at the start
#define RADIO_IDLE_MS 60000UL
#define SLEEP_AFTER_MS 45000UL

extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

enum AgState { AG_IDLE, AG_CONNECTING, AG_LISTENING, AG_TRANSCRIBING, AG_THINKING, AG_SPEAKING, AG_ERROR, AG_SLEEP };

static volatile AgState state = AG_IDLE;
static volatile bool active = false, held = false, abortReq = false;
static volatile int level = 0;
static volatile uint32_t lastActivity = 0, speakStart = 0;
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
  void flush() {
    if (fill) streamWrite(out, fill);
    fill = 0;
  }
  void push(int16_t x) {
    while (t < n) {
      float v = (prev + (x - prev) * (t - (n - 1))) * TTS_GAIN;
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
  doc["voice"] = TTS_VOICE;
  doc["instructions"] = TTS_STYLE;
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
  switch (s) {
    case AG_THINKING: return 180;
    case AG_CONNECTING: return 180;
    case AG_TRANSCRIBING: return 180;
    case AG_SPEAKING: return 130;
    case AG_LISTENING: return 240;
    case AG_SLEEP: return 900;
    case AG_ERROR: return 120;
    default: return 500;
  }
}

static void drawKraken(AgState s, int frame) {
  int sway = 0, bob = 0;
  switch (s) {
    case AG_IDLE: sway = sinf(frame * 0.18f) * 14; bob = sinf(frame * 0.36f) * 3; break;
    case AG_LISTENING: sway = sinf(frame * 0.45f) * 9; bob = cosf(frame * 0.45f) * 5; break;
    case AG_SPEAKING: sway = sinf(frame * 0.6f) * 7; bob = (frame % 2) ? 3 : -3; break;
    case AG_THINKING: case AG_TRANSCRIBING: case AG_CONNECTING: sway = sinf(frame * 0.1f) * 5; break;
    default: break;
  }
  offX = sway;
  offY = bob + ((frame % 4 == 0) ? PIX / 4 : 0);
  bool sleeping = s == AG_SLEEP;
  bool blinking = sleeping || (s == AG_IDLE && frame % 7 == 6);

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
  if (s == AG_SPEAKING && frame % 2 == 0) blk(5, 5, 2, 2, COL_TEXT);
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
  if (s == AG_ERROR) {
    blk(10, 0, 1, 2, 0xF800);
    blk(10, 3, 1, 1, 0xF800);
  }
  if (s == AG_LISTENING) {   // input level bar under the kraken
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
    case AG_SPEAKING: col = CORAL; return "HABLANDO";
    case AG_ERROR: col = 0xF800; return "ERROR";
    case AG_SLEEP: col = COL_DIM; return "ZZZ";
    default: col = 0x07E0; return "LISTO";
  }
}

#define LINE_CH 28
#define LINES_PER_PAGE 6
static char lines[24][LINE_CH + 1];
static int nLines;

static void wrapText(const char *t) {
  nLines = 0;
  const char *p = t;
  while (*p && nLines < 24) {
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
}

static bool needRepaint = true;

void agentEnter() {
  if (!cv) {
    cv = new Arduino_Canvas(240, CANVAS_H, gfx, 0, 0);
    cv->begin(GFX_SKIP_OUTPUT_BEGIN);
  }
  held = false;
  abortReq = false;
  active = true;
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
  if (state == AG_SPEAKING) { abortReq = true; return; }
  if (state != AG_IDLE && state != AG_ERROR && state != AG_SLEEP) return;
  if (!strlen(GROQ_API_KEY) || !strlen(OPENAI_API_KEY) || !strlen(N8N_WEBHOOK_URL)) return;
  held = true;
  abortReq = false;
  state = AG_LISTENING;   // instant feedback; the worker takes over from here
  if (task) xTaskNotify(task, B_REC, eSetBits);
}

void agentRelease() { held = false; }

void agentCancel() {
  if (state != AG_IDLE) abortReq = true;
}

void agentInit() {
  xTaskCreate(agentTask, "agent", 16384, nullptr, 1, &task);
}

void agentTick(bool visible) {
  if (!visible || !active || !cv) return;
  uint32_t now = millis();
  AgState s = state;
  if (s == AG_IDLE && now - lastActivity > SLEEP_AFTER_MS) s = AG_SLEEP;

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
    drawKraken(s, frame);
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
