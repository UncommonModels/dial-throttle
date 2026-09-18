#include "lcc.h"

#include <WiFi.h>

#include "config.h"
#include "settings.h"

namespace lcc {
namespace {

// --- CAN header layout (S-9.7.2.1) -----------------------------------------
// Bit 28 is always 1. Bit 27 selects OpenLCB message (1) or CAN control frame (0).
// Bits 26..12 are the variable field, bits 11..0 the source alias.
constexpr uint32_t HEADER_BASE = 0x10000000UL;
constexpr uint32_t FRAME_OPENLCB = 0x08000000UL; // bit 27
constexpr uint32_t FRAME_TYPE_MTI = 0x01000000UL; // global/addressed MTI frame

// CAN control frame variable fields.
constexpr uint16_t CID1 = 0x7000;
constexpr uint16_t CID2 = 0x6000;
constexpr uint16_t CID3 = 0x5000;
constexpr uint16_t CID4 = 0x4000;
constexpr uint16_t VF_RID = 0x0700; // reserve id
constexpr uint16_t VF_AMD = 0x0701; // alias map definition
constexpr uint16_t VF_AME = 0x0702; // alias map enquiry
constexpr uint16_t VF_AMR = 0x0703; // alias map reset

// Message type indicators.
constexpr uint16_t MTI_INIT_COMPLETE = 0x0100;
constexpr uint16_t MTI_VERIFIED_NODE_ID = 0x0170;
constexpr uint16_t MTI_VERIFIED_NODE_ID_SIMPLE = 0x0171;
constexpr uint16_t MTI_VERIFY_NODE_ID_ADDRESSED = 0x0488;
constexpr uint16_t MTI_VERIFY_NODE_ID_GLOBAL = 0x0490;
constexpr uint16_t MTI_PROTOCOL_SUPPORT_INQUIRY = 0x0828;
constexpr uint16_t MTI_PROTOCOL_SUPPORT_REPLY = 0x0668;
constexpr uint16_t MTI_EVENT_REPORT = 0x05B4;
constexpr uint16_t MTI_TRACTION_COMMAND = 0x05EB;
constexpr uint16_t MTI_TRACTION_REPLY = 0x01E9;

// Traction control commands.
constexpr uint8_t TRACTION_SET_SPEED = 0x00;
constexpr uint8_t TRACTION_SET_FN = 0x01;
constexpr uint8_t TRACTION_ESTOP = 0x02;
constexpr uint8_t TRACTION_CONTROLLER = 0x20;
constexpr uint8_t TRACTION_CTRL_ASSIGN = 0x01;
constexpr uint8_t TRACTION_CTRL_RELEASE = 0x02;

// Well-known node IDs for DCC locomotives (S-9.7.3.2).
constexpr uint64_t TRAIN_NODE_ID_DCC = 0x060100000000ULL;
constexpr uint64_t TRAIN_LONG_ADDRESS_FLAG = 0xC000ULL;

enum class State : uint8_t {
  Offline,
  Connecting,
  AliasClaiming, // CID frames sent, waiting out the reservation window
  Ready,         // alias reserved, node initialized
};

WiFiClient client;
String rxBuf;
String serverHost = LCC_GRIDCONNECT_HOST;
uint16_t serverPort = LCC_GRIDCONNECT_PORT;
State state = State::Offline;
const char* stateText = "offline";

uint64_t nodeId = 0;
uint16_t alias = 0;
uint32_t lfsr1 = 0;
uint32_t lfsr2 = 0;
unsigned long aliasClaimStartMs = 0;
unsigned long nextConnectMs = 0;
uint8_t aliasAttempts = 0;

uint64_t trainNodeId = 0;
uint16_t trainAlias = 0;
bool controllerAssigned = false;
unsigned long trainQueryMs = 0;
uint8_t trainQueryAttempts = 0;

int lastSpeedStep = 0;
bool lastForward = true;
bool turnoutThrown[1000] = {};
uint32_t framesSent = 0;
uint32_t framesReceived = 0;
String lastSentFrame;

// --- helpers ---------------------------------------------------------------

uint32_t controlHeader(uint16_t variableField, uint16_t src) {
  return HEADER_BASE | (static_cast<uint32_t>(variableField & 0x7FFF) << 12) | (src & 0x0FFF);
}

uint32_t messageHeader(uint16_t mti, uint16_t src) {
  return HEADER_BASE | FRAME_OPENLCB | FRAME_TYPE_MTI | (static_cast<uint32_t>(mti & 0x0FFF) << 12) |
         (src & 0x0FFF);
}

void nodeIdBytes(uint64_t nid, uint8_t* out) {
  for (int i = 0; i < 6; ++i) {
    out[i] = static_cast<uint8_t>((nid >> (40 - 8 * i)) & 0xFF);
  }
}

uint64_t bytesToNodeId(const uint8_t* in) {
  uint64_t nid = 0;
  for (int i = 0; i < 6; ++i) {
    nid = (nid << 8) | in[i];
  }
  return nid;
}

void sendFrame(uint32_t header, const uint8_t* data, uint8_t len) {
  char buf[40];
  int n = snprintf(buf, sizeof(buf), ":X%08lXN", static_cast<unsigned long>(header));
  for (uint8_t i = 0; i < len && n < static_cast<int>(sizeof(buf)) - 3; ++i) {
    n += snprintf(buf + n, sizeof(buf) - n, "%02X", data[i]);
  }
  buf[n++] = ';';
  buf[n] = '\0';
  lastSentFrame = buf;
  ++framesSent;
  Serial.print("LCC> ");
  Serial.println(buf);
  if (SERIAL_OUTPUT_ONLY || !client.connected()) {
    return;
  }
  client.print(buf);
  client.print('\n');
}

// Addressed messages carry the destination alias in the first two data bytes, with a flag nibble
// that marks multi-frame payloads (0 = only, 1 = first, 3 = middle, 2 = last).
void sendAddressed(uint16_t mti, uint16_t dest, const uint8_t* payload, uint8_t len) {
  if (dest == 0) {
    return;
  }
  if (len <= 6) {
    uint8_t d[8];
    d[0] = static_cast<uint8_t>((dest >> 8) & 0x0F);
    d[1] = static_cast<uint8_t>(dest & 0xFF);
    memcpy(d + 2, payload, len);
    sendFrame(messageHeader(mti, alias), d, static_cast<uint8_t>(len + 2));
    return;
  }
  uint8_t sent = 0;
  while (sent < len) {
    const uint8_t chunk = static_cast<uint8_t>(min<int>(6, len - sent));
    uint8_t flags;
    if (sent == 0) {
      flags = 0x1; // first frame
    } else if (sent + chunk >= len) {
      flags = 0x2; // last frame
    } else {
      flags = 0x3; // middle frame
    }
    uint8_t d[8];
    d[0] = static_cast<uint8_t>((flags << 4) | ((dest >> 8) & 0x0F));
    d[1] = static_cast<uint8_t>(dest & 0xFF);
    memcpy(d + 2, payload + sent, chunk);
    sendFrame(messageHeader(mti, alias), d, static_cast<uint8_t>(chunk + 2));
    sent = static_cast<uint8_t>(sent + chunk);
  }
}

void sendGlobal(uint16_t mti, const uint8_t* payload, uint8_t len) {
  sendFrame(messageHeader(mti, alias), payload, len);
}

// IEEE 754 half precision, used by the traction protocol for speed in metres per second.
uint16_t floatToHalf(float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  const uint16_t sign = static_cast<uint16_t>((bits >> 16) & 0x8000);
  int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFF) - 127 + 15;
  uint32_t mantissa = bits & 0x7FFFFF;

