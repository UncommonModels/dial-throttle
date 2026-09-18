#include "settings.h"

#include <Preferences.h>
#include <string.h>

#include "config.h"

namespace settings {

Config cfg = {};

namespace {

constexpr uint8_t CONFIG_VERSION = 1;
constexpr size_t VISIBLE_ROWS = 5;

Preferences prefs;
ApplyFn applyCallback = nullptr;
bool opened = false;
bool storedLoaded = false;

enum class ItemType : uint8_t { Submenu, Bool, Int, Enum, Ip, Text, Info, Action, Back };
enum class Action : uint8_t { None, SaveExit, Exit, ResetDefaults };

struct Item {
  const char* label;
  ItemType type;
  void* value;       // uint8_t, uint16_t or char[] depending on type and width
  uint8_t width;     // bytes of the value, or buffer size for Text
  int32_t min;
  int32_t max;
  int32_t step;
  const char* const* choices;
  uint8_t choiceCount;
  uint8_t target;    // page index for Submenu
  Action action;
  bool masked;       // Text: show as dots
};

const char* const PROTOCOL_NAMES[] = {"WiThrottle", "LCC"};
const char* const OFF_ON[] = {"Off", "On"};
const char* const SHORT_LONG[] = {"Short", "Long"};

#define ITEM_SUB(label, page) {label, ItemType::Submenu, nullptr, 0, 0, 0, 0, nullptr, 0, page, Action::None, false}
#define ITEM_BOOL(label, field) \
  {label, ItemType::Bool, &cfg.field, sizeof(cfg.field), 0, 1, 1, OFF_ON, 2, 0, Action::None, false}
#define ITEM_INT(label, field, lo, hi, st) \
  {label, ItemType::Int, &cfg.field, sizeof(cfg.field), lo, hi, st, nullptr, 0, 0, Action::None, false}
#define ITEM_ENUM(label, field, names, count) \
  {label, ItemType::Enum, &cfg.field, sizeof(cfg.field), 0, (count)-1, 1, names, count, 0, Action::None, false}
#define ITEM_IP(label) {label, ItemType::Ip, cfg.ip, 4, 0, 255, 1, nullptr, 0, 0, Action::None, false}
#define ITEM_TEXT(label, field, mask) \
  {label, ItemType::Text, cfg.field, sizeof(cfg.field), 0, 0, 0, nullptr, 0, 0, Action::None, mask}
#define ITEM_ACTION(label, act) {label, ItemType::Action, nullptr, 0, 0, 0, 0, nullptr, 0, 0, act, false}
#define ITEM_BACK() {"Back", ItemType::Back, nullptr, 0, 0, 0, 0, nullptr, 0, 0, Action::None, false}
#define ITEM_INFO(label) {label, ItemType::Info, nullptr, 0, 0, 0, 0, nullptr, 0, 0, Action::None, false}

enum PageId : uint8_t { PAGE_ROOT = 0, PAGE_NETWORK, PAGE_THROTTLE, PAGE_DISPLAY, PAGE_SYSTEM, PAGE_COUNT };

const Item ROOT_ITEMS[] = {
    ITEM_SUB("Network", PAGE_NETWORK),
    ITEM_SUB("Throttle", PAGE_THROTTLE),
    ITEM_SUB("Display & power", PAGE_DISPLAY),
    ITEM_SUB("System", PAGE_SYSTEM),
    ITEM_ACTION("Save & exit", Action::SaveExit),
    ITEM_ACTION("Discard & exit", Action::Exit),
};

const Item NETWORK_ITEMS[] = {
    ITEM_TEXT("WiFi name", ssid, false),
    ITEM_TEXT("WiFi pass", pass, true),
    ITEM_ENUM("Protocol", protocol, PROTOCOL_NAMES, 2),
    ITEM_IP("Server"),
    ITEM_INT("Port", port, 1, 65535, 1),
    ITEM_BACK(),
};

const Item THROTTLE_ITEMS[] = {
    ITEM_INT("Loco addr", locoAddress, 1, 9999, 1),
    ITEM_ENUM("Addr type", locoIsLong, SHORT_LONG, 2),
    ITEM_INT("Notches", notchCount, 4, 16, 4), // 4, 8, 12 or 16
    ITEM_INT("Clicks/notch", clicksPerNotch, 1, 4, 1),
    ITEM_BOOL("Reverse dial", encoderReversed),
    ITEM_BOOL("Momentum", momentum),
    ITEM_INT("Accel /s", accelPerSec, 1, 60, 1),
    ITEM_INT("Decel /s", decelPerSec, 1, 60, 1),
    ITEM_BACK(),
};

const Item DISPLAY_ITEMS[] = {
    ITEM_INT("Brightness", brightnessActive, 10, 255, 5),
    ITEM_INT("Dim level", brightnessDim, 0, 120, 2),
    ITEM_INT("Dim after s", dimAfterSec, 2, 600, 2),
    ITEM_INT("Off after s", offAfterSec, 5, 900, 5),
    ITEM_INT("Power off min", powerOffAfterMin, 0, 240, 1),
    ITEM_BOOL("Sound", sound),
    ITEM_BACK(),
};

const Item SYSTEM_ITEMS[] = {
    ITEM_BOOL("RFID reader", rfidEnabled),
    ITEM_ACTION("Reset defaults", Action::ResetDefaults),
    ITEM_BACK(),
};

struct Page {
  const char* title;
  const Item* items;
  uint8_t count;
};

const Page PAGES[PAGE_COUNT] = {
    {"SETTINGS", ROOT_ITEMS, sizeof(ROOT_ITEMS) / sizeof(Item)},
    {"NETWORK", NETWORK_ITEMS, sizeof(NETWORK_ITEMS) / sizeof(Item)},
    {"THROTTLE", THROTTLE_ITEMS, sizeof(THROTTLE_ITEMS) / sizeof(Item)},
    {"DISPLAY", DISPLAY_ITEMS, sizeof(DISPLAY_ITEMS) / sizeof(Item)},
    {"SYSTEM", SYSTEM_ITEMS, sizeof(SYSTEM_ITEMS) / sizeof(Item)},
};

uint8_t page = PAGE_ROOT;
uint8_t sel = 0;
uint8_t scrollTop = 0;
uint8_t pageStack[4] = {};
uint8_t selStack[4] = {};
uint8_t stackDepth = 0;

// Editing state.
enum class Edit : uint8_t { None, Value, Ip, Text };
Edit edit = Edit::None;
int8_t ipOctet = 0;
bool digitFresh = true;

// Text editor state.
const char TEXT_CHARS[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_.:/@#!$%&*+=?,()[]";
constexpr uint8_t TEXT_EXTRA = 2; // 0 = done, 1 = backspace
int16_t textPick = TEXT_EXTRA;
char textBuf[65] = {};
uint8_t textLimit = 0;
char* textTarget = nullptr;

const Item& currentItem() {
  return PAGES[page].items[sel];
}

int32_t readValue(const Item& item) {
  if (item.width == 1) {
    return *static_cast<uint8_t*>(item.value);
  }
  if (item.width == 2) {
    return *static_cast<uint16_t*>(item.value);
  }
  return 0;
}

void writeValue(const Item& item, int32_t v) {
  if (v < item.min) {
    v = item.max;
  } else if (v > item.max) {
    v = item.min;
  }
  if (item.width == 1) {
    *static_cast<uint8_t*>(item.value) = static_cast<uint8_t>(v);
  } else if (item.width == 2) {
    *static_cast<uint16_t*>(item.value) = static_cast<uint16_t>(v);
  }
}

void loadDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  strncpy(cfg.ssid, WIFI_SSID, sizeof(cfg.ssid) - 1);
  strncpy(cfg.pass, WIFI_PASS, sizeof(cfg.pass) - 1);
  cfg.protocol = THROTTLE_PROTOCOL;
  IPAddress parsed;
  if (parsed.fromString(WITHROTTLE_HOST)) {
    for (int i = 0; i < 4; ++i) {
      cfg.ip[i] = parsed[i];
    }
  }
  cfg.port = (cfg.protocol == PROTOCOL_LCC_GRIDCONNECT) ? LCC_GRIDCONNECT_PORT : WITHROTTLE_PORT;
  cfg.locoAddress = LOCO_ADDRESS;
  cfg.locoIsLong = LOCO_IS_LONG_ADDRESS ? 1 : 0;
  cfg.notchCount = SPEED_NOTCH_COUNT;
  cfg.clicksPerNotch = ENCODER_CLICKS_PER_NOTCH;
  cfg.encoderReversed = (ENCODER_DIRECTION_SIGN < 0) ? 1 : 0;
  cfg.momentum = MOMENTUM_ENABLED ? 1 : 0;
  cfg.accelPerSec = static_cast<uint8_t>(MOMENTUM_ACCEL_STEPS_PER_SEC);
  cfg.decelPerSec = static_cast<uint8_t>(MOMENTUM_DECEL_STEPS_PER_SEC);
  cfg.brightnessActive = DISPLAY_BRIGHTNESS_ACTIVE;
  cfg.brightnessDim = DISPLAY_BRIGHTNESS_DIM;
  cfg.dimAfterSec = DISPLAY_DIM_AFTER_MS / 1000;
  cfg.offAfterSec = DISPLAY_OFF_AFTER_MS / 1000;
  cfg.powerOffAfterMin = POWER_OFF_AFTER_MS / 60000UL;
  cfg.sound = 1;
  cfg.lccFullSpeedMph = static_cast<uint8_t>(LCC_FULL_SPEED_MPH);
  cfg.rfidEnabled = 1;
}

void openTextEditor(const Item& item) {
  textTarget = static_cast<char*>(item.value);
  textLimit = item.width;
  memset(textBuf, 0, sizeof(textBuf));
  strncpy(textBuf, textTarget, sizeof(textBuf) - 1);
  textPick = TEXT_EXTRA;
  edit = Edit::Text;
}

void commitText() {
  if (textTarget != nullptr) {
    memset(textTarget, 0, textLimit);
    strncpy(textTarget, textBuf, textLimit - 1);
  }
  textTarget = nullptr;
  edit = Edit::None;
}

void doAction(Action action) {
  switch (action) {
    case Action::SaveExit:
      close(true);
      break;
    case Action::Exit:
      close(false);
      break;
    case Action::ResetDefaults:
      loadDefaults();
      break;
    default:
      break;
  }
}

void enterPage(uint8_t target) {
  if (stackDepth < sizeof(pageStack)) {
    pageStack[stackDepth] = page;
    selStack[stackDepth] = sel;
    ++stackDepth;
  }
  page = target;
  sel = 0;
  scrollTop = 0;
}

void leavePage() {
  if (stackDepth == 0) {
    close(false);
    return;
  }
  --stackDepth;
  page = pageStack[stackDepth];
  sel = selStack[stackDepth];
  scrollTop = (sel >= VISIBLE_ROWS) ? static_cast<uint8_t>(sel - VISIBLE_ROWS + 1) : 0;
}

void followSelection() {
  if (sel < scrollTop) {
    scrollTop = sel;
  } else if (sel >= scrollTop + VISIBLE_ROWS) {
    scrollTop = static_cast<uint8_t>(sel - VISIBLE_ROWS + 1);
  }
}

} // namespace

// --- public API -------------------------------------------------------------

void begin(ApplyFn onApply) {
  applyCallback = onApply;
  loadDefaults();
  prefs.begin("dialthrottle", false);
  Config stored = {};
  const uint8_t version = prefs.getUChar("version", 0);
  const size_t size = prefs.getBytesLength("cfg");
  if (version == CONFIG_VERSION && size == sizeof(Config)) {
    prefs.getBytes("cfg", &stored, sizeof(stored));
    cfg = stored;
    storedLoaded = true;
  } else if (version != 0) {
    Serial.println("Settings: stored layout is from another firmware version, using defaults");
  }
  // Only 4, 8, 12 and 16 notches are offered, so snap anything else onto that grid.
  if (cfg.notchCount < 4 || cfg.notchCount > 16 || (cfg.notchCount % 4) != 0) {
    cfg.notchCount = static_cast<uint8_t>(constrain(((cfg.notchCount + 2) / 4) * 4, 4, 16));
  }
  if (cfg.clicksPerNotch < 1 || cfg.clicksPerNotch > 4) {
    cfg.clicksPerNotch = ENCODER_CLICKS_PER_NOTCH;
  }
  if (cfg.accelPerSec == 0) {
    cfg.accelPerSec = 1;
  }
  if (cfg.decelPerSec == 0) {
    cfg.decelPerSec = 1;
  }
  if (cfg.lccFullSpeedMph == 0) {
    cfg.lccFullSpeedMph = static_cast<uint8_t>(LCC_FULL_SPEED_MPH);
  }
  if (applyCallback != nullptr) {
    applyCallback();
  }
}

void save() {
  prefs.putUChar("version", CONFIG_VERSION);
  prefs.putBytes("cfg", &cfg, sizeof(cfg));
  if (applyCallback != nullptr) {
    applyCallback();
  }
}

void resetToDefaults() {
  loadDefaults();
  save();
}

bool isOpen() {
  return opened;
}

bool wasStored() {
  return storedLoaded;
}

void open() {
  opened = true;
  page = PAGE_ROOT;
  sel = 0;
  scrollTop = 0;
  stackDepth = 0;
  edit = Edit::None;
}

void close(bool saveChanges) {
  edit = Edit::None;
  opened = false;
  if (saveChanges) {
    save();
  } else {
    begin(applyCallback); // reload what is in flash and re-apply it
  }
}

void encoder(int8_t direction) {
  if (!opened || direction == 0) {
    return;
  }
  switch (edit) {
    case Edit::Text: {
      const int16_t total = static_cast<int16_t>(strlen(TEXT_CHARS)) + TEXT_EXTRA;
      textPick = static_cast<int16_t>((textPick + direction + total) % total);
      return;
    }
    case Edit::Ip: {
      int v = cfg.ip[ipOctet] + direction;
      if (v < 0) {
        v = 255;
      } else if (v > 255) {
        v = 0;
      }
      cfg.ip[ipOctet] = static_cast<uint8_t>(v);
      digitFresh = true;
      return;
    }
    case Edit::Value: {
      const Item& item = currentItem();
      writeValue(item, readValue(item) + direction * item.step);
      digitFresh = true;
      return;
    }
    case Edit::None:
    default: {
      const int count = PAGES[page].count;
      int next = static_cast<int>(sel) + direction;
      if (next < 0) {
        next = count - 1;
      } else if (next >= count) {
        next = 0;
      }
      sel = static_cast<uint8_t>(next);
      followSelection();
      return;
    }
  }
}

void press() {
  if (!opened) {
    return;
  }
  if (edit == Edit::Text) {
    if (textPick == 0) { // done
      commitText();
      return;
    }
    if (textPick == 1) { // backspace
      const size_t n = strlen(textBuf);
      if (n > 0) {
        textBuf[n - 1] = '\0';
      }
      return;
    }
    const size_t n = strlen(textBuf);
    if (n + 1 < sizeof(textBuf) && n + 1 < textLimit) {
      textBuf[n] = TEXT_CHARS[textPick - TEXT_EXTRA];
      textBuf[n + 1] = '\0';
    }
    return;
  }
  if (edit == Edit::Ip) {
    ++ipOctet;
    if (ipOctet > 3) {
      ipOctet = 0;
      edit = Edit::None;
    }
    digitFresh = true;
    return;
  }
  if (edit == Edit::Value) {
    edit = Edit::None;
    return;
  }

  const Item& item = currentItem();
  switch (item.type) {
    case ItemType::Submenu:
      enterPage(item.target);
      return;
    case ItemType::Back:
      leavePage();
      return;
    case ItemType::Action:
      doAction(item.action);
      return;
    case ItemType::Bool:
    case ItemType::Enum:
      writeValue(item, readValue(item) + 1);
      return;
    case ItemType::Int:
      edit = Edit::Value;
      digitFresh = true;
      return;
    case ItemType::Ip:
      edit = Edit::Ip;
      ipOctet = 0;
      digitFresh = true;
      return;
    case ItemType::Text:
      openTextEditor(item);
      return;
    case ItemType::Info:
    default:
      return;
  }
}

void longPress() {
  if (!opened) {
    return;
  }
  if (edit == Edit::Text) {
    commitText();
    return;
  }
  if (edit != Edit::None) {
    edit = Edit::None;
    return;
  }
  leavePage();
}

void digit(char key) {
  if (!opened || key < '0' || key > '9') {
    return;
  }
  const int n = key - '0';
  if (edit == Edit::Text) {
    const size_t len = strlen(textBuf);
    if (len + 1 < sizeof(textBuf) && len + 1 < textLimit) {
      textBuf[len] = key;
      textBuf[len + 1] = '\0';
    }
    return;
  }
  if (edit == Edit::Ip) {
    int v = digitFresh ? n : (cfg.ip[ipOctet] * 10 + n);
    if (v > 255) {
      v = n;
    }
    cfg.ip[ipOctet] = static_cast<uint8_t>(v);
    digitFresh = false;
    return;
  }
  if (edit == Edit::Value) {
    const Item& item = currentItem();
    if (item.step != 1) {
      return; // stepped values (like the notch count) only move in their own increments
    }
    int32_t v = digitFresh ? n : (readValue(item) * 10 + n);
    if (v > item.max) {
      v = n;
    }
    writeValue(item, v);
    digitFresh = false;
  }
}

String ipString() {
  return String(cfg.ip[0]) + "." + String(cfg.ip[1]) + "." + String(cfg.ip[2]) + "." + String(cfg.ip[3]);
}

const char* protocolName() {
  return (cfg.protocol == PROTOCOL_LCC_GRIDCONNECT) ? PROTOCOL_NAMES[1] : PROTOCOL_NAMES[0];
}

// --- rendering --------------------------------------------------------------

namespace {

String valueText(const Item& item) {
  switch (item.type) {
    case ItemType::Bool:
    case ItemType::Enum: {
      const int32_t v = readValue(item);
      if (item.choices != nullptr && v < item.choiceCount) {
        return String(item.choices[v]);
      }
      return String(v);
    }
    case ItemType::Int:
      return String(readValue(item));
    case ItemType::Ip:
      return ipString();
    case ItemType::Text: {
      const char* text = static_cast<const char*>(item.value);
      if (text[0] == '\0') {
        return "-";
      }
      if (item.masked) {
        String dots;
        for (size_t i = 0; i < strlen(text) && i < 8; ++i) {
          dots += '*';
        }
        return dots;
      }
      String out(text);
      if (out.length() > 11) {
        out = out.substring(0, 10) + "~";
      }
      return out;
    }
    case ItemType::Submenu:
      return ">";
    default:
      return "";
  }
}

void renderTextEditor(LovyanGFX& g) {
  const int16_t w = g.width();
  const int16_t cx = w / 2;
  g.setTextSize(1);
  g.setTextColor(0x9CD3, BLACK);
  const char* title = "EDIT TEXT";
  g.setCursor(cx - g.textWidth(title) / 2, 40);
  g.print(title);

  // Current string, wrapped over two short lines so a long password stays readable.
  String shown(textBuf);
  g.setTextSize(1);
  g.setTextColor(WHITE, BLACK);
  const size_t perLine = 20;
  for (size_t line = 0; line < 2; ++line) {
    const size_t start = line * perLine;
    if (start >= shown.length()) {
      break;
    }
    const String part = shown.substring(start, min(start + perLine, shown.length()));
    g.setCursor(cx - g.textWidth(part) / 2, 62 + static_cast<int16_t>(line) * 12);
    g.print(part);
  }
  g.drawFastHLine(40, 90, w - 80, 0x4208);

  // Character strip centred on the current pick.
  const int16_t total = static_cast<int16_t>(strlen(TEXT_CHARS)) + TEXT_EXTRA;
  for (int8_t offset = -3; offset <= 3; ++offset) {
    const int16_t index = static_cast<int16_t>((textPick + offset + total) % total);
    String label;
    if (index == 0) {
      label = "OK";
    } else if (index == 1) {
      label = "<X";
    } else {
      label = String(TEXT_CHARS[index - TEXT_EXTRA]);
    }
    const int16_t x = cx + offset * 28;
    const bool active = (offset == 0);
    if (active) {
      g.fillRoundRect(x - 13, 104, 26, 26, 4, 0x6B4D);
      g.drawRoundRect(x - 13, 104, 26, 26, 4, YELLOW);
    }
    g.setTextSize(active ? 2 : 1);
    g.setTextColor(active ? YELLOW : 0x7BEF, active ? 0x6B4D : BLACK);
    g.setCursor(x - g.textWidth(label) / 2, active ? 110 : 113);
    g.print(label);
  }

  g.setTextSize(1);
  g.setTextColor(0x7BEF, BLACK);
  const char* hint = "press=add  hold=done";
  g.setCursor(cx - g.textWidth(hint) / 2, 150);
  g.print(hint);
}

} // namespace

void render(LovyanGFX& g) {
  g.fillScreen(BLACK);
  const int16_t w = g.width();
  const int16_t cx = w / 2;

  if (edit == Edit::Text) {
    renderTextEditor(g);
    return;
  }

  const Page& p = PAGES[page];
  g.setTextSize(2);
  g.setTextColor(WHITE, BLACK);
  g.setCursor(cx - g.textWidth(p.title) / 2, 20);
  g.print(p.title);

  const int16_t rowH = 24;
  const int16_t firstY = 52;
  for (uint8_t row = 0; row < VISIBLE_ROWS; ++row) {
    const uint8_t index = static_cast<uint8_t>(scrollTop + row);
    if (index >= p.count) {
      break;
    }
    const Item& item = p.items[index];
    const bool selected = (index == sel);
    const int16_t y = firstY + row * rowH;
    const uint16_t bg = selected ? 0x2965 : BLACK;
    if (selected) {
      g.fillRoundRect(22, y - 3, w - 44, rowH - 3, 4, bg);
      g.drawRoundRect(22, y - 3, w - 44, rowH - 3, 4, WHITE);
    }

    g.setTextSize(1);
    const bool centred = (item.type == ItemType::Action || item.type == ItemType::Back);
    g.setTextColor(selected ? WHITE : 0x9CD3, bg);
    if (centred) {
      g.setCursor(cx - g.textWidth(item.label) / 2, y + 4);
      g.print(item.label);
      continue;
    }
    g.setCursor(32, y + 4);
    g.print(item.label);

    if (item.type == ItemType::Ip && edit == Edit::Ip && selected) {
      // Draw the address octet by octet so the one being edited stands out.
      String parts[4];
      int16_t total = 0;
      for (int k = 0; k < 4; ++k) {
        parts[k] = String(cfg.ip[k]);
        total += g.textWidth(parts[k]) + (k < 3 ? g.textWidth(".") : 0);
      }
      int16_t x = w - 32 - total;
      for (int k = 0; k < 4; ++k) {
        const bool active = (ipOctet == k);
        if (active) {
          g.fillRect(x - 1, y + 2, g.textWidth(parts[k]) + 2, 11, 0x6B4D);
        }
        g.setTextColor(active ? YELLOW : 0x07FF, active ? 0x6B4D : bg);
        g.setCursor(x, y + 4);
        g.print(parts[k]);
        x += g.textWidth(parts[k]);
        if (k < 3) {
          g.setTextColor(0x07FF, bg);
          g.setCursor(x, y + 4);
          g.print(".");
          x += g.textWidth(".");
        }
      }
      continue;
    }

    String value = valueText(item);
    if (item.type == ItemType::Info) {
      value = "";
    }
    if (value.length() == 0) {
      continue;
    }
    const bool editing = selected && (edit == Edit::Value);
    g.setTextColor(editing ? YELLOW : 0x07FF, bg);
    g.setCursor(w - 32 - g.textWidth(value), y + 4);
    g.print(value);
  }

  // Scroll position, drawn as a short bar on the right.
  if (p.count > VISIBLE_ROWS) {
    const int16_t trackY = firstY - 2;
    const int16_t trackH = VISIBLE_ROWS * rowH - 4;
    const int16_t barH = max<int16_t>(8, trackH * VISIBLE_ROWS / p.count);
    const int16_t barY = trackY + (trackH - barH) * scrollTop / max<int>(1, p.count - VISIBLE_ROWS);
    g.fillRoundRect(w - 18, trackY, 3, trackH, 1, 0x2104);
    g.fillRoundRect(w - 18, barY, 3, barH, 1, 0x7BEF);
  }

  g.setTextSize(1);
  g.setTextColor(0x7BEF, BLACK);
  const char* hint = (edit == Edit::None) ? "hold=back" : "press=next  hold=done";
  g.setCursor(cx - g.textWidth(hint) / 2, 196);
  g.print(hint);
}

void printState() {
  Serial.printf("settings: open=%d page=%s sel=%u edit=%d\n", opened ? 1 : 0, PAGES[page].title, sel,
                static_cast<int>(edit));
  Serial.printf("settings: wifi=\"%s\" pass=%s proto=%s server=%s:%u\n", cfg.ssid,
                cfg.pass[0] ? "(set)" : "(empty)", protocolName(), ipString().c_str(), cfg.port);
  Serial.printf("settings: loco=%u%s notches=%u clicks=%u revdial=%u momentum=%u accel=%u decel=%u\n",
                cfg.locoAddress, cfg.locoIsLong ? "L" : "S", cfg.notchCount, cfg.clicksPerNotch,
                cfg.encoderReversed, cfg.momentum, cfg.accelPerSec, cfg.decelPerSec);
  Serial.printf("settings: bright=%u dim=%u dim_s=%u off_s=%u poweroff_min=%u sound=%u rfid=%u mph=%u\n",
                cfg.brightnessActive, cfg.brightnessDim, cfg.dimAfterSec, cfg.offAfterSec,
                cfg.powerOffAfterMin, cfg.sound, cfg.rfidEnabled, cfg.lccFullSpeedMph);
}

} // namespace settings
