#include <Arduino.h>
#include <MySQL_Connection.h>
#include <MySQL_Cursor.h>
#include <math.h>
#include <M5Dial.h>
#include <WiFi.h>
#include <Wire.h>
#include <SparkFun_Qwiic_Keypad_Arduino_Library.h>
#include <HWCDC.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_wifi.h>

#include "config.h"
#include "lcc.h"
#include "settings.h"

namespace {

WiFiClient wtClient;
KEYPAD keypad;
M5Canvas uiCanvas(&M5Dial.Display);
bool uiCanvasReady = false;

uint16_t activeLocoAddress = LOCO_ADDRESS;
bool activeLocoIsLong = LOCO_IS_LONG_ADDRESS;
bool settingsWereStored = false;
String locoId;
bool locoSelected = false;
bool locoAcquired = false;
bool directionForward = true;
int speed126 = 0;
int throttleDemandSpeed = 0;
int throttleNotch = 0;
unsigned long lastMomentumMs = 0;
unsigned long lastDetentMs = 0;
int32_t momentumAccumMilli = 0;
const char* lastActivitySource = "boot";
uint32_t activityCount = 0;
bool functionState[29] = {false};
bool encoderUpdatingSpeed = false;
bool brakeHoldActive = false;
bool brakeRecovering = false;
int brakeRecoveryTargetSpeed = 0;
unsigned long lastBrakeStepMs = 0;
unsigned long lastBrakeTouchMs = 0;
bool brakeStoppedToZero = false;
bool estopLatched = false;

bool keypadAddressMode = false;
String keypadAddressBuffer;
bool keypadTurnoutMode = false;
String keypadTurnoutBuffer;
String lastRfidUid;
unsigned long lastRfidMs = 0;

unsigned long lastHeartbeatMs = 0;
unsigned long lastServerActivityMs = 0;

// WiThrottle session state.
bool locoAcquirePending = false;      // M0+ sent, waiting for the server to confirm
unsigned long locoAcquireSentMs = 0;
uint8_t locoAcquireAttempts = 0;
bool stealPending = false;            // server asked whether to steal the loco
float serverProtocolVersion = 0.0f;
// Speeds we sent recently. JMRI echoes every speed command back, and during a momentum ramp those
// echoes lag behind; matching them exactly avoids mistaking our own echo for another throttle.
struct SentSpeed {
  int16_t value;
  unsigned long atMs;
};
SentSpeed sentSpeeds[64] = {};
uint8_t sentSpeedIdx = 0;

String statusLine = "Booting...";
String infoLine = "";
unsigned long infoLineSetMs = 0;
bool uiDirty = true;
unsigned long lastUserActivityMs = 0;
uint8_t backlightMode = 0; // 0=active, 1=dim, 2=off
bool wifiSuspendedForPower = false;
bool displayPanelAsleep = false;
int8_t wifiPowerSaveApplied = -1; // -1 unknown, 0 none, 1 min modem, 2 max modem
bool wifiConnecting = false;
unsigned long wifiConnectStartMs = 0;
unsigned long wifiNextRetryMs = 0;
unsigned long wtNextRetryMs = 0;
uint32_t wtRetryIntervalMs = 5000;
bool keypadPresent = false;
unsigned long lastKeypadPollMs = 0;
unsigned long lastRfidPollMs = 0;
unsigned long lastRfidSeenMs = 0;
int batteryPercent = -1; // -1 = no gauge available
enum class PowerSource : uint8_t { Unknown, Battery, External };
enum class ChargeState : uint8_t { Unknown, NotCharging, Charging };
PowerSource powerSource = PowerSource::Unknown;
ChargeState chargeState = ChargeState::Unknown;
bool usbHostPresent = false;
int powerSenseMillivolts = -1;
unsigned long lastPowerSourceSampleMs = 0;
int batteryMillivolts = -1;
unsigned long lastBatterySampleMs = 0;
bool batteryLowWarned = false;
bool buttonLongHandled = false;

bool encoderStateInitialized = false;
bool encoderResyncPending = false;
int32_t encoderDetentRef = 0;   // raw count at the last accepted detent
int32_t encoderLastRaw = 0;
unsigned long encoderLastMotionMs = 0;
int8_t lastDetentDir = 0;
int8_t encoderPendingDetents = 0; // net detents travelled since the current notch position
uint32_t lastDrawUsec = 0;
unsigned long lastDrawMs = 0;
uint32_t lastRenderUsec = 0;
uint32_t maxLoopUsec = 0;
uint32_t lastLoopUsec = 0;

void setStatus(const String& s);
void setInfo(const String& s);
void sendWt(const String& line);
void setSpeed(int newSpeed);
void clearEmergencyStopLatch();
void backendAcquire();
void backendRelease();
void backendSendSpeed(int step);
void backendSendDirection(bool forward);
void backendSendFunction(uint8_t fn, bool on);
void backendEStop();
void backendQuit();
bool backendConnected();
void resyncLocoStateToServer();
void setStatus(const String& s);
void ensureLocoAcquired();
void applyStopAction(const char* statusText);
void noteUserActivity(const char* source = "?");
void updatePowerState();
void suspendWiFiForPower();
void resumeWiFiFromPower();
void enterBacklightMode(uint8_t mode);
void applyWiFiPowerSave();
void powerOffDevice();
void sampleBattery(bool force);
void samplePowerSource(bool force);
void applyNotch(int notch, bool fromEncoder);
void renderUi(LovyanGFX& g);
bool usingLcc();
void applySettings();
int notchCount();
int maxNotch();
int encoderSign();
int notchToSpeed(int notch);
int speedToNotch(int speed);
void syncEncoderToSpeed();

// Full-frame off-screen buffer so each redraw is a single push instead of clear-then-draw.
void initUiCanvas() {
  const int16_t w = M5Dial.Display.width();
  const int16_t h = M5Dial.Display.height();
  uiCanvas.setColorDepth(16);
  uiCanvasReady = uiCanvas.createSprite(w, h) != nullptr;
  if (!uiCanvasReady) {
    uiCanvas.setColorDepth(8);
    uiCanvasReady = uiCanvas.createSprite(w, h) != nullptr;
  }
  Serial.printf("UI canvas: %s (%d-bit), free heap %u\n", uiCanvasReady ? "ok" : "unavailable, drawing direct",
                uiCanvasReady ? uiCanvas.getColorDepth() : 0, static_cast<unsigned>(ESP.getFreeHeap()));
}

// Dump the rendered frame over serial as hex rows (decode with tools/screenshot.py).
void dumpScreenshot() {
  if (!uiCanvasReady) {
    Serial.println("! screenshot needs the UI canvas");
    return;
  }
  if (settings::isOpen()) {
    settings::render(uiCanvas);
  } else {
    renderUi(uiCanvas);
  }
  const int w = uiCanvas.width();
  const int h = uiCanvas.height();
  const int bpp = uiCanvas.getColorDepth() / 8;
  const uint8_t* buf = static_cast<const uint8_t*>(uiCanvas.getBuffer());
  Serial.printf("SHOT BEGIN %d %d %d\n", w, h, bpp * 8);
  static const char* hex = "0123456789ABCDEF";
  char line[240 * 4 + 1];
  for (int y = 0; y < h; ++y) {
    const uint8_t* row = buf + static_cast<size_t>(y) * w * bpp;
    int n = 0;
    for (int i = 0; i < w * bpp && n < static_cast<int>(sizeof(line)) - 2; ++i) {
      line[n++] = hex[row[i] >> 4];
      line[n++] = hex[row[i] & 0x0F];
    }
    line[n] = 0;
    Serial.println(line);
    Serial.flush();
  }
  Serial.println("SHOT END");
}

bool onExternalPower();
void drawUi(bool force = false);
void beep();

void beep() {
  if (settings::cfg.sound) {
    M5Dial.Speaker.tone(1200, 140);
  }
}

void beepRfidAddressChange() {
  if (settings::cfg.sound) {
    M5Dial.Speaker.tone(1650, 140);
  }
}

void noteUserActivity(const char* source) {
  lastUserActivityMs = millis();
  lastActivitySource = source;
  ++activityCount;
  if (backlightMode != 0) {
    enterBacklightMode(0);
  }
}

// True when it is safe to drop the WiThrottle link or sleep: nothing is moving or braking.
bool throttleIsIdleSafe() {
  return speed126 == 0 && throttleDemandSpeed == 0 && !brakeHoldActive && !brakeRecovering;
}

void suspendWiFiForPower() {
  if (SERIAL_OUTPUT_ONLY || !ENABLE_WIFI_POWER_GATING_WHEN_DISPLAY_OFF || wifiSuspendedForPower) {
    return;
  }

  wtClient.stop();
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  wifiSuspendedForPower = true;
  wifiConnecting = false;
  wifiPowerSaveApplied = -1;
  Serial.println("Power: WiFi suspended (loco stopped)");
  setInfo("WiFi sleep");
}

void resumeWiFiFromPower() {
  if (SERIAL_OUTPUT_ONLY || !wifiSuspendedForPower) {
    return;
  }

  WiFi.mode(WIFI_STA);
  wifiSuspendedForPower = false;
  wifiNextRetryMs = 0; // reconnect on the next loop pass
  setInfo("WiFi wake");
}

// Modem sleep depth follows the backlight tier: minimum while in use, maximum while idle.
void applyWiFiPowerSave() {
  if (SERIAL_OUTPUT_ONLY || wifiSuspendedForPower || WiFi.status() != WL_CONNECTED) {
    return;
  }
  int8_t want = 0;
  if (ENABLE_WIFI_MODEM_SLEEP) {
    want = (ENABLE_WIFI_MAX_MODEM_SLEEP_WHEN_IDLE && backlightMode != 0) ? 2 : 1;
  }
  if (want == wifiPowerSaveApplied) {
    return;
  }
  wifiPowerSaveApplied = want;
  esp_wifi_set_ps(want == 2 ? WIFI_PS_MAX_MODEM : (want == 1 ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE));
}

void sleepDisplayPanel() {
  if (!ENABLE_DISPLAY_PANEL_SLEEP || displayPanelAsleep) {
    return;
  }
  M5Dial.Display.sleep();
  M5Dial.Display.waitDisplay();
  displayPanelAsleep = true;
}

void wakeDisplayPanel() {
  if (!displayPanelAsleep) {
    return;
  }
  M5Dial.Display.wakeup(); // restores the cached brightness, which is 0 here; set explicitly by caller
  delay(130);              // GC9A01 needs >=120 ms after SLPOUT before it reliably accepts pixel data
  M5Dial.Display.startWrite();
  M5Dial.Display.writeCommand(0x29); // DISPON, in case the panel dropped it during sleep
  M5Dial.Display.endWrite();
  displayPanelAsleep = false;
}

void enterBacklightMode(uint8_t mode) {
  if (mode == backlightMode) {
    return;
  }
  backlightMode = mode;
  Serial.printf("Power: %s\n", mode == 0 ? "active" : (mode == 1 ? "display dim" : "display off"));
  if (mode == 0) {
    if (wifiSuspendedForPower) {
      resumeWiFiFromPower();
    }
    wakeDisplayPanel();
    uiDirty = true;
    drawUi(true); // paint the frame before the backlight comes on
    M5Dial.Display.setBrightness(settings::cfg.brightnessActive);
  } else if (mode == 1) {
    if (wifiSuspendedForPower) {
      resumeWiFiFromPower();
    }
    wakeDisplayPanel();
    M5Dial.Display.setBrightness(settings::cfg.brightnessDim);
  } else {
    M5Dial.Display.setBrightness(0);
    sleepDisplayPanel();
  }
  applyWiFiPowerSave();
}

// Deep sleep stops LEDC and releases the backlight PWM pin, which lets the backlight come on over a
// sleeping (black) panel. Drive it low and latch it so it stays off until setup() releases it.
void holdBacklightOff() {
  const gpio_num_t bl = static_cast<gpio_num_t>(DISPLAY_BACKLIGHT_PIN);
  ledcDetach(bl);
  pinMode(bl, OUTPUT);
  digitalWrite(bl, LOW);
  gpio_hold_en(bl);
  gpio_deep_sleep_hold_en();
}

void powerOffDevice() {
  setStatus("Powering off");
  if (!SERIAL_OUTPUT_ONLY && wtClient.connected()) {
    if (locoAcquired || locoAcquirePending) {
      backendRelease();
      locoAcquired = false;
      locoAcquirePending = false;
    }
    backendQuit();
    wtClient.flush();
    delay(100);
    wtClient.stop();
  }
  if (!SERIAL_OUTPUT_ONLY) {
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
  }
  M5Dial.Rfid.PCD_AntennaOff();
  Serial.println("Power off (battery: press dial button to restart; USB: touch screen to wake)");
  Serial.flush();
  M5Dial.Display.setBrightness(0);
  M5Dial.Display.sleep();
  M5Dial.Display.waitDisplay();
  holdBacklightOff();
  // On battery the hold circuit cuts power. On USB the ESP32 stays powered and M5Unified falls back to
  // deep sleep, so arm the touch controller INT line (GPIO14, RTC capable) as a wake source first.
  rtc_gpio_pullup_en(GPIO_NUM_14);
  rtc_gpio_pulldown_dis(GPIO_NUM_14);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_14, 0);
  M5Dial.Power.powerOff();
}

