#pragma once

// WiFi settings
static const char* WIFI_SSID = "YOUR_WIFI_SSID";
static const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// Throttle protocol: WiThrottle (JMRI, Engine Driver compatible) or LCC/OpenLCB over GridConnect.
#define PROTOCOL_WITHROTTLE 0
#define PROTOCOL_LCC_GRIDCONNECT 1
// Override at build time with: make build DEFINES=-DTHROTTLE_PROTOCOL_DEFAULT=PROTOCOL_LCC_GRIDCONNECT
#ifndef THROTTLE_PROTOCOL_DEFAULT
#define THROTTLE_PROTOCOL_DEFAULT PROTOCOL_WITHROTTLE
#endif
// This is only the default for a device with nothing saved yet; the settings screen wins after that.
static const uint8_t THROTTLE_PROTOCOL = THROTTLE_PROTOCOL_DEFAULT;

// JMRI WiThrottle server settings
static const char* WITHROTTLE_HOST = "192.168.1.50";
static const uint16_t WITHROTTLE_PORT = 12090;
// TCP connect timeout for the WiThrottle server. Connect attempts block the UI for at most this long;
// retries back off from 5 s to 30 s while the server is unreachable.
static const uint32_t WITHROTTLE_CONNECT_TIMEOUT_MS = 1500;

// Locomotive settings
static const uint16_t LOCO_ADDRESS = 3;
static const bool LOCO_IS_LONG_ADDRESS = false;

// Reverse is capped to this fraction of the notches, so the dial only reaches half speed backwards.
// 1 gives reverse the same range as forward.
static const uint8_t REVERSE_NOTCH_DIVISOR = 2;

// Device identity shown in JMRI
static const char* THROTTLE_NAME = "DialThrottle";
static const char* THROTTLE_ID_PREFIX = "M5DIAL-";

// The second status line clears after this long, which brings back the "Btn Hold = Setup" hint.
static const uint32_t INFO_LINE_TIMEOUT_MS = 8000;

// Hold the dial button this long to open the on-device settings screen (protocol, server address
// and port). Settings are saved in flash and survive a reflash of the firmware.
static const uint32_t SETTINGS_LONG_PRESS_MS = 1200;

// Multi-throttle key. Any single character identifies this throttle to the server; Engine Driver
// uses '0' to '6'.
static const char WITHROTTLE_THROTTLE_KEY = '0';
// JMRI echoes every speed command back. A report matching one we sent within this window is treated
// as our own echo rather than another throttle moving the loco.
static const uint32_t WITHROTTLE_ECHO_WINDOW_MS = 4000;
// When the server says the loco is already in use, take it automatically. Set false to require the
// serial 'steal' command instead, which is safer if someone else may be running that loco.
static const bool WITHROTTLE_AUTO_STEAL = true;
// How long to wait for the server to confirm an acquire before retrying.
static const uint32_t ACQUIRE_CONFIRM_TIMEOUT_MS = 3000;
static const uint8_t ACQUIRE_MAX_ATTEMPTS = 3;

// LCC / OpenLCB over a GridConnect TCP link (JMRI's LCC GridConnect server, or a CAN gateway).
// Only used when THROTTLE_PROTOCOL is PROTOCOL_LCC_GRIDCONNECT.
static const char* LCC_GRIDCONNECT_HOST = WITHROTTLE_HOST;
static const uint16_t LCC_GRIDCONNECT_PORT = 12021;
static const uint32_t LCC_CONNECT_TIMEOUT_MS = 1500;
// This node's LCC node ID. It must be unique on the network. The default keeps the top three bytes
// (05.01.01 is the range openlcb.org set aside for non-commercial use) and makes the low three bytes
// unique to this board from its MAC address. Set LCC_NODE_ID_FROM_MAC false to use the value as-is.
static const uint64_t LCC_NODE_ID = 0x050101011234ULL;
static const bool LCC_NODE_ID_FROM_MAC = true;
// Speed scaling. The traction protocol carries speed in metres per second, so the throttle's top
// notch has to be mapped to a prototype speed. 126 mph puts one speed step at one mph, which matches
// the usual OpenMRN command station mapping. Adjust if your command station scales differently.
static const float LCC_FULL_SPEED_MPH = 126.0f;
// Alias reservation window: the standard requires at least 200 ms between the CID frames and RID.
static const uint32_t LCC_ALIAS_RESERVE_WAIT_MS = 250;
static const uint32_t LCC_TRAIN_QUERY_TIMEOUT_MS = 1500;
static const uint8_t LCC_TRAIN_QUERY_ATTEMPTS = 4;
// LCC accessories are event driven, so turnout numbers are mapped onto an event range by convention:
// turnout n produces base + 2n (closed) or base + 2n + 1 (thrown).
static const uint64_t LCC_TURNOUT_EVENT_BASE = 0x0501010114FF0000ULL;