  if (((bits >> 23) & 0xFF) == 0xFF) { // inf / nan
    return static_cast<uint16_t>(sign | 0x7C00 | (mantissa ? 0x0200 : 0));
  }
  if (exponent >= 0x1F) { // overflow to infinity
    return static_cast<uint16_t>(sign | 0x7C00);
  }
  if (exponent <= 0) { // subnormal or zero
    if (exponent < -10) {
      return sign;
    }
    mantissa |= 0x800000;
    const uint32_t shift = static_cast<uint32_t>(14 - exponent);
    uint16_t sub = static_cast<uint16_t>(mantissa >> shift);
    if ((mantissa >> (shift - 1)) & 1) {
      ++sub; // round to nearest
    }
    return static_cast<uint16_t>(sign | sub);
  }
  uint16_t half = static_cast<uint16_t>(sign | (exponent << 10) | (mantissa >> 13));
  if (mantissa & 0x1000) {
    ++half; // round to nearest
  }
  return half;
}

uint64_t trainNodeIdFor(uint16_t address, bool isLong) {
  return TRAIN_NODE_ID_DCC | (isLong ? (TRAIN_LONG_ADDRESS_FLAG | address) : static_cast<uint64_t>(address));
}

// Alias generator from the CAN standard: a 48-bit LFSR seeded with the node ID.
void seedAlias() {
  lfsr1 = static_cast<uint32_t>((nodeId >> 24) & 0xFFFFFF);
  lfsr2 = static_cast<uint32_t>(nodeId & 0xFFFFFF);
}

uint16_t nextAlias() {
  const uint32_t temp1 = ((lfsr1 << 9) | ((lfsr2 >> 15) & 0x1FF)) & 0xFFFFFF;
  const uint32_t temp2 = (lfsr2 << 9) & 0xFFFFFF;
  lfsr2 = (lfsr2 + temp2 + 0x7A4BA9) & 0xFFFFFF;
  lfsr1 = (lfsr1 + temp1 + 0x1B0CA3 + ((lfsr2 >> 24) & 1)) & 0xFFFFFF;
  uint16_t a = static_cast<uint16_t>((lfsr1 ^ lfsr2 ^ (lfsr1 >> 12) ^ (lfsr2 >> 12)) & 0x0FFF);
  if (a == 0) {
    a = 1;
  }
  return a;
}

void sendAmd() {
  uint8_t nid[6];
  nodeIdBytes(nodeId, nid);
  sendFrame(controlHeader(VF_AMD, alias), nid, 6);
}

void sendVerifiedNodeId() {
  uint8_t nid[6];
  nodeIdBytes(nodeId, nid);
  sendGlobal(MTI_VERIFIED_NODE_ID, nid, 6);
}

void queryTrainNode() {
  if (trainNodeId == 0 || state != State::Ready) {
    return;
  }
  uint8_t nid[6];
  nodeIdBytes(trainNodeId, nid);
  sendGlobal(MTI_VERIFY_NODE_ID_GLOBAL, nid, 6);
  trainQueryMs = millis();
  ++trainQueryAttempts;
}

void assignController() {
  if (trainAlias == 0) {
    return;
  }
  uint8_t payload[9];
  payload[0] = TRACTION_CONTROLLER;
  payload[1] = TRACTION_CTRL_ASSIGN;
  payload[2] = 0x00; // flags
  nodeIdBytes(nodeId, payload + 3);
  sendAddressed(MTI_TRACTION_COMMAND, trainAlias, payload, sizeof(payload));
}

void startAliasClaim() {
  alias = nextAlias();
  uint8_t none[1] = {0};
  (void)none;
  sendFrame(controlHeader(static_cast<uint16_t>(CID1 | ((nodeId >> 36) & 0xFFF)), alias), nullptr, 0);
  sendFrame(controlHeader(static_cast<uint16_t>(CID2 | ((nodeId >> 24) & 0xFFF)), alias), nullptr, 0);
  sendFrame(controlHeader(static_cast<uint16_t>(CID3 | ((nodeId >> 12) & 0xFFF)), alias), nullptr, 0);
  sendFrame(controlHeader(static_cast<uint16_t>(CID4 | (nodeId & 0xFFF)), alias), nullptr, 0);
  aliasClaimStartMs = millis();
  ++aliasAttempts;
  state = State::AliasClaiming;
  stateText = "claiming alias";
}

void finishAliasClaim() {
  sendFrame(controlHeader(VF_RID, alias), nullptr, 0);
  sendAmd();
  uint8_t nid[6];
  nodeIdBytes(nodeId, nid);
  sendGlobal(MTI_INIT_COMPLETE, nid, 6);
  state = State::Ready;
  stateText = "ready";
  aliasAttempts = 0;
  if (trainNodeId != 0) {
    trainQueryAttempts = 0;
    queryTrainNode();
  }
}

void handleAliasConflict() {
  if (state == State::Ready) {
    sendFrame(controlHeader(VF_AMR, alias), nullptr, 0);
  }
  Serial.println("LCC: alias conflict, reclaiming");
  controllerAssigned = false;
  trainAlias = 0;
  startAliasClaim();
}

void handleFrame(uint32_t header, const uint8_t* data, uint8_t len) {
  ++framesReceived;
  const uint16_t src = static_cast<uint16_t>(header & 0x0FFF);

  if (alias != 0 && src == alias) {
    handleAliasConflict();
    return;
  }
  if (state != State::Ready) {
    return; // still claiming: only conflicts matter
  }

  if ((header & FRAME_OPENLCB) == 0) { // CAN control frame
    const uint16_t vf = static_cast<uint16_t>((header >> 12) & 0x7FFF);
    if (vf == VF_AME) {
      if (len == 0 || (len == 6 && bytesToNodeId(data) == nodeId)) {
        sendAmd();
      }
    }
    return;
  }

  const uint16_t mti = static_cast<uint16_t>((header >> 12) & 0x0FFF);
  switch (mti) {
    case MTI_VERIFY_NODE_ID_GLOBAL:
      if (len == 0 || (len == 6 && bytesToNodeId(data) == nodeId)) {
        sendVerifiedNodeId();
      }
      return;

    case MTI_VERIFY_NODE_ID_ADDRESSED:
      if (len >= 2 && (((static_cast<uint16_t>(data[0] & 0x0F) << 8) | data[1]) == alias)) {
        sendVerifiedNodeId();
      }
      return;

    case MTI_VERIFIED_NODE_ID:
    case MTI_VERIFIED_NODE_ID_SIMPLE:
      if (len == 6 && trainNodeId != 0 && bytesToNodeId(data) == trainNodeId && trainAlias != src) {
        trainAlias = src;
        Serial.printf("LCC: train node found, alias 0x%03X\n", trainAlias);
        assignController();
      }
      return;

    case MTI_PROTOCOL_SUPPORT_INQUIRY: {
      if (len >= 2 && (((static_cast<uint16_t>(data[0] & 0x0F) << 8) | data[1]) == alias)) {
        // Simple protocol subset: simple node information is not implemented.
        uint8_t payload[6] = {0x10, 0x00, 0x00, 0x00, 0x00, 0x00};
        sendAddressed(MTI_PROTOCOL_SUPPORT_REPLY, src, payload, sizeof(payload));
      }
      return;
    }

    case MTI_TRACTION_REPLY: {
      if (len < 4 || src != trainAlias) {
        return;
      }
      if (data[2] == TRACTION_CONTROLLER && data[3] == TRACTION_CTRL_ASSIGN) {
        const bool ok = (len < 5) || (data[4] == 0x00);
        controllerAssigned = ok;
        Serial.printf("LCC: controller assign %s\n", ok ? "accepted" : "refused");
        // The throttle pushes speed, direction and functions once it sees trainAssigned().
      }
      return;
    }

    default:
      return;
  }
}

// Parse one GridConnect frame, e.g. ":X19170123N0102030405;".
bool parseFrame(const String& frame, uint32_t& header, uint8_t* data, uint8_t& len) {
  int i = frame.indexOf(':');
  if (i < 0 || i + 2 >= static_cast<int>(frame.length())) {
    return false;
  }
  ++i;
  const char kind = frame[i++];
  if (kind != 'X' && kind != 'S') {
    return false;
  }
  uint32_t h = 0;
  int digits = 0;
  while (i < static_cast<int>(frame.length()) && isHexadecimalDigit(frame[i])) {
    h = (h << 4) | static_cast<uint32_t>(strtoul(String(frame[i]).c_str(), nullptr, 16));
    ++i;
    ++digits;
  }
  if (digits == 0 || i >= static_cast<int>(frame.length())) {
    return false;
  }
  const char type = frame[i++];
  if (type == 'R') {
    len = 0; // remote frame, no payload
    header = h;
    return true;
  }
  if (type != 'N') {
    return false;
  }
  len = 0;
  while (i + 1 < static_cast<int>(frame.length()) && isHexadecimalDigit(frame[i]) &&
         isHexadecimalDigit(frame[i + 1]) && len < 8) {
    char pair[3] = {frame[i], frame[i + 1], 0};
    data[len++] = static_cast<uint8_t>(strtoul(pair, nullptr, 16));
    i += 2;
  }
  header = h;
  return true;
}

void resetSession() {
  state = State::Offline;
  stateText = "offline";
  alias = 0;
  trainAlias = 0;
  controllerAssigned = false;
  aliasAttempts = 0;
  rxBuf = "";
}

} // namespace