void updatePowerState() {
  const unsigned long idleMs = millis() - lastUserActivityMs;
  uint8_t target = 0;
  if (idleMs >= (settings::cfg.offAfterSec * 1000UL)) {
    target = 2;
  } else if (idleMs >= (settings::cfg.dimAfterSec * 1000UL)) {
    target = 1;
  }

  if (target != backlightMode) {
    enterBacklightMode(target);
  }

  // Only drop the WiThrottle link (and later sleep/power off) once the loco is stopped, so a train
  // running hands-off keeps receiving heartbeats.
  if (backlightMode == 2 && !wifiSuspendedForPower && throttleIsIdleSafe()) {
    suspendWiFiForPower();
  }

  if ((settings::cfg.powerOffAfterMin * 60000UL) > 0 && idleMs >= (settings::cfg.powerOffAfterMin * 60000UL) && throttleIsIdleSafe() &&
      !(POWER_OFF_ONLY_ON_BATTERY && onExternalPower())) {
    powerOffDevice();
  }
}

// Light-sleep between polls while the display is off and WiFi is already suspended.
void idleSleep() {
  // Light sleep drops the USB-JTAG-serial link, so skip it whenever a USB host is physically present
  // (SOF frames seen), not just while a terminal has the port open.
  if (!ENABLE_LIGHT_SLEEP_WHEN_DISPLAY_OFF || !wifiSuspendedForPower || M5Dial.Speaker.isPlaying() ||
      (LIGHT_SLEEP_SKIP_WHEN_USB_SERIAL && HWCDC::isPlugged())) {
    delay(LOOP_DELAY_OFF_MS);
    return;
  }

  Serial.flush();
  const gpio_num_t encA = static_cast<gpio_num_t>(DIAL_ENCODER_PIN_A);
  const gpio_num_t encB = static_cast<gpio_num_t>(DIAL_ENCODER_PIN_B);
  const gpio_num_t btn = GPIO_NUM_42;      // dial push button
  const gpio_num_t touchInt = GPIO_NUM_14; // FT3267 touch INT (active low)
  gpio_wakeup_enable(encA, gpio_get_level(encA) ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
  gpio_wakeup_enable(encB, gpio_get_level(encB) ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
  gpio_wakeup_enable(btn, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable(touchInt, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(LIGHT_SLEEP_INTERVAL_MS) * 1000ULL);
  esp_light_sleep_start();

  // gpio_wakeup_enable() re-types these pins as level interrupts. Restore the encoder library's edge
  // interrupts and leave the polled pins with interrupts disabled, or the ISR would run continuously.
  gpio_wakeup_disable(encA);
  gpio_wakeup_disable(encB);
  gpio_wakeup_disable(btn);
  gpio_wakeup_disable(touchInt);
  gpio_set_intr_type(encA, GPIO_INTR_ANYEDGE);
  gpio_set_intr_type(encB, GPIO_INTR_ANYEDGE);
  gpio_set_intr_type(btn, GPIO_INTR_DISABLE);
  gpio_set_intr_type(touchInt, GPIO_INTR_DISABLE);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
}

// Piecewise 1S Li-ion/LiPo open-circuit voltage curve.
int batteryPercentFromMillivolts(int mv) {
  static const struct {
    int mv;
    int pct;
  } curve[] = {{4200, 100}, {4100, 92}, {4000, 80}, {3900, 65}, {3850, 55}, {3800, 45}, {3750, 35},
               {3700, 25},  {3650, 17}, {3600, 10}, {3500, 5},  {3400, 2},  {3300, 0}};
  const size_t n = sizeof(curve) / sizeof(curve[0]);
  if (mv >= curve[0].mv) {
    return 100;
  }
  if (mv <= curve[n - 1].mv) {
    return 0;
  }
  for (size_t i = 1; i < n; ++i) {
    if (mv >= curve[i].mv) {
      const int span = curve[i - 1].mv - curve[i].mv;
      return curve[i].pct + (curve[i - 1].pct - curve[i].pct) * (mv - curve[i].mv) / span;
    }
  }
  return 0;
}

bool onExternalPower() {
  return powerSource == PowerSource::External || chargeState == ChargeState::Charging;
}

void samplePowerSource(bool force) {
  const unsigned long now = millis();
  if (!force && (now - lastPowerSourceSampleMs) < 1000) {
    return;
  }
  lastPowerSourceSampleMs = now;

  // A USB host keeps sending SOF frames; a plain charger or the DC terminal does not, so this alone
  // can only prove "external", never "battery".
  usbHostPresent = HWCDC::isPlugged();
  PowerSource ps = usbHostPresent ? PowerSource::External : PowerSource::Unknown;
  if (POWER_SENSE_ADC_PIN >= 0) {
    uint32_t acc = 0;
    for (int i = 0; i < 8; ++i) {
      acc += analogReadMilliVolts(static_cast<uint8_t>(POWER_SENSE_ADC_PIN));
    }
    powerSenseMillivolts = static_cast<int>((acc / 8) * POWER_SENSE_DIVIDER_RATIO + 0.5f);
    ps = (usbHostPresent || powerSenseMillivolts >= POWER_SENSE_MIN_MV) ? PowerSource::External
                                                                         : PowerSource::Battery;
  }

  ChargeState cs = ChargeState::Unknown;
  if (CHARGE_STATUS_PIN >= 0) {
    const bool low = digitalRead(CHARGE_STATUS_PIN) == LOW;
    cs = (low == CHARGE_STATUS_ACTIVE_LOW) ? ChargeState::Charging : ChargeState::NotCharging;
  }

  if (ps != powerSource || cs != chargeState) {
    powerSource = ps;
    chargeState = cs;
    uiDirty = true;
  }
}

void sampleBattery(bool force) {
  const unsigned long now = millis();
  if (!force && (now - lastBatterySampleMs) < BATTERY_SAMPLE_INTERVAL_MS) {
    return;
  }
  lastBatterySampleMs = now;

  int mv = -1;
  int pct = -1;
  if (BATTERY_ADC_PIN >= 0) {
    uint32_t acc = 0;
    for (int i = 0; i < 16; ++i) {
      acc += analogReadMilliVolts(static_cast<uint8_t>(BATTERY_ADC_PIN));
    }
    mv = static_cast<int>((acc / 16) * BATTERY_DIVIDER_RATIO + 0.5f);
    if (batteryMillivolts > 0 && !force) {
      mv = (batteryMillivolts * 3 + mv) / 4; // light smoothing against load transients
    }
    pct = batteryPercentFromMillivolts(mv);
  } else {
    // The M5Dial has no battery sense line, so this returns -2 today; kept so a future M5Unified
    // release (or a different board) works without changes.
    const int32_t level = M5Dial.Power.getBatteryLevel();
    if (level >= 0 && level <= 100) {
      pct = static_cast<int>(level);
    }
  }

  batteryMillivolts = mv;
  if (pct != batteryPercent) {
    batteryPercent = pct;
    uiDirty = true;
  }
  if (pct >= 0 && pct <= BATTERY_LOW_WARN_PERCENT) {
    if (!batteryLowWarned) {
      batteryLowWarned = true;
      setStatus(String("Battery low: ") + pct + "%");
      beep();
    }
  } else if (pct > BATTERY_LOW_WARN_PERCENT + 5) {
    batteryLowWarned = false;
  }
}

int notchToSpeed(int notch) {
  if (notch < 0) {
    notch = 0;
  }
  if (notch > notchCount()) {
    notch = notchCount();
  }
  return (notch * 126 + notchCount() / 2) / notchCount();
}

int speedToNotch(int speed) {
  if (speed <= 0) {
    return 0;
  }
  if (speed >= 126) {
    return notchCount();
  }
  return (speed * notchCount() + 63) / 126;
}

void syncEncoderToSpeed() {
  const int32_t rawTarget = static_cast<int32_t>(throttleNotch) * settings::cfg.clicksPerNotch *
                            ENCODER_RAW_COUNTS_PER_STEP * encoderSign();
  M5Dial.Encoder.write(rawTarget);
  // Ignore one encoder delta after write() so we don't interpret the sync as user rotation.
  encoderResyncPending = true;
}

// Operator selects a notch; with momentum the speed follows over time, otherwise immediately.
void applyNotch(int notch, bool fromEncoder) {
  if (notch < 0) {
    notch = 0;
  } else if (notch > maxNotch()) {
    notch = maxNotch();
  }
  if (estopLatched && notch > 0) {
    setStatus("E-Stop latched: press BtnA to clear");
    if (!fromEncoder) {
      return;
    }
    notch = 0;
  }
  if (notch == throttleNotch && throttleDemandSpeed == notchToSpeed(notch)) {
    return;
  }
  throttleNotch = notch;
  throttleDemandSpeed = notchToSpeed(notch);
  uiDirty = true;
  if (!fromEncoder) {
    syncEncoderToSpeed();
  }
  if (notch > 0) {
    ensureLocoAcquired();
  }
  if (brakeHoldActive || brakeRecovering) {
    brakeRecoveryTargetSpeed = throttleDemandSpeed;
    return;
  }
  if (settings::cfg.momentum) {
    lastMomentumMs = millis();
    return;
  }
  encoderUpdatingSpeed = true;
  setSpeed(throttleDemandSpeed);
  encoderUpdatingSpeed = false;
}

// Ramp the actual speed toward the notch target.
void handleMomentum() {
  if (!settings::cfg.momentum || brakeHoldActive || brakeRecovering || estopLatched) {
    return;
  }
  const unsigned long now = millis();
  const int target = throttleDemandSpeed;
  if (speed126 == target) {
    lastMomentumMs = now;
    momentumAccumMilli = 0;
    return;
  }
  const unsigned long dt = now - lastMomentumMs;
  if (dt < MOMENTUM_TICK_MS) {
    return;
  }
  lastMomentumMs = now;
  const float rate = (speed126 < target) ? static_cast<float>(settings::cfg.accelPerSec) : static_cast<float>(settings::cfg.decelPerSec);
  momentumAccumMilli += static_cast<int32_t>(rate * 1000.0f * static_cast<float>(dt) / 1000.0f);
  const int steps = momentumAccumMilli / 1000;
  if (steps <= 0) {
    return;
  }
  momentumAccumMilli -= steps * 1000;
  int next = (speed126 < target) ? min(speed126 + steps, target) : max(speed126 - steps, target);
  encoderUpdatingSpeed = true;
  setSpeed(next);
  encoderUpdatingSpeed = false;
}

String buildLocoId(uint16_t address, bool isLong) {
  return String(isLong ? "L" : "S") + String(address);
}

void updateLocoLabel() {
  if (!locoSelected) {
    locoId = "-";
    return;
  }
  locoId = buildLocoId(activeLocoAddress, activeLocoIsLong);
}

void setActiveLoco(uint16_t address, bool isLong, bool autoAcquire, const String& source) {
  if (address == 0 || address > 9999) {
    setStatus("Invalid loco address");
    return;
  }

  const bool addressChanged = (!locoSelected || activeLocoAddress != address || activeLocoIsLong != isLong);

  if (locoAcquired || locoAcquirePending) {
    backendRelease();
    locoAcquired = false;
    locoAcquirePending = false;
  }
  stealPending = false;

  activeLocoAddress = address;
  activeLocoIsLong = isLong;
  locoSelected = true;
  updateLocoLabel();
  if (addressChanged && source.startsWith("RFID")) {
    beepRfidAddressChange();
  }
  setStatus(String("Loco set ") + locoId + " via " + source);

  if (autoAcquire) {
    backendAcquire();
    setStatus(String("Loco selected ") + locoId + " via " + source);
  }
}

void setStatus(const String& s) {
  statusLine = s;
  uiDirty = true;
  Serial.println(s);
}

void setInfo(const String& s) {
  infoLineSetMs = millis();
  if (infoLine == s) {
    return;
  }
  infoLine = s;
  uiDirty = true;
}

// Transient notes fade so the settings hint underneath comes back into view.
void expireInfoLine() {
  if (!infoLine.isEmpty() && (millis() - infoLineSetMs) > INFO_LINE_TIMEOUT_MS) {
    infoLine = "";
    uiDirty = true;
  }
}

// Remember a speed we sent so its echo can be recognised later.
void noteSentSpeed(int v) {
  sentSpeeds[sentSpeedIdx].value = static_cast<int16_t>(v);
  sentSpeeds[sentSpeedIdx].atMs = millis();
  sentSpeedIdx = (sentSpeedIdx + 1) % (sizeof(sentSpeeds) / sizeof(sentSpeeds[0]));
}

// True if this speed is the echo of one we sent recently; the entry is consumed.
bool consumeSpeedEcho(int v) {
  const unsigned long now = millis();
  for (size_t i = 0; i < sizeof(sentSpeeds) / sizeof(sentSpeeds[0]); ++i) {
    if (sentSpeeds[i].atMs != 0 && sentSpeeds[i].value == v &&
        (now - sentSpeeds[i].atMs) <= WITHROTTLE_ECHO_WINDOW_MS) {
      sentSpeeds[i].atMs = 0;
      return true;
    }
  }
  return false;
}

void sendWt(const String& line) {
  Serial.print("WT> ");
  Serial.println(line);

  if (SERIAL_OUTPUT_ONLY) {
    return;
  }

  if (!wtClient.connected()) {
    Serial.println("! WiThrottle not connected");
    return;
  }
  wtClient.print(line);
  wtClient.print('\n');
  lastServerActivityMs = millis();
}

// ---------------------------------------------------------------------------
// Throttle backend: WiThrottle (JMRI) or LCC/OpenLCB over a GridConnect link.
// ---------------------------------------------------------------------------

bool usingLcc() {
  return settings::cfg.protocol == PROTOCOL_LCC_GRIDCONNECT;
}

int notchCount() {
  return settings::cfg.notchCount > 0 ? settings::cfg.notchCount : 8;
}

// Highest notch the dial will select. Reverse is capped (REVERSE_NOTCH_DIVISOR) so the throttle
// only runs the loco up to a fraction of full speed backwards.
int maxNotch() {
  if (directionForward || REVERSE_NOTCH_DIVISOR <= 1) {
    return notchCount();
  }
  return max(1, notchCount() / REVERSE_NOTCH_DIVISOR);
}

int encoderSign() {
  return settings::cfg.encoderReversed ? -1 : 1;
}

// Push saved settings into the parts of the firmware that hold their own copy.
void applySettings() {
  lcc::setServer(settings::ipString(), settings::cfg.port);
  if (backlightMode == 0) {
    M5Dial.Display.setBrightness(settings::cfg.brightnessActive);
  } else if (backlightMode == 1) {
    M5Dial.Display.setBrightness(settings::cfg.brightnessDim);
  }
  // A changed notch count (or dial direction) invalidates the current notch, so derive it again
  // from the speed the loco is actually doing.
  throttleNotch = speedToNotch(speed126);
  throttleDemandSpeed = settings::cfg.momentum ? notchToSpeed(throttleNotch) : speed126;
  syncEncoderToSpeed();
  uiDirty = true;
}

String wtLocoKey() {
  return String(WITHROTTLE_THROTTLE_KEY) + locoId;
}

bool backendConnected() {
  if (SERIAL_OUTPUT_ONLY) {
    return true;
  }
  return usingLcc() ? lcc::connected() : wtClient.connected();
}

void backendAcquire() {
  if (!locoSelected) {
    return;
  }
  if (usingLcc()) {
    lcc::selectTrain(activeLocoAddress, activeLocoIsLong);
    locoAcquirePending = true;
    locoAcquireSentMs = millis();
    ++locoAcquireAttempts;
    setStatus(String("Acquiring ") + locoId + "...");
    return;
  }
  // WiThrottle: the key before <;> must match the address after it.
  sendWt(String("M") + WITHROTTLE_THROTTLE_KEY + "+" + locoId + "<;>" + locoId);
  locoAcquirePending = true;
  locoAcquired = false;
  locoAcquireSentMs = millis();
  ++locoAcquireAttempts;
  setStatus(String("Acquiring ") + locoId + "...");
}

void backendRelease() {
  if (!locoSelected) {
    return;
  }
  if (usingLcc()) {
    lcc::releaseTrain();
    return;
  }
  sendWt(String("M") + WITHROTTLE_THROTTLE_KEY + "-" + locoId + "<;>r");
}

void backendSendSpeed(int step) {
  if (!locoAcquired) {
    return;
  }
  if (usingLcc()) {
    lcc::setSpeed(step, directionForward);
    return;
  }
  noteSentSpeed(step);
  sendWt(String("M") + WITHROTTLE_THROTTLE_KEY + "A" + locoId + "<;>V" + String(step));
}

void backendSendDirection(bool forward) {
  if (!locoAcquired) {
    return;
  }
  if (usingLcc()) {
    // LCC carries direction in the sign of the speed, so resend the current speed.
    lcc::setSpeed(speed126, forward);
    return;
  }
  sendWt(String("M") + WITHROTTLE_THROTTLE_KEY + "A" + locoId + "<;>R" + String(forward ? 1 : 0));
}

void backendSendFunction(uint8_t fn, bool on) {
  if (!locoAcquired) {
    return;
  }
  if (usingLcc()) {
    lcc::setFunction(fn, on);
    return;
  }
  // 'f' is force-function: it sets the state directly instead of emulating a key press.
  sendWt(String("M") + WITHROTTLE_THROTTLE_KEY + "A" + locoId + "<;>f" + String(on ? 1 : 0) + String(fn));
}

void backendEStop() {
  if (usingLcc()) {
    lcc::eStop();
    return;
  }
  if (locoAcquired) {
    sendWt(String("M") + WITHROTTLE_THROTTLE_KEY + "A" + locoId + "<;>X");
  }
}

void backendQuit() {
  if (usingLcc()) {
    lcc::quit();
    return;
  }
  sendWt("Q");
}

void backendTurnout(const String& id) {
  if (usingLcc()) {
    lcc::turnout(id);
    return;
  }
  sendWt(String("PTA2") + id);
}

// Answer a steal prompt by repeating the request as a steal.
void sendStealRequest() {
  if (usingLcc() || !locoSelected) {
    return;
  }
  sendWt(String("M") + WITHROTTLE_THROTTLE_KEY + "S" + locoId + "<;>" + locoId);
  stealPending = false;
  locoAcquirePending = true;
  locoAcquireSentMs = millis();
  setStatus(String("Stealing ") + locoId + "...");
}

void onLocoAcquired() {
  locoAcquired = true;
  locoAcquirePending = false;
  stealPending = false;
  locoAcquireAttempts = 0;
  uiDirty = true;
  setStatus(String("Loco acquired ") + locoId);
  resyncLocoStateToServer();
}

// Retry or give up on an acquire that the server never confirmed.
void serviceLocoAcquire() {
  if (!locoAcquirePending) {
    return;
  }
  // Under LCC the loco is "acquired" once the train node answers and accepts us as its controller.
  if (usingLcc()) {
    if (lcc::trainAssigned()) {
      onLocoAcquired();
    } else if ((millis() - locoAcquireSentMs) > ACQUIRE_CONFIRM_TIMEOUT_MS * 3) {
      locoAcquirePending = false;
      setStatus(String("No train node for ") + locoId);
    }
    return;
  }
  if ((millis() - locoAcquireSentMs) < ACQUIRE_CONFIRM_TIMEOUT_MS) {
    return;
  }
  locoAcquirePending = false;
  if (locoAcquireAttempts < ACQUIRE_MAX_ATTEMPTS && backendConnected()) {
    setStatus("Acquire timed out, retrying");
    backendAcquire();
    return;
  }
  locoAcquireAttempts = 0;
  setStatus(String("Acquire failed: ") + locoId);
}

// Render the whole UI into `g` (the off-screen canvas normally, the panel directly as a fallback).
void renderUi(LovyanGFX& g) {
  g.fillScreen(BLACK);
  g.setTextColor(WHITE, BLACK);
  const int16_t screenW = g.width();

  auto drawCentered = [&](int16_t y, uint8_t textSize, const String& text) {
    g.setTextSize(textSize);
    const int16_t x = (screenW - g.textWidth(text)) / 2;
    g.setCursor(max<int16_t>(0, x), y);
    g.print(text);
  };

  const int16_t cx = screenW / 2;
  const int16_t cy = g.height() / 2;
  const int16_t ringR = 114;
  const uint16_t ringColor = brakeHoldActive ? RED : (brakeRecovering ? ORANGE : DARKGREY);
  g.drawCircle(cx, cy, ringR, ringColor);
  g.drawCircle(cx, cy, ringR - 1, ringColor);

  const uint16_t activeBarColor = directionForward ? 0x07FF : 0xFD20;

  // Eight large notch blocks around the dial. A block is outlined white when its notch is selected;
  // the actual speed fills the blocks progressively in the direction color, so with momentum the fill
  // grows toward the outlined notch and shrinks when coasting or braking.
  const int16_t blockInnerR = 94;
  const int16_t blockOuterR = 112;
  const float notchSpanDeg = 360.0f / notchCount();
  const float notchGapDeg = 5.0f;
  const float speedDeg = 360.0f * static_cast<float>(speed126) / 126.0f;
  const uint16_t emptyFill = 0x2104;
  const uint16_t selectedBorder = WHITE;
  const uint16_t idleBorder = 0x4208;
  // Angles here are clockwise from the top; fillArc wants clockwise from 3 o'clock.
  auto arc = [&](int32_t r0, int32_t r1, float degFrom, float degTo, uint16_t color) {
    if (degTo <= degFrom) {
      return;
    }
    g.fillArc(cx, cy, r0, r1, degFrom + 270.0f, degTo + 270.0f, color);
  };
  for (int n = 0; n < notchCount(); ++n) {
    const float a0 = n * notchSpanDeg + notchGapDeg / 2.0f;
    const float a1 = (n + 1) * notchSpanDeg - notchGapDeg / 2.0f;
    // Half-step: outline the block being moved into (or out of) in grey.
    const bool halfBlock = (encoderPendingDetents > 0 && n == throttleNotch) ||
                           (encoderPendingDetents < 0 && n == throttleNotch - 1);
    // Blocks above the reverse cap are drawn dimmer, so it is obvious why the dial stops early.
    const uint16_t outOfRangeBorder = 0x2104;
    const uint16_t border = (n >= maxNotch())
                                ? outOfRangeBorder
                                : (halfBlock ? YELLOW
                                             : ((n < throttleNotch) ? selectedBorder : idleBorder));
    // Border-coloured block, then an inset interior: 2-3 arc fills per block instead of 6.
    const float i0 = a0 + 1.2f;
    const float i1 = a1 - 1.2f;
    arc(blockInnerR, blockOuterR, a0, a1, border);
    arc(blockInnerR + 2, blockOuterR - 2, i0, i1, emptyFill);
    if (speedDeg > i0) {
      arc(blockInnerR + 2, blockOuterR - 2, i0, min(i1, speedDeg), activeBarColor);
    }
  }

  if (estopLatched) {
    g.fillRoundRect(40, 8, screenW - 80, 18, 8, RED);
    g.setTextColor(WHITE, RED);
    drawCentered(11, 1, "E-STOP LATCHED");
    g.setTextColor(WHITE, BLACK);
  }

  // Top indicator row: power source / charging tag plus battery gauge, centered as one group.
  {
    String tag;
    uint16_t tagColor = 0x07FF;
    if (chargeState == ChargeState::Charging) {
      tag = "CHG";
      tagColor = YELLOW;
    } else if (powerSource == PowerSource::External) {
      tag = usbHostPresent ? "USB" : "PWR";
    }

    g.setTextSize(1);
    const int16_t by = 31;
    const int16_t bw = 22;
    const int16_t bh = 10;
    const int16_t tagW = tag.isEmpty() ? 0 : (g.textWidth(tag) + 6);
    String pctText;
    int16_t gaugeW = 0;
    if (batteryPercent >= 0) {
      pctText = String(batteryPercent) + "%";
      if (BATTERY_SHOW_VOLTAGE && batteryMillivolts > 0) {
        pctText += " " + String(batteryMillivolts / 1000.0f, 2) + "V";
      }
      gaugeW = bw + 2 + 4 + g.textWidth(pctText);
    }

    int16_t x = (screenW - (tagW + gaugeW)) / 2;
    if (!tag.isEmpty()) {
      g.setTextColor(tagColor, BLACK);
      g.setCursor(x, by + 1);
      g.print(tag);
      x += tagW;
    }
    if (batteryPercent >= 0) {
      const uint16_t batColor =
          batteryPercent > 40 ? 0x07E0 : (batteryPercent > BATTERY_LOW_WARN_PERCENT ? YELLOW : RED);
      g.drawRoundRect(x, by, bw, bh, 2, WHITE);
      g.fillRect(x + bw, by + 3, 2, bh - 6, WHITE);
      const int16_t fillW = (bw - 4) * batteryPercent / 100;
      if (fillW > 0) {
        g.fillRect(x + 2, by + 2, fillW, bh - 4, batColor);
      }
      g.setTextColor(WHITE, BLACK);
      g.setCursor(x + bw + 6, by + 1);
      g.print(pctText);
    }
    g.setTextColor(WHITE, BLACK);
  }

  const String addrText = locoSelected ? locoId : "-";
  g.setTextSize(2);
  const int16_t addrW = g.textWidth(addrText) + 16;
  const int16_t addrX = (screenW - addrW) / 2;
  g.fillRoundRect(addrX, 46, addrW, 18, 4, WHITE);
  g.setTextColor(BLACK, WHITE);
  g.setCursor(addrX + 8, 48);
  g.print(addrText);
  g.setTextColor(WHITE, BLACK);

  // Big actual speed step, with the selected notch underneath (they differ while ramping or braking).
  drawCentered(70, 6, String(speed126));
  g.setTextColor(0xC618, BLACK);
  drawCentered(124, 1, String("Notch ") + String(throttleNotch) + "/" + String(maxNotch()));
  g.setTextColor(WHITE, BLACK);

  // Draw a font-independent direction arrow so it renders on all built-in fonts.
  const int16_t ay = 152;
  const int16_t shaftHalf = 20;
  if (directionForward) {
    g.drawLine(cx - shaftHalf, ay, cx + shaftHalf, ay, WHITE);
    g.drawLine(cx - shaftHalf, ay - 1, cx + shaftHalf, ay - 1, WHITE);
    g.fillTriangle(cx + shaftHalf + 1, ay, cx + shaftHalf - 7, ay - 7, cx + shaftHalf - 7,
                                ay + 7, WHITE);
  } else {
    g.drawLine(cx + shaftHalf, ay, cx - shaftHalf, ay, WHITE);
    g.drawLine(cx + shaftHalf, ay - 1, cx - shaftHalf, ay - 1, WHITE);
    g.fillTriangle(cx - shaftHalf - 1, ay, cx - shaftHalf + 7, ay - 7, cx - shaftHalf + 7,
                                ay + 7, WHITE);
  }

  // Function latch chips split left/right of speed. Common functions get letter icons.
  // Draw simple bitmapped icons for functions (no text labels).
  auto drawFnChip = [&](int fn, int16_t x, int16_t y) {
    const bool on = functionState[fn];
    const uint16_t fill = on ? 0x07E0 : 0x2104;      // Green or gray fill
    const uint16_t border = on ? 0xAFE5 : 0x7BEF;    // Bright or dim border
    const uint16_t iconColor = on ? BLACK : 0xC618;  // Black or muted gold
    
    g.fillRoundRect(x - 10, y - 8, 20, 16, 4, fill);
    g.drawRoundRect(x - 10, y - 8, 20, 16, 4, border);
    
    // Draw icon based on function number using simple line/circle drawing.
    switch (fn) {
      case 0: { // Light: small circle with rays
        g.fillCircle(x, y, 2, iconColor);  // bulb center
        g.drawLine(x - 4, y - 4, x - 3, y - 5, iconColor); // top-left ray
        g.drawLine(x + 4, y - 4, x + 3, y - 5, iconColor); // top-right ray
        break;
      }
      case 1: { // Bell: inverted V with dots
        g.drawLine(x - 3, y - 3, x, y, iconColor);
        g.drawLine(x, y, x + 3, y - 3, iconColor);
        g.fillCircle(x - 1, y + 1, 1, iconColor); // left dot
        g.fillCircle(x + 1, y + 1, 1, iconColor); // right dot
        break;
      }
      case 2: { // Horn: angle bracket >
        g.drawLine(x - 4, y - 3, x + 2, y, iconColor);
        g.drawLine(x + 2, y, x - 4, y + 3, iconColor);
        g.drawLine(x - 2, y - 2, x + 3, y + 1, iconColor); // double-line effect
        break;
      }
      case 3: { // Smoke: small clouds (3 overlapping circles)
        g.drawCircle(x - 2, y, 2, iconColor);
        g.drawCircle(x, y + 1, 2, iconColor);
        g.drawCircle(x + 2, y, 2, iconColor);
        break;
      }
      default: { // Generic: just show number
        g.setTextSize(1);
        g.setTextColor(iconColor, fill);
        const String num = String(fn);
        g.setCursor(x - 2, y - 3);
        g.print(num);
        break;
      }
    }
  };

  // Arrange function icons in two curved columns following the ring shape.
  // Left column: angles 150-210° (left arc), Right column: angles -30 to 30° (right arc)
  // Radius 80 brings them closer to center while following the ring curve.
  const int16_t ringCx = 120;
  const int16_t ringCy = 120;
  const int16_t curveRadius = 80;
  
  // Left column: F0, F1, F2, F3, E-Stop
  const float leftAngles[5] = {150.0f, 165.0f, 180.0f, 195.0f, 210.0f};
  for (int i = 0; i < 5; ++i) {
    const float angle = leftAngles[i] * DEG_TO_RAD;
    const int16_t x = static_cast<int16_t>(ringCx + curveRadius * cosf(angle));
    const int16_t y = static_cast<int16_t>(ringCy + curveRadius * sinf(angle));
    if (i < 4) {
      drawFnChip(i, x, y);
    } else {
      // Draw E-Stop at position 4 (210°)
      const uint16_t estopFill = estopLatched ? RED : 0x2104;
      const uint16_t estopBorder = estopLatched ? 0xFB00 : 0x7BEF;
      const uint16_t estopIcon = estopLatched ? WHITE : 0xC618;
      g.fillRoundRect(x - 10, y - 8, 20, 16, 4, estopFill);
      g.drawRoundRect(x - 10, y - 8, 20, 16, 4, estopBorder);
      g.setTextSize(1);
      g.setTextColor(estopIcon, estopFill);
      g.setCursor(x - 2, y - 3);
      g.print("E");
    }
  }
  
  // Right column: F4-F8
  const float rightAngles[5] = {-30.0f, -15.0f, 0.0f, 15.0f, 30.0f};
  for (int i = 0; i < 5; ++i) {
    const float angle = rightAngles[i] * DEG_TO_RAD;
    const int16_t x = static_cast<int16_t>(ringCx + curveRadius * cosf(angle));
    const int16_t y = static_cast<int16_t>(ringCy + curveRadius * sinf(angle));
    drawFnChip(4 + i, x, y);
  }

  g.setTextColor(WHITE, BLACK);

  // Status lines are clipped to the chord width inside the notch blocks so they never run into them.
  auto drawClipped = [&](int16_t y, const String& text) {
    const float dy = static_cast<float>(y + 4 - cy);
    const float half = sqrtf(max(0.0f, 91.0f * 91.0f - dy * dy));
    const int16_t maxW = static_cast<int16_t>(2.0f * half) - 4;
    g.setTextSize(1);
    String t = text;
    while (!t.isEmpty() && g.textWidth(t) > maxW) {
      t.remove(t.length() - 1);
    }
    drawCentered(y, 1, t);
  };
  drawClipped(176, statusLine);
  if (!infoLine.isEmpty()) {
    drawClipped(194, infoLine);
  } else {
    // Standing hint telling the operator how to reach the settings menu. It sits a little higher
    // than the info line it replaces, because the longer label needs the wider chord to clear the
    // notch blocks.
    g.setTextColor(0x8410, BLACK);
    drawClipped(190, "Btn Hold = Setup");
    g.setTextColor(WHITE, BLACK);
  }
}

void drawUi(bool force) {
  if (!force && !uiDirty) {
    return;
  }
  const unsigned long nowMs = millis();
  if (!force && (nowMs - lastDrawMs) < UI_MIN_REDRAW_INTERVAL_MS) {
    return; // keep uiDirty set: the frame lands on a later pass, inputs stay responsive
  }
  lastDrawMs = nowMs;
  uiDirty = false;
  const uint32_t t0 = micros();
  if (uiCanvasReady) {
    if (settings::isOpen()) {
      settings::render(uiCanvas);
    } else {
      renderUi(uiCanvas);
    }
    lastRenderUsec = micros() - t0;
    uiCanvas.pushSprite(0, 0); // one blit per frame: no clear-then-draw flicker
  } else {
    if (settings::isOpen()) {
      settings::render(M5Dial.Display);
    } else {
      renderUi(M5Dial.Display);
    }
    lastRenderUsec = micros() - t0;
  }
  lastDrawUsec = micros() - t0;
}

void setSpeed(int newSpeed) {
  if (newSpeed < 0) {
    newSpeed = 0;
  }
  if (newSpeed > 126) {
    newSpeed = 126;
  }

  // Keep throttle at zero until the operator explicitly clears the latch.
  if (estopLatched && newSpeed > 0) {
    setStatus("E-Stop latched: press BtnA to clear");
    return;
  }

  if (newSpeed == speed126) {
    return;
  }

  speed126 = newSpeed;
  uiDirty = true;
  if (newSpeed > 0) {
    ensureLocoAcquired();
  }
  if (!encoderUpdatingSpeed) {
    throttleDemandSpeed = newSpeed;
    throttleNotch = speedToNotch(newSpeed);
    syncEncoderToSpeed();
  }
  backendSendSpeed(speed126);
}

void setDirection(bool forward) {
  if (directionForward == forward) {
    return;
  }
  directionForward = forward;
  uiDirty = true;
  ensureLocoAcquired();
  backendSendDirection(directionForward);
  // The button only flips direction at a stand, but the serial `dir` command and an inbound
  // WiThrottle direction change can arrive while moving, so bring the notch inside the new limit.
  if (throttleNotch > maxNotch()) {
    applyNotch(maxNotch(), false);
  }
}

void toggleDirection() {
  setDirection(!directionForward);
}

void emergencyStop() {
  estopLatched = true;
  setSpeed(0);
  ensureLocoAcquired();
  backendEStop();
  setStatus("E-Stop sent (latched)");
  beep();
}

void clearEmergencyStopLatch() {
  if (!estopLatched) {
    setStatus("E-Stop not latched");
    return;
  }
  estopLatched = false;
  setStatus("E-Stop latch cleared");
}

void applyStopAction(const char* statusText) {
  brakeHoldActive = false;
  brakeRecovering = false;
  brakeRecoveryTargetSpeed = 0;
  throttleDemandSpeed = 0;
  throttleNotch = 0;
  encoderUpdatingSpeed = true;
  setSpeed(0);
  encoderUpdatingSpeed = false;
  syncEncoderToSpeed();
  setStatus(statusText);
}

void setFunction(uint8_t fn, bool on) {
  if (fn > 28) {
    return;
  }
  functionState[fn] = on;
  uiDirty = true;
  ensureLocoAcquired();
  backendSendFunction(fn, on);
}

void toggleFunction(uint8_t fn) {
  if (fn > 28) {
    return;
  }
  setFunction(fn, !functionState[fn]);
}

void acquireLoco() {
  if (!locoSelected) {
    setStatus("Select loco first");
    return;
  }
  backendAcquire();
}

void ensureLocoAcquired() {
  if (!locoSelected || locoAcquired || locoAcquirePending) {
    return;
  }
  acquireLoco();
}

void releaseLoco() {
  backendRelease();
  locoAcquired = false;
  locoAcquirePending = false;
  stealPending = false;
  setStatus("Loco released");
}

// After the server confirms the loco, push the throttle's current state so a reconnect restores it.
void resyncLocoStateToServer() {
  backendSendDirection(directionForward);
  backendSendSpeed(speed126);
  for (uint8_t fn = 0; fn <= 28; ++fn) {
    if (functionState[fn]) {
      backendSendFunction(fn, true);
    }
  }
}

// One property notification from the server, e.g. "V23", "R1", "F10", "s1".
void handleThrottleProperty(const String& payload) {
  if (payload.isEmpty()) {
    return;
  }
  const char kind = payload[0];
  const String arg = payload.substring(1);

  switch (kind) {
    case 'V': {
      const int v = arg.toInt();
      // A negative speed means the command station has this address in emergency stop.
      if (v < 0) {
        estopLatched = true;
        speed126 = 0;
        throttleNotch = 0;
        throttleDemandSpeed = 0;
        encoderPendingDetents = 0;
        syncEncoderToSpeed();
        uiDirty = true;
        setStatus("Server reports E-Stop");
        return;
      }
      if (v > 126) {
        return;
      }
      if (consumeSpeedEcho(v)) {
        return; // our own command coming back
      }
      // Another throttle (or JMRI itself) moved this loco: follow it.
      speed126 = v;
      if (!brakeHoldActive && !brakeRecovering) {
        throttleNotch = speedToNotch(v);
        throttleDemandSpeed = settings::cfg.momentum ? notchToSpeed(throttleNotch) : v;
        encoderPendingDetents = 0;
        syncEncoderToSpeed();
        lastMomentumMs = millis();
      }
      uiDirty = true;
      return;
    }
    case 'R': {
      const bool forward = arg.toInt() != 0;
      if (forward != directionForward) {
        directionForward = forward;
        uiDirty = true;
      }
      return;
    }
    case 'F': {
      // F<state><number>: the server's report of a function's current state.
      if (arg.length() < 2) {
        return;
      }
      const bool on = (arg[0] == '1');
      const int fn = arg.substring(1).toInt();
      if (fn < 0 || fn > 28) {
        return;
      }
      if (functionState[fn] != on) {
        functionState[fn] = on;
        uiDirty = true;
      }
      return;
    }
    default:
      return; // 's' speed step mode, 'm' momentary and others are not used here
  }
}

void parseServerLine(const String& line) {
  if (line.isEmpty()) {
    return;
  }

  Serial.print("< ");
  Serial.println(line);

  if (line.startsWith("*")) {
    setInfo("Heartbeat window: " + line.substring(1) + "s");
    return;
  }

  if (line.startsWith("VN")) {
    serverProtocolVersion = line.substring(2).toFloat();
    setInfo(String("WiThrottle v") + line.substring(2));
    return;
  }

  // HM is an alert (usually an error), Hm is informational.
  if (line.startsWith("HM") || line.startsWith("Hm")) {
    setStatus(line.substring(2));
    if (line.startsWith("HM") && locoAcquirePending) {
      locoAcquirePending = false;
      locoAcquired = false;
      locoAcquireAttempts = 0;
    }
    return;
  }

  // Multi-throttle notification: M<key><type><locoId><;><property>
  if (line.length() > 3 && line[0] == 'M' && line[1] == WITHROTTLE_THROTTLE_KEY) {
    const int sep = line.indexOf("<;>");
    if (sep < 3) {
      return;
    }
    const char type = line[2];
    const String key = line.substring(3, sep);
    const String payload = line.substring(sep + 3);
    if (key != locoId && key != "*") {
      return; // a different locomotive on this throttle
    }

    switch (type) {
      case '+': // add confirmed
        if (!locoAcquired) {
          onLocoAcquired();
        }
        return;
      case '-': // the server dropped the loco
        if (locoAcquired || locoAcquirePending) {
          locoAcquired = false;
          locoAcquirePending = false;
          uiDirty = true;
          setStatus(String("Loco released ") + locoId);
        }
        return;
      case 'S': // in use elsewhere: the server is asking whether to steal it
        locoAcquirePending = false;
        if (WITHROTTLE_AUTO_STEAL) {
          sendStealRequest();
        } else {
          stealPending = true;
          setStatus(String("In use. Serial 'steal' to take ") + locoId);
        }
        return;
      case 'A': // action / property notification
        handleThrottleProperty(payload);
        return;
      case 'L': // function label list
      default:
        return;
    }
  }
}

void readServer() {
  if (SERIAL_OUTPUT_ONLY) {
    return;
  }

  static String rx;

  while (wtClient.connected() && wtClient.available()) {
    const char c = static_cast<char>(wtClient.read());
    if (c == '\n' || c == '\r') {
      if (!rx.isEmpty()) {
        parseServerLine(rx);
        rx = "";
      }
      continue;
    }
    rx += c;
    if (rx.length() > 300) {
      rx = "";
    }
  }
}

void onWiFiConnected() {
  wifiConnecting = false;
  wifiPowerSaveApplied = -1;
  WiFi.setSleep(ENABLE_WIFI_MODEM_SLEEP);
  if (ENABLE_WIFI_LOW_TX_POWER) {
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
  }
  applyWiFiPowerSave();
  setStatus(String("WiFi OK: ") + WiFi.localIP().toString());
}

void startWiFiConnect() {
  setStatus("Connecting WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(settings::cfg.ssid, settings::cfg.pass);
  wifiConnecting = true;
  wifiConnectStartMs = millis();
}

void connectWiThrottle();

// Non-blocking WiFi/WiThrottle maintenance from loop(): the UI stays responsive after a wake.
void serviceWiFi() {
  if (SERIAL_OUTPUT_ONLY || wifiSuspendedForPower) {
    return;
  }
  const unsigned long now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (wifiConnecting) {
      onWiFiConnected();
    }
    if (usingLcc()) {
      return; // the LCC link is serviced separately, and has no WiThrottle session
    }
    if (!wtClient.connected() && (locoAcquired || locoAcquirePending)) {
      locoAcquired = false; // the session is gone; re-acquire before sending anything
      locoAcquirePending = false;
      uiDirty = true;
    }
    if (!wtClient.connected() && static_cast<long>(now - wtNextRetryMs) >= 0) {
      connectWiThrottle();
      if (wtClient.connected()) {
        wtRetryIntervalMs = 5000;
      } else {
        wtRetryIntervalMs = min<uint32_t>(wtRetryIntervalMs * 2, 30000);
      }
      wtNextRetryMs = millis() + wtRetryIntervalMs;
    }
    return;
  }

  if (wifiConnecting) {
    if ((now - wifiConnectStartMs) >= 30000) {
      wifiConnecting = false;
      wifiNextRetryMs = now + 10000;
      setStatus("WiFi connect failed");
    }
    return;
  }

  if (static_cast<long>(now - wifiNextRetryMs) >= 0) {
    startWiFiConnect();
  }
}

void connectWiThrottle() {
  if (SERIAL_OUTPUT_ONLY) {
    setStatus("Serial debug only mode");
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  setStatus(String("Connecting ") + settings::ipString() + ":" + String(settings::cfg.port));
  const IPAddress serverIp(settings::cfg.ip[0], settings::cfg.ip[1], settings::cfg.ip[2],
                           settings::cfg.ip[3]);
  if (!wtClient.connect(serverIp, settings::cfg.port,
                        static_cast<int32_t>(WITHROTTLE_CONNECT_TIMEOUT_MS))) {
    setStatus("WiThrottle connect failed");
    return;
  }

  const String uniqueId = String(THROTTLE_ID_PREFIX) + WiFi.macAddress();

  sendWt(String("N") + THROTTLE_NAME);
  sendWt(String("HU") + uniqueId);
  sendWt("*+");

  // The session is new, so nothing is acquired yet. Speed, direction and functions are pushed by
  // resyncLocoStateToServer() once the server confirms the loco, which is what restores the
  // throttle after a reconnect.
  locoAcquired = false;
  locoAcquirePending = false;
  locoAcquireAttempts = 0;
  if (locoSelected) {
    backendAcquire();
  }

  setStatus("WiThrottle connected");
}

// Detent-aligned decoding: the raw count must move a full detent from the reference before anything
// is accepted, so chatter never registers. settings::cfg.clicksPerNotch detents of net travel in one
// direction make one notch; turning back cancels the pending travel, like a gated notch lever.
void handleEncoder() {
  const int32_t raw = M5Dial.Encoder.read() * encoderSign();
  const unsigned long now = millis();
  const int32_t perDetent = ENCODER_RAW_COUNTS_PER_STEP;

  if (!encoderStateInitialized || encoderResyncPending) {
    encoderDetentRef = raw;
    encoderLastRaw = raw;
    encoderLastMotionMs = now;
    encoderPendingDetents = 0;
    encoderStateInitialized = true;
    encoderResyncPending = false;
    return;
  }

  if (raw != encoderLastRaw) {
    encoderLastRaw = raw;
    encoderLastMotionMs = now;
  }

  if (encoderPendingDetents != 0 && ENCODER_PARTIAL_NOTCH_TIMEOUT_MS > 0 &&
      (now - lastDetentMs) >= ENCODER_PARTIAL_NOTCH_TIMEOUT_MS) {
    encoderPendingDetents = 0;
    uiDirty = true;
  }

  const int32_t delta = raw - encoderDetentRef;
  if (delta > -perDetent && delta < perDetent) {
    // Less than a detent away. Once the dial has rested, adopt the current count as the reference
    // so accumulated drift plus chatter cannot reach a full detent later.
    if (delta != 0 && (now - encoderLastMotionMs) >= ENCODER_REST_REALIGN_MS) {
      encoderDetentRef = raw;
    }
    return;
  }

  const int8_t dir = (delta > 0) ? 1 : -1;
  const unsigned long sinceLastDetent = now - lastDetentMs;

  // Reversal right after a detent is a miscount, not the operator turning back: absorb it.
  if (lastDetentDir != 0 && dir != lastDetentDir && sinceLastDetent < ENCODER_REVERSAL_PAUSE_MS) {
    encoderDetentRef = raw;
    return;
  }
  // Same-direction detent arriving implausibly soon: bounce or double count, absorb it.
  if (sinceLastDetent < ENCODER_DETENT_MIN_INTERVAL_MS) {
    encoderDetentRef = raw;
    return;
  }

  noteUserActivity("encoder");
  encoderDetentRef += dir * perDetent; // one detent per pass; a fast flick catches up on later passes
  lastDetentMs = now;
  lastDetentDir = dir;

  if (settings::isOpen()) {
    settings::encoder(dir);
    uiDirty = true;
    return;
  }

  // Net travel toward the next notch. Turning back subtracts, so up-then-down sits still.
  encoderPendingDetents += dir;
  uiDirty = true; // show the half-step outline
  if (abs(encoderPendingDetents) < settings::cfg.clicksPerNotch) {
    return;
  }
  encoderPendingDetents = 0;

  int newNotch = throttleNotch + dir;
  if (newNotch < 0) {
    newNotch = 0;
  } else if (newNotch > maxNotch()) {
    newNotch = maxNotch();
  }
  if (newNotch == throttleNotch) {
    encoderDetentRef = raw; // at an end stop: don't bank travel
    return;
  }
  applyNotch(newNotch, true);
}

void handleTouchBrake() {
  if (settings::isOpen()) {
    return;
  }
  auto t = M5Dial.Touch.getDetail();
  const bool touching = (t.state == m5::touch_state_t::touch ||
                         t.state == m5::touch_state_t::touch_begin ||
                         t.state == m5::touch_state_t::hold ||
                         t.state == m5::touch_state_t::hold_begin ||
                         t.state == m5::touch_state_t::drag ||
                         t.state == m5::touch_state_t::drag_begin);
  const unsigned long now = millis();
  if (touching || t.state == m5::touch_state_t::touch_end || t.state == m5::touch_state_t::hold_end) {
    noteUserActivity("touch");
  }

  if (touching && !brakeHoldActive && (now - lastBrakeTouchMs) > BRAKE_TOUCH_DEBOUNCE_MS) {
    brakeHoldActive = true;
    brakeRecovering = false;
    brakeStoppedToZero = false;
    brakeRecoveryTargetSpeed = throttleDemandSpeed;
    lastBrakeStepMs = now;
    lastBrakeTouchMs = now;
    setStatus("Brake hold");
  }

  if (brakeHoldActive && touching) {
    brakeRecoveryTargetSpeed = throttleDemandSpeed;
    if ((now - lastBrakeStepMs) >= BRAKE_TICK_MS) {
      lastBrakeStepMs = now;
      encoderUpdatingSpeed = true;
      setSpeed(speed126 - BRAKE_DECEL_PER_TICK);
      encoderUpdatingSpeed = false;
      if (speed126 <= 0) {
        brakeStoppedToZero = true;
      }
    }
    return;
  }

  if (brakeHoldActive && !touching) {
    if (brakeStoppedToZero && speed126 == 0) {
      brakeStoppedToZero = false;
      applyStopAction("Brake hold stop");
      return;
    }
    brakeStoppedToZero = false;
    brakeHoldActive = false;
    if (settings::cfg.momentum) {
      brakeRecovering = false;
      lastMomentumMs = now;
      momentumAccumMilli = 0;
      setStatus("Brake release");
      return;
    }
    brakeRecovering = true;
    lastBrakeStepMs = now;
    setStatus("Brake release");
  }

  if (brakeRecovering && !brakeHoldActive) {
    brakeRecoveryTargetSpeed = throttleDemandSpeed;
    if (speed126 == brakeRecoveryTargetSpeed) {
      brakeRecovering = false;
      return;
    }
    if ((now - lastBrakeStepMs) >= BRAKE_TICK_MS) {
      lastBrakeStepMs = now;
      const int dir = (speed126 < brakeRecoveryTargetSpeed) ? 1 : -1;
      encoderUpdatingSpeed = true;
      setSpeed(speed126 + dir * BRAKE_ACCEL_PER_TICK);
      encoderUpdatingSpeed = false;
    }
  }
}

// A quick press: activate in the settings screen, otherwise stop / change direction.
void buttonShortAction() {
  if (settings::isOpen()) {
    settings::press();
    uiDirty = true;
    return;
  }
  if (estopLatched) {
    clearEmergencyStopLatch();
    return;
  }
  if (speed126 > 0 || throttleDemandSpeed > 0) {
    applyStopAction("Button stop");
    return;
  }
  toggleDirection();
  setStatus(directionForward ? "Direction FWD" : "Direction REV");
}

// A long press: open the settings screen, or back out of it.
void buttonLongAction() {
  if (settings::isOpen()) {
    settings::longPress();
  } else if (speed126 > 0 || throttleDemandSpeed > 0) {
    setStatus("Stop the train first");
    return;
  } else {
    settings::open();
  }
  uiDirty = true;
}

void handleButton() {
  if (M5Dial.BtnA.wasPressed()) {
    noteUserActivity("button");
    buttonLongHandled = false;
  }

  // Holding the button opens the settings screen, and backs out of it again.
  if (!buttonLongHandled && M5Dial.BtnA.isPressed() && M5Dial.BtnA.pressedFor(SETTINGS_LONG_PRESS_MS)) {
    buttonLongHandled = true;
    noteUserActivity("button");
    buttonLongAction();
    return;
  }

  if (!M5Dial.BtnA.wasReleased()) {
    return;
  }
  noteUserActivity("button");
  if (buttonLongHandled) {
    buttonLongHandled = false; // the hold already did the work
    return;
  }

  buttonShortAction();
}

void handleKeypad() {
  const unsigned long now = millis();
  if (!keypadPresent) {
    // Retry detection occasionally so a keypad plugged in after boot still works.
    if ((now - lastKeypadPollMs) < 10000) {
      return;
    }
    lastKeypadPollMs = now;
    keypadPresent = keypad.begin(Wire, KEYPAD_I2C_ADDR);
    if (keypadPresent) {
      setStatus("Qwiic keypad connected");
    }
    return;
  }

  // The keypad buffers presses in its own FIFO, so poll on a schedule instead of every loop pass.
  const uint32_t interval = (backlightMode == 0) ? KEYPAD_POLL_ACTIVE_MS : KEYPAD_POLL_IDLE_MS;
  if ((now - lastKeypadPollMs) < interval) {
    return;
  }
  lastKeypadPollMs = now;

  keypad.updateFIFO();
  const uint8_t k = keypad.getButton();
  if (k == 0x00 || k == 0xFF) {
    return;
  }
  lastKeypadPollMs = 0; // drain any further buffered keys on the next pass
  noteUserActivity("keypad");

  const char key = static_cast<char>(k);
  setInfo(String("Key: ") + key);

  if (settings::isOpen()) {
    if (key >= '0' && key <= '9') {
      settings::digit(key);
    } else if (key == '#') {
      settings::press();
    } else if (key == '*') {
      settings::longPress();
    }
    uiDirty = true;
    return;
  }

  if (keypadTurnoutMode) {
    if (key >= '0' && key <= '9') {
      if (keypadTurnoutBuffer.length() < 8) {
        keypadTurnoutBuffer += key;
      }
      setStatus(String("Turnout: ") + keypadTurnoutBuffer);
      return;
    }

    if (key == '*') {
      if (keypadTurnoutBuffer.isEmpty()) {
        keypadTurnoutMode = false;
        setStatus("Turnout mode canceled");
        return;
      }
      backendTurnout(keypadTurnoutBuffer);
        beep();
      setStatus(String("Turnout flip ") + keypadTurnoutBuffer);
      keypadTurnoutMode = false;
      keypadTurnoutBuffer = "";
      return;
    }

    if (key == '#') {
      keypadTurnoutMode = false;
      keypadTurnoutBuffer = "";
      setStatus("Turnout mode canceled");
      return;
    }

    return;
  }

  if (keypadAddressMode) {
    if (key >= '0' && key <= '9') {
      if (keypadAddressBuffer.length() < 4) {
        keypadAddressBuffer += key;
      }
      setStatus(String("Loco input: ") + keypadAddressBuffer);
      return;
    }

    if (key == '*') {
      keypadAddressMode = false;
      keypadAddressBuffer = "";
      setStatus("Loco mode canceled");
      return;
    }

    if (key == '#') {
      if (keypadAddressBuffer.isEmpty()) {
        keypadAddressMode = false;
        setStatus("Addr input canceled");
        return;
      }
      const uint16_t addr = static_cast<uint16_t>(keypadAddressBuffer.toInt());
      const bool isLong = addr > 127;
      keypadAddressMode = false;
      setActiveLoco(addr, isLong, true, "KEYPAD");
      keypadAddressBuffer = "";
      return;
    }

    return;
  }

  switch (key) {
    case '1':
      toggleFunction(1);
      break;
    case '2':
      toggleFunction(2);
      break;
    case '3':
      toggleFunction(3);
      break;
    case '4':
      toggleFunction(4);
      break;
    case '5':
      toggleFunction(5);
      break;
    case '6':
      toggleFunction(6);
      break;
    case '7':
      toggleFunction(7);
      break;
    case '8':
      toggleFunction(8);
      break;
    case '9':
      emergencyStop();
      break;
    case '0':
      toggleFunction(0);
      break;
    case '*':
      keypadTurnoutMode = true;
      keypadTurnoutBuffer = "";
      setStatus("Turnout mode: digits then *=flip");
      break;
    case '#':
      keypadAddressMode = true;
      keypadAddressBuffer = "";
      setStatus("Loco mode: digits then #=select");
      break;
    default:
      break;
  }
}

bool parseLocoIdString(const String& rawLocoId, uint16_t& outAddress, bool& outIsLong) {
  String text = rawLocoId;
  text.trim();
  text.toUpperCase();
  if (text.isEmpty()) {
    return false;
  }

  bool hasTypePrefix = false;
  bool parsedIsLong = false;
  if (text[0] == 'S' || text[0] == 'L') {
    hasTypePrefix = true;
    parsedIsLong = (text[0] == 'L');
    text = text.substring(1);
    text.trim();
  }

  if (text.isEmpty()) {
    return false;
  }
  for (size_t i = 0; i < text.length(); ++i) {
    if (!isDigit(text[i])) {
      return false;
    }
  }

  const long addr = text.toInt();
  if (addr <= 0 || addr > 9999) {
    return false;
  }

  outAddress = static_cast<uint16_t>(addr);
  outIsLong = hasTypePrefix ? parsedIsLong : (outAddress > 127);
  return true;
}

bool parseMysqlBoolField(const char* value, bool& outValue) {
  if (value == nullptr) {
    return false;
  }

  String text(value);
  text.trim();
  text.toLowerCase();
  if (text == "1" || text == "true" || text == "t" || text == "yes" || text == "y" || text == "long" ||
      text == "l") {
    outValue = true;
    return true;
  }
  if (text == "0" || text == "false" || text == "f" || text == "no" || text == "n" || text == "short" ||
      text == "s") {
    outValue = false;
    return true;
  }
  return false;
}

bool lookupRfidLocoMysql(const String& uidHex, uint16_t& outAddress, bool& outIsLong) {
  if (!ENABLE_RFID_MYSQL_LOOKUP || SERIAL_OUTPUT_ONLY || WiFi.status() != WL_CONNECTED) {
    return false;
  }

  IPAddress dbHost;
  const String dbHostText = RFID_MYSQL_FOLLOW_SERVER_IP ? settings::ipString() : String(RFID_MYSQL_HOST);
  if (!dbHost.fromString(dbHostText)) {
    Serial.println("RFID DB lookup skipped: RFID_MYSQL_HOST must be an IP address");
    return false;
  }

  WiFiClient mysqlClient;
  MySQL_Connection mysqlConn((Client*)&mysqlClient);

  if (!mysqlConn.connect(dbHost, RFID_MYSQL_PORT, const_cast<char*>(RFID_MYSQL_USER),
                         const_cast<char*>(RFID_MYSQL_PASS))) {
    Serial.println("RFID DB lookup failed: MySQL connect failed");
    return false;
  }

  bool found = false;
  bool parsed = false;
  bool parsedLongOverride = false;
  char query[320] = {0};

  if (RFID_MYSQL_IS_LONG_COLUMN != nullptr) {
    snprintf(query, sizeof(query), "SELECT %s, %s FROM %s.%s WHERE %s='%s' LIMIT 1", RFID_MYSQL_LOCO_COLUMN,
             RFID_MYSQL_IS_LONG_COLUMN, RFID_MYSQL_DB, RFID_MYSQL_TABLE, RFID_MYSQL_UID_COLUMN, uidHex.c_str());
  } else {
    snprintf(query, sizeof(query), "SELECT %s FROM %s.%s WHERE %s='%s' LIMIT 1", RFID_MYSQL_LOCO_COLUMN,
             RFID_MYSQL_DB, RFID_MYSQL_TABLE, RFID_MYSQL_UID_COLUMN, uidHex.c_str());
  }

  MySQL_Cursor cursor(&mysqlConn);
  if (cursor.execute(query)) {
    cursor.get_columns();
    row_values* row = cursor.get_next_row();
    if (row != nullptr && row->values != nullptr && row->values[0] != nullptr) {
      found = true;
      parsed = parseLocoIdString(String(row->values[0]), outAddress, outIsLong);
      if (parsed && RFID_MYSQL_IS_LONG_COLUMN != nullptr && row->values[1] != nullptr) {
        parsedLongOverride = parseMysqlBoolField(row->values[1], outIsLong);
      }
    }
  } else {
    Serial.println("RFID DB lookup failed: query execution error");
  }

  mysqlConn.close();

  if (found && !parsed) {
    Serial.println("RFID DB lookup found row, but loco_id was invalid");
    return false;
  }
  if (found && RFID_MYSQL_IS_LONG_COLUMN != nullptr && !parsedLongOverride) {
    Serial.println("RFID DB lookup note: invalid is_long value, derived from address/type");
  }
  return found && parsed;
}

bool lookupRfidLocoLocalMap(const String& uidHex, uint16_t& outAddress, bool& outIsLong) {
  for (size_t i = 0; i < (sizeof(RFID_LOCO_MAP) / sizeof(RFID_LOCO_MAP[0])); ++i) {
    if (uidHex.equalsIgnoreCase(RFID_LOCO_MAP[i].uidHex)) {
      outAddress = RFID_LOCO_MAP[i].address;
      outIsLong = RFID_LOCO_MAP[i].isLong;
      return true;
    }
  }
  return false;
}

void deriveRfidLocoFromUidTail(const String& uidHex, uint16_t& outAddress, bool& outIsLong) {
  const int start = uidHex.length() > 4 ? uidHex.length() - 4 : 0;
  const String tailHex = uidHex.substring(start);
  const uint32_t raw = static_cast<uint32_t>(strtoul(tailHex.c_str(), nullptr, 16));
  const uint16_t normalized = static_cast<uint16_t>(((raw == 0 ? 1u : raw) - 1u) % 9999u + 1u);
  outAddress = normalized;
  outIsLong = outAddress > 127;
}

String resolveRfidLoco(const String& uidHex, uint16_t& outAddress, bool& outIsLong) {
  if (lookupRfidLocoMysql(uidHex, outAddress, outIsLong)) {
    return "db";
  }

  if (lookupRfidLocoLocalMap(uidHex, outAddress, outIsLong)) {
    return "map";
  }

  deriveRfidLocoFromUidTail(uidHex, outAddress, outIsLong);
  return "tail";
}

void rfidAntennaOff() {
  if (ENABLE_RFID_ANTENNA_DUTY_CYCLE) {
    M5Dial.Rfid.PCD_AntennaOff();
  }
}

void handleRfid() {
  if (!settings::cfg.rfidEnabled) {
    return;
  }
  const unsigned long now = millis();
  const uint32_t interval = (backlightMode == 0) ? RFID_POLL_ACTIVE_MS : RFID_POLL_IDLE_MS;
  if ((now - lastRfidPollMs) < interval) {
    return;
  }
  lastRfidPollMs = now;

  if (ENABLE_RFID_ANTENNA_DUTY_CYCLE) {
    M5Dial.Rfid.PCD_AntennaOn();
    delay(RFID_ANTENNA_SETTLE_MS);
  }
  if (!(M5Dial.Rfid.PICC_IsNewCardPresent() && M5Dial.Rfid.PICC_ReadCardSerial())) {
    rfidAntennaOff();
    return;
  }

  String uid;
  for (byte i = 0; i < M5Dial.Rfid.uid.size; i++) {
    if (M5Dial.Rfid.uid.uidByte[i] < 16) {
      uid += '0';
    }
    uid += String(M5Dial.Rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();

  // A tag resting on the reader re-answers every poll once the field is cycled, so treat the same
  // UID as one event until it has been away for RFID_REPEAT_IGNORE_MS.
  const bool sameTagStillPresent = (uid == lastRfidUid) && ((now - lastRfidSeenMs) < RFID_REPEAT_IGNORE_MS);
  lastRfidSeenMs = now;
  if (sameTagStillPresent) {
    M5Dial.Rfid.PICC_HaltA();
    rfidAntennaOff();
    return;
  }
  noteUserActivity("rfid");
  lastRfidUid = uid;
  lastRfidMs = now;

  uint16_t mappedAddress = 0;
  bool mappedIsLong = false;
  const String source = resolveRfidLoco(uid, mappedAddress, mappedIsLong);

  Serial.printf("RFID UID %s -> %s%u (%s)\n", uid.c_str(), mappedIsLong ? "L" : "S", mappedAddress,
                source.c_str());

  String selectionSource = "RFID-TAIL";
  if (source == "db") {
    selectionSource = "RFID-DB";
  } else if (source == "map") {
    selectionSource = "RFID-MAP";
  }
  setActiveLoco(mappedAddress, mappedIsLong, true, selectionSource);
  M5Dial.Rfid.PICC_HaltA();
  rfidAntennaOff();
}

void printDebugHelp() {
  Serial.println("Debug commands:");
  Serial.println("  help                 - show this help");
  Serial.println("  status               - print WiFi/WiThrottle state");
  Serial.println("  acq                  - acquire configured loco");
  Serial.println("  rel                  - release configured loco");
  Serial.println("  estop                - send emergency stop");
  Serial.println("  clear                - clear E-Stop latch");
  Serial.println("  speed <0-126>        - set speed step");
  Serial.println("  notch <0-8>          - set throttle notch");
  Serial.println("  diag                 - power/input diagnostics (does not reset idle timer)");
  Serial.println("  tier <0|1|2>         - force backlight tier (2 = display off) for testing");
  Serial.println("  shot                 - dump a screenshot of the UI as hex (tools/screenshot.py)");
  Serial.println("  simbat <pct|-1>      - fake a battery percentage on the display (testing)");
  Serial.println("  enc <counts>         - inject raw encoder counts, 2 = one detent (testing)");
  Serial.println("  btn [long]           - fake a dial button press (testing)");
  Serial.println("  dir <f|r|t>          - forward/reverse/toggle");
  Serial.println("  fn <0-28> <on|off|t> - function on/off/toggle");
  Serial.println("  wt                   - reconnect WiThrottle");
  Serial.println("  wifi                 - reconnect WiFi");
  Serial.println("  send <raw>           - send raw WiThrottle line");
  Serial.println("  loco <addr> <s|l>    - set active loco and acquire");
  Serial.println("  settings             - show settings and open the settings screen");
  Serial.println("  set ip <a.b.c.d>     - set and save the server address");
  Serial.println("  set port <n>         - set and save the server port");
  Serial.println("  set proto <wt|lcc>   - choose WiThrottle or LCC GridConnect");
  Serial.println("  steal                - take a loco another throttle holds");
  Serial.println("  wtin <line>          - inject a server line (testing)");
  Serial.println("  lcc                  - LCC/GridConnect status");
  Serial.println("  lccin <frame>        - inject a GridConnect frame (testing)");
  Serial.println("  bat                  - print battery reading");
  Serial.println("  off                  - power off now (dial button / touch wakes)");
  if (SERIAL_OUTPUT_ONLY) {
    Serial.println("  mode                 - SERIAL_OUTPUT_ONLY active");
  }
}

void printPowerSourceStatus() {
  const char* src = powerSource == PowerSource::External ? (usbHostPresent ? "external (USB host)" : "external")
                    : powerSource == PowerSource::Battery ? "battery"
                                                           : "unknown (no USB host; add POWER_SENSE_ADC_PIN to tell battery from charger)";
  const char* chg = chargeState == ChargeState::Charging ? "charging"
                    : chargeState == ChargeState::NotCharging ? "not charging"
                                                               : "unknown (CHARGE_STATUS_PIN not wired)";
  Serial.printf("Power source: %s; %s\n", src, chg);
  if (powerSenseMillivolts >= 0) {
    Serial.printf("5V input sense: %d mV\n", powerSenseMillivolts);
  }
}

void printBatteryStatus() {
  printPowerSourceStatus();
  if (batteryPercent < 0) {
    Serial.println("Battery: no gauge (set BATTERY_ADC_PIN in config.h; the M5Dial has no built-in sense line)");
    return;
  }
  if (batteryMillivolts > 0) {
    Serial.printf("Battery: %d%% (%d mV)\n", batteryPercent, batteryMillivolts);
  } else {
    Serial.printf("Battery: %d%%\n", batteryPercent);
  }
}

void printDebugStatus() {
  Serial.printf("Mode: %s\n", SERIAL_OUTPUT_ONLY ? "serial-only" : "wifi-withrottle");
  Serial.printf("Power: tier %u (%s), wifi %s\n", backlightMode,
                backlightMode == 0 ? "active" : (backlightMode == 1 ? "dim" : "display off"),
                wifiSuspendedForPower ? "suspended" : "on");
  printBatteryStatus();
  if (SERIAL_OUTPUT_ONLY) {
    Serial.printf("WiThrottle: serial-output-only\n");
    Serial.printf("Loco: %s (%s)\n", locoId.c_str(), locoAcquired ? "acquired" : "free");
    Serial.printf("Notch: %d/%d Speed: %d Dir: %s\n", throttleNotch, maxNotch(), speed126,
                directionForward ? "FWD" : "REV");
    return;
  }

  Serial.printf("WiFi: %s\n", WiFi.status() == WL_CONNECTED ? "connected" : "disconnected");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
  }
  Serial.printf("Protocol: %s\n", usingLcc() ? "LCC GridConnect" : "WiThrottle");
  if (usingLcc()) {
    lcc::printDiag();
  } else {
    Serial.printf("WiThrottle: %s (server v%.1f)\n", wtClient.connected() ? "connected" : "disconnected",
                  serverProtocolVersion);
  }
  Serial.printf("Loco: %s (%s)\n", locoId.c_str(),
                locoAcquired ? "acquired" : (locoAcquirePending ? "acquiring" : "free"));
  Serial.printf("Notch: %d/%d Speed: %d Dir: %s\n", throttleNotch, maxNotch(), speed126,
                directionForward ? "FWD" : "REV");
}

uint32_t functionBits() {
  uint32_t bits = 0;
  for (uint8_t i = 0; i <= 28; ++i) {
    if (functionState[i]) {
      bits |= (1UL << i);
    }
  }
  return bits;
}

void printDiag() {
  const unsigned long now = millis();
  auto t = M5Dial.Touch.getDetail();
  Serial.printf("diag: up=%lu idle=%lu heap=%u act=%lu src=%s tier=%u panel=%s wifi=%s enc=%ld notch=%d+%d fn=%08lX draw=%lu(render %lu)us loop=%lu/%luus spd=%d dem=%d touch=%d brake=%d/%d\n",
                now, now - lastUserActivityMs, static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned long>(activityCount), lastActivitySource,
                backlightMode, displayPanelAsleep ? "asleep" : "awake",
                wifiSuspendedForPower ? "suspended" : (WiFi.status() == WL_CONNECTED ? "connected" : "down"),
                static_cast<long>(M5Dial.Encoder.read()), throttleNotch, encoderPendingDetents, functionBits(),
                static_cast<unsigned long>(lastDrawUsec), static_cast<unsigned long>(lastRenderUsec),
                static_cast<unsigned long>(lastLoopUsec),
                static_cast<unsigned long>(maxLoopUsec), speed126, throttleDemandSpeed,
                static_cast<int>(t.state), brakeHoldActive ? 1 : 0, brakeRecovering ? 1 : 0);
  settings::printState();
}

void handleSerialCommand(const String& in) {
  String cmd = in;
  cmd.trim();
  if (cmd.isEmpty()) {
    return;
  }

  // Diagnostics that must not disturb the idle timer.
  if (cmd.equalsIgnoreCase("diag")) {
    printDiag();
    return;
  }
  if (cmd.equalsIgnoreCase("shot")) {
    dumpScreenshot();
    return;
  }
  if (cmd.equalsIgnoreCase("settings")) {
    settings::printState();
    if (!settings::isOpen()) {
      settings::open();
    }
    uiDirty = true;
    return;
  }

  if (cmd.equalsIgnoreCase("defaults")) {
    settings::resetToDefaults();
    Serial.println("Settings reset to defaults");
    return;
  }

  if (cmd.startsWith("set ")) {
    String arg = cmd.substring(4);
    arg.trim();
    if (arg.startsWith("ip ")) {
      IPAddress parsed;
      if (!parsed.fromString(arg.substring(3))) {
        Serial.println("! Usage: set ip <a.b.c.d>");
        return;
      }
      for (int i = 0; i < 4; ++i) {
        settings::cfg.ip[i] = parsed[i];
      }
    } else if (arg.startsWith("port ")) {
      const long port = arg.substring(5).toInt();
      if (port < 1 || port > 65535) {
        Serial.println("! Usage: set port <1-65535>");
        return;
      }
      settings::cfg.port = static_cast<uint16_t>(port);
    } else if (arg.startsWith("proto ")) {
      String which = arg.substring(6);
      which.trim();
      which.toLowerCase();
      if (which == "wt" || which == "withrottle") {
        settings::cfg.protocol = PROTOCOL_WITHROTTLE;
      } else if (which == "lcc") {
        settings::cfg.protocol = PROTOCOL_LCC_GRIDCONNECT;
      } else {
        Serial.println("! Usage: set proto <wt|lcc>");
        return;
      }
    } else if (arg.startsWith("ssid ")) {
      const String value = arg.substring(5);
      strncpy(settings::cfg.ssid, value.c_str(), sizeof(settings::cfg.ssid) - 1);
      settings::cfg.ssid[sizeof(settings::cfg.ssid) - 1] = '\0';
    } else if (arg.startsWith("pass ")) {
      const String value = arg.substring(5);
      strncpy(settings::cfg.pass, value.c_str(), sizeof(settings::cfg.pass) - 1);
      settings::cfg.pass[sizeof(settings::cfg.pass) - 1] = '\0';
    } else {
      Serial.println("! set ip|port|proto|ssid|pass <value>");
      return;
    }
    settings::save();
    wtClient.stop();
    lcc::quit();
    locoAcquired = false;
    locoAcquirePending = false;
    wtNextRetryMs = 0;
    settings::printState();
    return;
  }

  if (cmd.startsWith("wtin ")) { // test only: feed a line in as if the server had sent it
    parseServerLine(cmd.substring(5));
    return;
  }
  if (cmd.startsWith("lccin ")) { // test only: feed in a GridConnect frame
    lcc::injectFrame(cmd.substring(6));
    return;
  }
  if (cmd.equalsIgnoreCase("lcc")) {
    lcc::printDiag();
    return;
  }
  if (cmd.equalsIgnoreCase("btn") || cmd.equalsIgnoreCase("btn long")) { // test only: fake a press
    noteUserActivity("button");
    if (cmd.equalsIgnoreCase("btn long")) {
      buttonLongAction();
    } else {
      buttonShortAction();
    }
    return;
  }
  if (cmd.startsWith("enc ")) { // test only: inject raw encoder counts as if the dial were turned
    const int counts = cmd.substring(4).toInt();
    M5Dial.Encoder.write(M5Dial.Encoder.read() + counts * encoderSign());
    return;
  }
  if (cmd.startsWith("simbat ")) { // test only: fake a battery reading for the display
    const int pct = cmd.substring(7).toInt();
    batteryPercent = (pct < 0) ? -1 : min(pct, 100);
    batteryMillivolts = -1;
    uiDirty = true;
    return;
  }
  if (cmd.startsWith("tier ")) {
    const int tier = cmd.substring(5).toInt();
    if (tier == 0) {
      noteUserActivity("serial");
    } else if (tier == 1) {
      lastUserActivityMs = millis() - (settings::cfg.dimAfterSec * 1000UL);
    } else {
      lastUserActivityMs = millis() - (settings::cfg.offAfterSec * 1000UL);
    }
    Serial.printf("# tier %d requested\n", tier);
    return;
  }

  noteUserActivity("serial");

  Serial.print("# ");
  Serial.println(cmd);

  if (cmd.equalsIgnoreCase("help") || cmd == "?") {
    printDebugHelp();
    return;
  }

  if (cmd.equalsIgnoreCase("status")) {
    printDebugStatus();
    return;
  }

  if (cmd.equalsIgnoreCase("bat")) {
    samplePowerSource(true);
    sampleBattery(true);
    printBatteryStatus();
    return;
  }

  if (cmd.equalsIgnoreCase("off")) {
    powerOffDevice();
    return;
  }

  if (cmd.equalsIgnoreCase("acq")) {
    locoAcquireAttempts = 0;
    acquireLoco();
    return;
  }

  if (cmd.equalsIgnoreCase("steal")) {
    sendStealRequest();
    return;
  }

  if (cmd.equalsIgnoreCase("rel")) {
    releaseLoco();
    return;
  }

  if (cmd.equalsIgnoreCase("estop")) {
    emergencyStop();
    return;
  }

  if (cmd.equalsIgnoreCase("clear") || cmd.equalsIgnoreCase("estop clear")) {
    clearEmergencyStopLatch();
    return;
  }

  if (cmd.equalsIgnoreCase("wt")) {
    if (SERIAL_OUTPUT_ONLY) {
      Serial.println("! SERIAL_OUTPUT_ONLY is active");
      return;
    }
    connectWiThrottle();
    return;
  }

  if (cmd.equalsIgnoreCase("wifi")) {
    if (SERIAL_OUTPUT_ONLY) {
      Serial.println("! SERIAL_OUTPUT_ONLY is active");
      return;
    }
    if (wifiSuspendedForPower) {
      resumeWiFiFromPower();
    }
    WiFi.disconnect();
    startWiFiConnect();
    return;
  }

  if (cmd.startsWith("send ")) {
    const String raw = cmd.substring(5);
    sendWt(raw);
    return;
  }

  if (cmd.startsWith("speed ")) {
    setSpeed(cmd.substring(6).toInt());
    return;
  }

  if (cmd.startsWith("notch ")) {
    const int n = cmd.substring(6).toInt();
    if (n < 0 || n > maxNotch()) {
      Serial.printf("! Notch must be 0..%d\n", maxNotch());
      return;
    }
    applyNotch(n, false);
    return;
  }

  if (cmd.startsWith("dir ")) {
    const String arg = cmd.substring(4);
    if (arg.equalsIgnoreCase("f")) {
      setDirection(true);
      return;
    }
    if (arg.equalsIgnoreCase("r")) {
      setDirection(false);
      return;
    }
    if (arg.equalsIgnoreCase("t")) {
      toggleDirection();
      return;
    }
  }

  if (cmd.startsWith("fn ")) {
    char buf[80] = {0};
    cmd.toCharArray(buf, sizeof(buf));
    int fn = -1;
    char state[16] = {0};
    if (sscanf(buf, "fn %d %15s", &fn, state) == 2) {
      if (fn < 0 || fn > 28) {
        Serial.println("! Function must be 0..28");
        return;
      }
      String stateArg = String(state);
      stateArg.toLowerCase();
      if (stateArg == "on") {
        setFunction(static_cast<uint8_t>(fn), true);
        return;
      }
      if (stateArg == "off") {
        setFunction(static_cast<uint8_t>(fn), false);
        return;
      }
      if (stateArg == "t") {
        toggleFunction(static_cast<uint8_t>(fn));
        return;
      }
    }
  }

  if (cmd.startsWith("loco ")) {
    char buf[80] = {0};
    cmd.toCharArray(buf, sizeof(buf));
    int addr = -1;
    char type[8] = {0};
    if (sscanf(buf, "loco %d %7s", &addr, type) == 2) {
      String typeArg = String(type);
      typeArg.toLowerCase();
      const bool isLong = (typeArg == "l");
      if (addr > 0 && addr <= 9999 && (typeArg == "s" || typeArg == "l")) {
        setActiveLoco(static_cast<uint16_t>(addr), isLong, true, "SERIAL");
        return;
      }
    }
    Serial.println("! Usage: loco <1-9999> <s|l>");
    return;
  }

  Serial.println("! Unknown command. Type 'help'.");
}

void readSerialCommands() {
  static String rx;
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      if (!rx.isEmpty()) {
        handleSerialCommand(rx);
        rx = "";
      }
    } else {
      rx += c;
      if (rx.length() > 120) {
        rx = "";
      }
    }
  }
}

} // namespace

void setup() {
  Serial.begin(115200);
  delay(100);

  // Release the backlight latch left by powerOffDevice(), or the PWM can never turn it back on.
  gpio_hold_dis(static_cast<gpio_num_t>(DISPLAY_BACKLIGHT_PIN));
  gpio_deep_sleep_hold_dis();
  M5Dial.begin(true, true);
  setCpuFrequencyMhz(CPU_FREQ_MHZ);
  M5Dial.Speaker.begin();
  M5Dial.Speaker.setVolume(240);
  M5Dial.Display.setRotation(0);
  M5Dial.Display.setBrightness(settings::cfg.brightnessActive);
  lastUserActivityMs = millis();
  backlightMode = 0;
  initUiCanvas();
  settings::begin(applySettings);
  settingsWereStored = settings::wasStored();
  activeLocoAddress = settings::cfg.locoAddress;
  activeLocoIsLong = settings::cfg.locoIsLong != 0;

  updateLocoLabel();

  Wire.begin();
  keypadPresent = keypad.begin(Wire, KEYPAD_I2C_ADDR);
  if (keypadPresent) {
    setStatus("Qwiic keypad connected");
  } else {
    setStatus("Qwiic keypad NOT found");
  }
  lastKeypadPollMs = millis();

  if (BATTERY_ADC_PIN >= 0) {
    pinMode(BATTERY_ADC_PIN, INPUT);
    analogSetPinAttenuation(static_cast<uint8_t>(BATTERY_ADC_PIN), ADC_11db);
  }
  if (POWER_SENSE_ADC_PIN >= 0) {
    pinMode(POWER_SENSE_ADC_PIN, INPUT);
    analogSetPinAttenuation(static_cast<uint8_t>(POWER_SENSE_ADC_PIN), ADC_11db);
  }
  if (CHARGE_STATUS_PIN >= 0) {
    pinMode(CHARGE_STATUS_PIN, INPUT_PULLUP);
  }
  samplePowerSource(true);
  sampleBattery(true);
  rfidAntennaOff(); // M5Dial.begin() leaves the RFID field on; keep it off between polls

  M5Dial.Encoder.write(0);
  syncEncoderToSpeed();
  drawUi(true);

  // Don't write credentials to flash on every connect attempt: flash writes mask GPIO interrupts and
  // cause the encoder to miss quadrature edges.
  WiFi.persistent(false);
  if (usingLcc()) {
    lcc::begin();
  }
  // WiFi and WiThrottle connect non-blocking from loop() via serviceWiFi(), so inputs work immediately.
  if (SERIAL_OUTPUT_ONLY) {
    setStatus("Serial debug only mode");
  } else {
    startWiFiConnect();
  }
  printDebugHelp();
  if (!settingsWereStored) {
    // Nothing saved yet: open the menu instead of retrying a placeholder address.
    settings::open();
    setStatus("Set WiFi and server");
  }
  drawUi(true);
  lastUserActivityMs = millis();
}

void loop() {
  const uint32_t loopStartUsec = micros();
  M5Dial.update();
  updatePowerState();
  serviceWiFi();
  if (usingLcc() && !wifiSuspendedForPower) {
    lcc::service();
  }

  readSerialCommands();
  handleEncoder();
  handleTouchBrake();
  handleButton();
  handleKeypad();
  handleRfid();
  handleMomentum();
  readServer();
  serviceLocoAcquire();
  expireInfoLine();
  samplePowerSource(false);
  sampleBattery(false);

  if (!SERIAL_OUTPUT_ONLY && wtClient.connected() && (millis() - lastHeartbeatMs) > 2000) {
    sendWt("*");
    lastHeartbeatMs = millis();
  }

  if (!SERIAL_OUTPUT_ONLY && (millis() - lastServerActivityMs) > 15000 && wtClient.connected()) {
    setInfo("Waiting for server data");
  }

  if (uiDirty && backlightMode != 2) {
    drawUi();
  }
  if (backlightMode == 0) {
    delay(LOOP_DELAY_ACTIVE_MS);
  } else if (backlightMode == 1) {
    delay(LOOP_DELAY_DIM_MS);
  } else {
    idleSleep();
  }
  lastLoopUsec = micros() - loopStartUsec;
  if (lastLoopUsec > maxLoopUsec) {
    maxLoopUsec = lastLoopUsec;
  }
}
