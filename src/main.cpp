// Orientation-driven apps on Waveshare ESP32-S3-Touch-LCD-1.54.
// Gravity from the QMI8658 picks one of three poses; each pose is its own "app".
#include "hw.h"
#include <Wire.h>
#include "touch.h"
#include "pomodoro.h"
#include "sound.h"
#include "buttons.h"
#include "net.h"
#include "clock.h"
#include "brightness.h"
#include "agent.h"
#include "fx.h"
#include "SensorQMI8658.hpp"

#define GFX_BL 46
#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_RED 0xF800
#define IMU_SDA 42
#define IMU_SCL 41
#define BAT_HOLD 2   // drive high to stay powered from battery after PWR is released
#define BAT_ADC 1    // battery voltage through a 1:3 divider

Arduino_DataBus *bus = new Arduino_ESP32SPI(45 /* DC */, 21 /* CS */, 38 /* SCK */, 39 /* MOSI */, -1);
Arduino_GFX *gfx = new Arduino_ST7789(bus, 40 /* RST */, 0, true, 240, 240,
                                         0, 0, 0, 80 /* 240x240 window offset in rotations 2/3 */);
SensorQMI8658 qmi;

enum Pose { POSE_UP, POSE_LEFT, POSE_RIGHT, POSE_NONE, POSE_FLAT };

// Display rotation that keeps content upright for each pose; tune after first test.
const uint8_t ROT[3] = {0, 3, 1};
const char *NAME[3] = {"VERTICAL", "IZQUIERDA", "DERECHA"};
const uint16_t COL[3] = {0x07E0, 0x07FF, 0xFD20};

const float G_THRESH = 0.65f;   // dominant-axis share (in g) needed to call a pose
const uint32_t HOLD_MS = 250;   // pose must be stable this long before switching

Pose classify(float ax, float ay, float az) {
  // Lying flat, screen up (gravity along -Z): neutral pose, screen off to save battery.
  if (az < -0.85f && fabsf(ax) < 0.35f && fabsf(ay) < 0.35f) return POSE_FLAT;
  // Buttons-up vertical: gravity along +Y (measured). Rolled 90 deg: gravity along +/-X.
  if (ay > G_THRESH) return POSE_UP;
  if (ax > G_THRESH) return POSE_LEFT;
  if (ax < -G_THRESH) return POSE_RIGHT;
  return POSE_NONE;
}

void drawPose(Pose p, float ax, float ay, float az) {
  gfx->setRotation(ROT[p]);
  gfx->fillScreen(C_BLACK);
  gfx->setTextColor(COL[p]);
  gfx->setTextSize(3);
  gfx->setCursor(20, 90);
  gfx->print(NAME[p]);
}

void drawReadout(float ax, float ay, float az) {
  gfx->setTextSize(1);
  gfx->setTextColor(C_WHITE, C_BLACK);
  gfx->setCursor(20, 140);
  gfx->printf("ax %5.2f ay %5.2f az %5.2f", ax, ay, az);
  gfx->setCursor(20, 155);
  gfx->printf("bat %.2f V   ", analogReadMilliVolts(BAT_ADC) * 3 / 1000.0f);
}

Pose current = POSE_NONE, candidate = POSE_NONE;
uint32_t candidateSince = 0;
bool screenOff = false;   // panel + backlight dark (lying flat, or PWR pressed)
bool manualOff = false;   // PWR toggled the screen off

FxApp fxFor(Pose p) { return p == POSE_UP ? FX_POMODORO : p == POSE_LEFT ? FX_CLOCK : FX_AGENT; }

// Brings app `p` on screen: old content collapses, the new orientation splashes, then the app paints.
void enterApp(Pose p, bool fromDark) {
  if (fromDark) {
    gfx->displayOn();
    gfx->setRotation(ROT[p]);
    gfx->fillScreen(C_BLACK);
    backlightSleep(false);
    screenOff = false;
  } else {
    fxCloseOld();
    gfx->setRotation(ROT[p]);
  }
  fxSplash(fxFor(p));
  if (p == POSE_UP) pomodoroEnter();
  else if (p == POSE_LEFT) clockEnter();
  else agentEnter();
}

void goDark() {
  if (current == POSE_RIGHT) agentLeave();
  backlightSleep(true);
  gfx->displayOff();
  screenOff = true;
}

void repaintApp() {
  if (current == POSE_UP) pomodoroEnter();
  else if (current == POSE_LEFT) clockEnter();
  else if (current == POSE_RIGHT) agentRepaint();
}