// --- public API -------------------------------------------------------------

void setServer(const String& host, uint16_t port) {
  if (host == serverHost && port == serverPort) {
    return;
  }
  serverHost = host;
  serverPort = port;
  if (client.connected()) {
    client.stop();
    resetSession();
  }
}

void begin() {
  nodeId = LCC_NODE_ID;
  if (LCC_NODE_ID_FROM_MAC) {
    uint8_t mac[6] = {0};
    WiFi.macAddress(mac);
    // Keep the configured manufacturer prefix, make the low 24 bits unique to this device.
    nodeId = (LCC_NODE_ID & 0xFFFFFF000000ULL) |
             (static_cast<uint64_t>(mac[3]) << 16) | (static_cast<uint64_t>(mac[4]) << 8) | mac[5];
  }
  seedAlias();
  resetSession();
  Serial.printf("LCC node ID %02X.%02X.%02X.%02X.%02X.%02X\n",
                static_cast<unsigned>((nodeId >> 40) & 0xFF), static_cast<unsigned>((nodeId >> 32) & 0xFF),
                static_cast<unsigned>((nodeId >> 24) & 0xFF), static_cast<unsigned>((nodeId >> 16) & 0xFF),
                static_cast<unsigned>((nodeId >> 8) & 0xFF), static_cast<unsigned>(nodeId & 0xFF));
}

