#pragma once

#include <Arduino.h>
#include <M5Dial.h>

// On-device settings: a scrolling, multi-page menu driven by the dial, the button and (when fitted)
// the Qwiic keypad. Values live in flash, so a throttle can be pointed at a different layout, or
// retuned, without rebuilding the firmware.
namespace settings {

struct Config {
  // Network
  char ssid[33];
  char pass[65];
  uint8_t protocol; // PROTOCOL_WITHROTTLE or PROTOCOL_LCC_GRIDCONNECT
  uint8_t ip[4];
  uint16_t port;

  // Throttle
  uint16_t locoAddress;
  uint8_t locoIsLong;
  uint8_t notchCount;
  uint8_t clicksPerNotch;
  uint8_t encoderReversed;
  uint8_t momentum;
  uint8_t accelPerSec;
  uint8_t decelPerSec;

  // Display and power
  uint8_t brightnessActive;
  uint8_t brightnessDim;
  uint16_t dimAfterSec;
  uint16_t offAfterSec;
  uint16_t powerOffAfterMin; // 0 = never
  uint8_t sound;

  // LCC
  uint8_t lccFullSpeedMph;

  // RFID
  uint8_t rfidEnabled;
};

extern Config cfg;

// Called after the user saves, so the firmware can apply the new values.
using ApplyFn = void (*)();
void begin(ApplyFn onApply);
void save();
void resetToDefaults();

bool isOpen();
bool wasStored(); // true when values came from flash rather than compiled defaults
void open();
void close(bool saveChanges);

// Input, routed from the throttle's own handlers.
void encoder(int8_t direction);
void press();
void longPress();
void digit(char key);

void render(LovyanGFX& g);
void printState();

String ipString();
const char* protocolName();

} // namespace settings