// Optional SparkFun Qwiic Keypad address (default is 0x4B)
static const uint8_t KEYPAD_I2C_ADDR = 0x4B;

// Throttle notches: the dial steps between SPEED_NOTCH_COUNT notches (0 = stop, top notch = full speed).
// The settings menu offers 4, 8, 12 or 16; this is only the default for an unconfigured device.
static const uint8_t SPEED_NOTCH_COUNT = 8;
// Encoder detents (clicks) per notch. At 2, the notch moves only after two detents in the same
// direction, like a gated notch lever: one detent up then one detent down returns to the notch
// position and changes nothing. A half-step shows as a grey outline on the block being approached.
static const uint8_t ENCODER_CLICKS_PER_NOTCH = 2;
// A half-step is held indefinitely at 0 (a slow, deliberate turn still reaches the next notch).
// Set a value in ms to discard a half-step the operator left hanging.
static const uint32_t ENCODER_PARTIAL_NOTCH_TIMEOUT_MS = 0;
// Detent debounce: a second detent arriving sooner than this is treated as bounce/miscount and
// ignored. Deliberate clicks are further apart; a fast flick is ~40-50 ms.
static const uint32_t ENCODER_DETENT_MIN_INTERVAL_MS = 40;
// A reversal sooner than this after an accepted detent is treated as a miscount (a missed quadrature
// edge makes the encoder library guess a two-count jump, sometimes the wrong way), not as the
// operator turning back. Real turn-backs after a short pause register on the first click.
static const uint32_t ENCODER_REVERSAL_PAUSE_MS = 150;
// After the dial has been still this long, re-reference to the current count so contact chatter
// and count drift cannot add up to a phantom click.
static const uint32_t ENCODER_REST_REALIGN_MS = 400;
// Momentum: the notch sets a target speed and the actual speed ramps toward it like a real locomotive.
// Rates are in speed steps (0..126) per second: 8/s takes ~16 s from stop to full, 5/s ~25 s full to stop.
// The touch brake still bleeds speed off quickly; releasing it hands control back to the ramp.
static const bool MOMENTUM_ENABLED = true;
static const float MOMENTUM_ACCEL_STEPS_PER_SEC = 8.0f;
static const float MOMENTUM_DECEL_STEPS_PER_SEC = 5.0f;
static const uint32_t MOMENTUM_TICK_MS = 100;

// Encoder: raw counts per detent click and rotation sense.
static const int ENCODER_RAW_COUNTS_PER_STEP = 2;
// Set to -1 if turning clockwise decreases speed on your unit.
static const int ENCODER_DIRECTION_SIGN = 1;

// Touch brake behavior (press and hold to brake, release to recover).
static const uint32_t BRAKE_TICK_MS = 40;
static const int BRAKE_DECEL_PER_TICK = 3;
static const int BRAKE_ACCEL_PER_TICK = 4;
static const uint32_t BRAKE_TOUCH_DEBOUNCE_MS = 120;