void service() {
  const unsigned long now = millis();

  // Serial-only mode runs the same state machine and prints the frames, it just has no socket.
  if (SERIAL_OUTPUT_ONLY) {
    if (state == State::Offline) {
      startAliasClaim();
    }
  } else if (!client.connected()) {
    if (state != State::Offline) {
      resetSession();
    }
    if (WiFi.status() != WL_CONNECTED || static_cast<long>(now - nextConnectMs) < 0) {
      return;
    }
    nextConnectMs = now + 5000;
    stateText = "connecting";
    if (!client.connect(serverHost.c_str(), serverPort, LCC_CONNECT_TIMEOUT_MS)) {
      stateText = "connect failed";
      return;
    }
    Serial.println("LCC: GridConnect link up");
    startAliasClaim();
    return;
  }

  while (!SERIAL_OUTPUT_ONLY && client.available()) {
    const char c = static_cast<char>(client.read());
    if (c == ';') {
      rxBuf += c;
      uint32_t header = 0;
      uint8_t data[8] = {0};
      uint8_t len = 0;
      if (parseFrame(rxBuf, header, data, len)) {
        handleFrame(header, data, len);
      }
      rxBuf = "";
      continue;
    }
    if (c == '\n' || c == '\r') {
      rxBuf = "";
      continue;
    }
    rxBuf += c;
    if (rxBuf.length() > 64) {
      rxBuf = "";
    }
  }

  if (state == State::AliasClaiming && (now - aliasClaimStartMs) >= LCC_ALIAS_RESERVE_WAIT_MS) {
    finishAliasClaim();
    return;
  }

  // Retry finding the train node until it answers.
  if (state == State::Ready && trainNodeId != 0 && trainAlias == 0 &&
      (now - trainQueryMs) >= LCC_TRAIN_QUERY_TIMEOUT_MS && trainQueryAttempts < LCC_TRAIN_QUERY_ATTEMPTS) {
    queryTrainNode();
  }
}