// Vertical drag anywhere on screen = master volume (up louder). Taps go to the app on lift.
void handleTouch(const TouchEvent &te) {
  static bool volDrag = false;
  static int volStart = 0;
  if (te.kind == TOUCH_DRAG) {
    // panel-native displacement -> displacement along the screen's current vertical axis
    int ldy = current == POSE_UP ? te.dy : current == POSE_RIGHT ? -te.dx : te.dx;
    if (current == POSE_RIGHT && agentInHistory()) { agentHistDrag(ldy, false); return; }   // reading: scroll, not volume
    if (!volDrag) {
      if (abs(ldy) < 14) return;
      volDrag = true;
      volStart = soundVolume();
    }
    int v = constrain(volStart - ldy * 100 / 140, 0, 100);
    soundSetVolume(v, false);
    fxVolumeShow(v);
  } else if (te.kind == TOUCH_DRAG_END) {
    if (current == POSE_RIGHT && agentInHistory()) { agentHistDrag(0, true); return; }
    if (volDrag) {
      volDrag = false;
      soundSetVolume(soundVolume(), true);
      soundPlay(SND_CLICK);   // sample of the new level
    }
  } else if (te.kind == TOUCH_TAP && !fxOverlayActive()) {
    if (Serial) Serial.printf("tap x=%d y=%d\n", te.x, te.y);
    if (current == POSE_RIGHT) agentTouch();
    if (current == POSE_UP) pomodoroTouch(te.x, te.y);
  }
}

void setup() {
  pinMode(BAT_HOLD, OUTPUT);
  digitalWrite(BAT_HOLD, HIGH);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // never block when no USB host is attached (battery use)
  backlightInit();
  gfx->begin();
  gfx->fillScreen(C_BLACK);

  if (!qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IMU_SDA, IMU_SCL)) {
    gfx->setTextColor(C_RED);
    gfx->println("IMU no responde");
    while (true) delay(1000);
  }
  qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_125Hz,
                          SensorQMI8658::LPF_MODE_0);
  qmi.enableAccelerometer();
  Wire.setClock(400000);
  touchInit();
  buttonsInit();
  soundInit();
  netInit();
  clockInit();
  agentInit();
}

void loop() {
  IMUdata acc;
  if (qmi.getDataReady() && qmi.getAccelerometer(acc.x, acc.y, acc.z)) {
    Pose p = classify(acc.x, acc.y, acc.z);
    uint32_t now = millis();
    if (p != candidate) { candidate = p; candidateSince = now; }
    bool hold = p == POSE_FLAT && current == POSE_RIGHT && agentBusy();   // agent mid-turn: stay lit until idle
    if (p != POSE_NONE && p != current && !hold && now - candidateSince >= HOLD_MS) {
      Pose prev = current;
      if (prev == POSE_RIGHT) agentLeave();
      current = p;
      if (p == POSE_FLAT || manualOff) { if (!screenOff) goDark(); }
      else enterApp(p, screenOff);
    }
    static uint32_t lastLog = 0;
    if (now - lastLog > 300) {
      lastLog = now;
      if (Serial) Serial.printf("ax=%.2f ay=%.2f az=%.2f pose=%d\n", acc.x, acc.y, acc.z, (int)current);
    }
  }
  TouchEvent te = touchPoll();
  if (te.kind != TOUCH_NONE) {
    if (!screenOff) handleTouch(te);
    else if (te.kind == TOUCH_TAP && manualOff && current != POSE_FLAT) {   // a tap wakes a PWR-blanked screen
      manualOff = false;
      enterApp(current, true);
    }
  }
  BtnEvent plus = buttonsPoll(BTN_PLUS), boot = buttonsPoll(BTN_BOOT), pwr = buttonsPoll(BTN_PWR);
  if (Serial && (plus || boot || pwr)) Serial.printf("keys plus=%d boot=%d pwr=%d\n", plus, boot, pwr);
  if (pwr == EV_LONG) fxPowerOff();
  if (pwr == EV_CLICK && current != POSE_FLAT && current != POSE_NONE) {
    manualOff = !manualOff;
    if (manualOff) { if (!screenOff) goDark(); }
    else if (screenOff) enterApp(current, true);
  }
  bool live = !screenOff;
  if (live && current == POSE_UP) {
    if (plus == EV_CLICK) pomodoroAction(POMO_MAIN);
    if (plus == EV_LONG) pomodoroAction(POMO_RESET);
    if (boot == EV_CLICK) pomodoroAction(POMO_NEXT);
  }
  if (live && current == POSE_RIGHT) {
    static bool talking = false;
    bool held = buttonsHeld(BTN_PLUS);
    if (held && !talking) agentPress();
    if (!held && talking) agentRelease();
    talking = held;
    if (boot == EV_CLICK) agentBoot();
    if (boot == EV_LONG) agentSkinNext();
  }
  if (live && current == POSE_LEFT) {
    if (boot == EV_CLICK) { backlightCycle(); soundPlay(SND_CLICK); }
    if (plus == EV_CLICK) clockToggleChime();
  }
  if (fxOverlayTick()) repaintApp();
  bool show = live && !fxOverlayActive();
  agentTick(show && current == POSE_RIGHT);
  pomodoroTick(show && current == POSE_UP);
  clockBackground();
  clockTick(show && current == POSE_LEFT);
  delay(screenOff ? 40 : 5);   // idle faster polling while the screen is off
}