// Debug mode: print outgoing protocol lines to Serial only, do not send over WiFi.
// Override with make build DEFINES=-DSERIAL_OUTPUT_ONLY_DEFAULT=1 to test protocol output with no layout attached.
#ifndef SERIAL_OUTPUT_ONLY_DEFAULT
#define SERIAL_OUTPUT_ONLY_DEFAULT 0
#endif
static const bool SERIAL_OUTPUT_ONLY = SERIAL_OUTPUT_ONLY_DEFAULT;

// Power saving (useful for rechargeable battery operation).
static const uint8_t DISPLAY_BRIGHTNESS_ACTIVE = 72;
static const uint8_t DISPLAY_BRIGHTNESS_DIM = 8;
// M5Dial LCD backlight PWM pin (driven by M5GFX; held low across deep sleep).
static const uint8_t DISPLAY_BACKLIGHT_PIN = 9;
static const uint32_t DISPLAY_DIM_AFTER_MS = 8000;
static const uint32_t DISPLAY_OFF_AFTER_MS = 20000;
static const bool ENABLE_WIFI_MODEM_SLEEP = true;
static const bool ENABLE_WIFI_LOW_TX_POWER = true;
static const bool ENABLE_WIFI_POWER_GATING_WHEN_DISPLAY_OFF = true;
// 240 MHz keeps the UI redraw short so the encoder is polled often and the dial feels immediate.
// 80 MHz saves a little current but makes a full redraw ~3x slower; the idle tiers below do the real
// power saving.
static const uint16_t CPU_FREQ_MHZ = 240;
// Minimum spacing between full redraws. Bursts of state changes (a fast spin, a momentum ramp)
// coalesce into one frame instead of queueing several.
static const uint32_t UI_MIN_REDRAW_INTERVAL_MS = 30;
static const uint16_t LOOP_DELAY_ACTIVE_MS = 8;
static const uint16_t LOOP_DELAY_DIM_MS = 25;
static const uint16_t LOOP_DELAY_OFF_MS = 80;

// Deeper power saving.
// While the backlight is dimmed/off, WiFi uses maximum modem sleep (higher RX latency, lower power).
static const bool ENABLE_WIFI_MAX_MODEM_SLEEP_WHEN_IDLE = true;
// Put the LCD panel itself into sleep (SLPIN) while the backlight is off.
static const bool ENABLE_DISPLAY_PANEL_SLEEP = true;
// Once the display is off AND WiFi is suspended, use ESP32 light sleep between polls.
// Wake sources: dial rotation, dial button, touch screen, and a periodic timer (keypad/RFID polls).
// Note: USB serial drops while light-sleeping. With LIGHT_SLEEP_SKIP_WHEN_USB_SERIAL true, sleep is skipped
// while a USB host (computer) is attached; it still happens on battery or a plain USB charger.
static const bool ENABLE_LIGHT_SLEEP_WHEN_DISPLAY_OFF = true;
static const bool LIGHT_SLEEP_SKIP_WHEN_USB_SERIAL = true;
static const uint32_t LIGHT_SLEEP_INTERVAL_MS = 250;
// Full power off after this much idle time with the loco stopped (0 disables).
// On battery the hold circuit cuts power; press the dial button to turn it back on.
// On USB power the unit deep-sleeps instead and wakes on a screen touch.
static const uint32_t POWER_OFF_AFTER_MS = 15UL * 60UL * 1000UL;
// WiFi is never suspended, and the unit never light-sleeps or powers off, while the loco is moving.

// RFID reader duty cycle: keep the 13.56 MHz field off between short polls.
static const bool ENABLE_RFID_ANTENNA_DUTY_CYCLE = true;
static const uint32_t RFID_POLL_ACTIVE_MS = 150;
static const uint32_t RFID_POLL_IDLE_MS = 1000;
static const uint32_t RFID_ANTENNA_SETTLE_MS = 4;
// A tag left resting on the reader is reported once; it must be away this long before it re-triggers.
static const uint32_t RFID_REPEAT_IGNORE_MS = 2500;