bool connected() {
  return state == State::Ready;
}

bool trainAssigned() {
  return state == State::Ready && trainAlias != 0 && controllerAssigned;
}

void selectTrain(uint16_t address, bool isLong) {
  releaseTrain();
  trainNodeId = trainNodeIdFor(address, isLong);
  trainAlias = 0;
  controllerAssigned = false;
  trainQueryAttempts = 0;
  Serial.printf("LCC: train node %02X.%02X.%02X.%02X.%02X.%02X\n",
                static_cast<unsigned>((trainNodeId >> 40) & 0xFF), static_cast<unsigned>((trainNodeId >> 32) & 0xFF),
                static_cast<unsigned>((trainNodeId >> 24) & 0xFF), static_cast<unsigned>((trainNodeId >> 16) & 0xFF),
                static_cast<unsigned>((trainNodeId >> 8) & 0xFF), static_cast<unsigned>(trainNodeId & 0xFF));
  queryTrainNode();
}

void releaseTrain() {
  if (trainAlias != 0 && controllerAssigned) {
    uint8_t payload[9];
    payload[0] = TRACTION_CONTROLLER;
    payload[1] = TRACTION_CTRL_RELEASE;
    payload[2] = 0x00;
    nodeIdBytes(nodeId, payload + 3);
    sendAddressed(MTI_TRACTION_COMMAND, trainAlias, payload, sizeof(payload));
  }
  trainAlias = 0;
  trainNodeId = 0;
  controllerAssigned = false;
}

void setSpeed(int step0to126, bool forward) {
  lastSpeedStep = constrain(step0to126, 0, 126);
  lastForward = forward;
  if (!trainAssigned()) {
    return;
  }
  const float mph = (static_cast<float>(lastSpeedStep) / 126.0f) * static_cast<float>(settings::cfg.lccFullSpeedMph);
  float mps = mph * 0.44704f;
  if (!forward) {
    mps = -mps;
  }
  uint16_t half = floatToHalf(mps);
  if (!forward) {
    half |= 0x8000; // reverse is carried by the sign bit, including at a standstill
  }
  uint8_t payload[3] = {TRACTION_SET_SPEED, static_cast<uint8_t>(half >> 8), static_cast<uint8_t>(half & 0xFF)};
  sendAddressed(MTI_TRACTION_COMMAND, trainAlias, payload, sizeof(payload));
}

void setFunction(uint8_t fn, bool on) {
  if (!trainAssigned()) {
    return;
  }
  uint8_t payload[6] = {TRACTION_SET_FN, 0x00, 0x00, fn, 0x00, static_cast<uint8_t>(on ? 0x01 : 0x00)};
  sendAddressed(MTI_TRACTION_COMMAND, trainAlias, payload, sizeof(payload));
}

void eStop() {
  if (!trainAssigned()) {
    return;
  }
  uint8_t payload[1] = {TRACTION_ESTOP};
  sendAddressed(MTI_TRACTION_COMMAND, trainAlias, payload, sizeof(payload));
}

void quit() {
  releaseTrain();
  if (state == State::Ready) {
    sendFrame(controlHeader(VF_AMR, alias), nullptr, 0);
  }
  client.stop();
  resetSession();
}

// LCC has no address-based turnout command: accessories are driven by events. This produces the
// conventional "base + 2*id" event pair, which matches how most layouts number turnout events.
void turnout(const String& id) {
  if (state != State::Ready) {
    return;
  }
  const long n = id.toInt();
  if (n < 0 || n > 999) {
    return;
  }
  turnoutThrown[n] = !turnoutThrown[n];
  const uint64_t event = LCC_TURNOUT_EVENT_BASE + static_cast<uint64_t>(n) * 2 + (turnoutThrown[n] ? 1 : 0);
  uint8_t payload[8];
  for (int i = 0; i < 8; ++i) {
    payload[i] = static_cast<uint8_t>((event >> (56 - 8 * i)) & 0xFF);
  }
  sendGlobal(MTI_EVENT_REPORT, payload, sizeof(payload));
}

void injectFrame(const String& frame) {
  uint32_t header = 0;
  uint8_t data[8] = {0};
  uint8_t len = 0;
  if (!parseFrame(frame, header, data, len)) {
    Serial.println("! not a GridConnect frame");
    return;
  }
  Serial.printf("LCC< %s (header %08lX, %u bytes)\n", frame.c_str(), static_cast<unsigned long>(header), len);
  handleFrame(header, data, len);
}

void printDiag() {
  Serial.printf("LCC: %s, alias 0x%03X, node %02X.%02X.%02X.%02X.%02X.%02X\n", stateText, alias,
                static_cast<unsigned>((nodeId >> 40) & 0xFF), static_cast<unsigned>((nodeId >> 32) & 0xFF),
                static_cast<unsigned>((nodeId >> 24) & 0xFF), static_cast<unsigned>((nodeId >> 16) & 0xFF),
                static_cast<unsigned>((nodeId >> 8) & 0xFF), static_cast<unsigned>(nodeId & 0xFF));
  Serial.printf("LCC: server %s:%u\n", serverHost.c_str(), serverPort);
  Serial.printf("LCC: train alias 0x%03X, controller %s, frames tx %lu rx %lu\n", trainAlias,
                controllerAssigned ? "assigned" : "not assigned", static_cast<unsigned long>(framesSent),
                static_cast<unsigned long>(framesReceived));
}

} // namespace lcc