// Qwiic keypad I2C poll schedule (the keypad buffers presses in its own FIFO between polls).
static const uint32_t KEYPAD_POLL_ACTIVE_MS = 40;
static const uint32_t KEYPAD_POLL_IDLE_MS = 250;

// Battery gauge.
// The M5Dial has a battery connector and charger but NO battery voltage sense line, so a divider is
// needed: BAT+ -> R1 -> ADC pin -> R2 -> GND (e.g. 100k/100k, ratio 2.0). Use an ADC1 pin from Port B:
// GPIO1 or GPIO2. Set to -1 to disable the gauge (indicator is hidden).
static const int8_t BATTERY_ADC_PIN = -1;
static const float BATTERY_DIVIDER_RATIO = 2.0f;
static const uint32_t BATTERY_SAMPLE_INTERVAL_MS = 5000;
static const uint8_t BATTERY_LOW_WARN_PERCENT = 15;
// Show millivolts next to the percentage (useful when calibrating BATTERY_DIVIDER_RATIO).
static const bool BATTERY_SHOW_VOLTAGE = false;

// Power source / charging indicator.
// Per the M5Dial schematic the TP4057 charger's CHRG/STDBY pins are not wired to the ESP32, and the Grove
// 5V pin is boosted from the battery, so a stock unit can only detect a USB *host* (computer) attached.
// That detection needs no hardware and is always on. Two optional wire taps give the full picture:
//  - POWER_SENSE_ADC_PIN: divider (e.g. 100k/100k) from the 5V input rail (StampS3 header pin "M5V" / net
//    +5VIN, present only on USB or DC-terminal power) to GPIO1 or GPIO2. Shows "PWR" on external power.
//  - CHARGE_STATUS_PIN: wire from the TP4057 CHRG pin (pin 1, open drain, low while charging) to a free GPIO
//    (GPIO39/43/44 on the StampS3 header, or GPIO1/2). Uses the internal pull-up. Shows "CHG" while charging.
static const int8_t POWER_SENSE_ADC_PIN = -1;
static const float POWER_SENSE_DIVIDER_RATIO = 2.0f;
static const uint16_t POWER_SENSE_MIN_MV = 4300;
static const int8_t CHARGE_STATUS_PIN = -1;
static const bool CHARGE_STATUS_ACTIVE_LOW = true;
// Skip the idle power-off while external power is detected (USB host, or POWER_SENSE/CHARGE pins).
static const bool POWER_OFF_ONLY_ON_BATTERY = true;

// RFID tag to locomotive mapping.
// UID must be uppercase hex without separators, e.g. "04A1B2C3D4".
struct RfidLocoMap {
	const char* uidHex;
	uint16_t address;
	bool isLong;
};

// Optional MySQL lookup for RFID -> loco assignment.
// Lookup order is: MySQL -> RFID_LOCO_MAP -> last 4 UID hex digits.
// Leave disabled until credentials and schema are set.
static const bool ENABLE_RFID_MYSQL_LOOKUP = false;
// Use the server address set on the device rather than RFID_MYSQL_HOST.
static const bool RFID_MYSQL_FOLLOW_SERVER_IP = true;
static const char* RFID_MYSQL_HOST = WITHROTTLE_HOST;
static const uint16_t RFID_MYSQL_PORT = 3306;
static const char* RFID_MYSQL_USER = "dialthrottle";
static const char* RFID_MYSQL_PASS = "change_me";
static const char* RFID_MYSQL_DB = "wifithrottle";
static const char* RFID_MYSQL_TABLE = "rfid_loco_map";
static const char* RFID_MYSQL_UID_COLUMN = "rfid_uid";
static const char* RFID_MYSQL_LOCO_COLUMN = "loco_id";
// Optional long/short column. Set to nullptr to derive long from address (>127).
static const char* RFID_MYSQL_IS_LONG_COLUMN = nullptr;

// Add your known tag mappings here.
static const RfidLocoMap RFID_LOCO_MAP[] = {
		{"DEADBEEF", 3, false},
};
