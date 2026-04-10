#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <NimBLEDevice.h>
#include <Update.h>
#include <WebServer.h>
#include <array>
#include <cstdarg>
#include <esp_system.h>
#include <esp32-hal-bt.h>
#include <map>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <time.h>
#include <vector>
#include <WiFiClientSecure.h>

#include "app_defaults.h"
#include "label_engine.h"
#include "settings_store.h"
#include "web_pages.h"

namespace {

constexpr uint16_t VICTRON_COMPANY_ID = 0x02E1;
constexpr uint8_t VICTRON_RECORD_SOLAR_CHARGER = 0x01;
constexpr uint32_t WIFI_RETRY_MS = 15000;
constexpr uint32_t WIFI_JOIN_TIMEOUT_MS = 12000;
constexpr uint32_t NTP_RETRY_MS = 30000;
constexpr uint32_t BLE_STALE_MS = 5UL * 60UL * 1000UL;
constexpr char LOS_ANGELES_TZ[] = "PST8PDT,M3.2.0/2,M11.1.0/2";
constexpr uint16_t HTTP_CONNECT_TIMEOUT_MS = 15000;
constexpr uint16_t HTTP_TIMEOUT_MS = 60000;
constexpr bool BLE_RUNTIME_ENABLED = true;
constexpr bool PRINTER_CLASSIC_TRANSPORT_ENABLED = false;
constexpr size_t PRINTER_BLE_CHUNK_SIZE = 180;
constexpr size_t PRINTER_ZLIB_BLOCK_SIZE = 1024;
constexpr uint32_t PRINTER_FLOW_WAIT_MS = 8000;
constexpr uint32_t PRINTER_BATTERY_STALE_MS = 30UL * 60UL * 1000UL;
constexpr size_t REMOTE_LOG_CAPACITY = 240;

struct LiveTelemetry {
  bool valid = false;
  bool hasTime = false;
  uint8_t deviceState = 0;
  uint8_t errorCode = 0;
  float batteryVoltage = NAN;
  float batteryCurrent = NAN;
  float yieldTodayKwh = NAN;
  float pvPowerWatts = NAN;
  float loadCurrent = NAN;
  int rssi = 0;
  uint32_t packetCounter = 0;
  uint32_t lastPacketMillis = 0;
  time_t lastPacketEpoch = 0;
};

struct DiscoveredDevice {
  String name;
  String address;
  int rssi = 0;
  std::vector<int> channels;
};

struct PrintSessionState {
  bool active = false;
  uint32_t startedAtMs = 0;
  size_t expectedRasterLength = 0;
  int widthBytes = 0;
  int height = 0;
  size_t receivedRaw = 0;
  std::vector<uint8_t> raster;
};

struct RemoteLogEntry {
  uint32_t id = 0;
  String line;
};

struct SerialPrinterSession {
  NimBLEClient *client = nullptr;
  NimBLERemoteService *service = nullptr;
  NimBLERemoteCharacteristic *writeChar = nullptr;
  NimBLERemoteCharacteristic *flowChar = nullptr;
  bool ff03Subscribed = false;
  String connectedMode;
};

String characteristicPropertySummary(const NimBLERemoteCharacteristic *characteristic) {
  if (!characteristic) {
    return String();
  }
  String summary;
  if (characteristic->canRead()) summary += "read ";
  if (characteristic->canWrite()) summary += "write ";
  if (characteristic->canWriteNoResponse()) summary += "write-no-rsp ";
  if (characteristic->canNotify()) summary += "notify ";
  if (characteristic->canIndicate()) summary += "indicate ";
  summary.trim();
  return summary;
}

bool initBleController();
String hexBytes(const uint8_t *data, size_t length);
void printerFlowNotifyCB(NimBLERemoteCharacteristic *characteristic, uint8_t *data, size_t length, bool isNotify);
void resetPrinterFlowControl();
bool sendBufferedLegacyBareHoldExact(NimBLERemoteCharacteristic *writeChar,
                                     const std::vector<uint8_t> &raster,
                                     uint16_t widthBytes,
                                     uint16_t height,
                                     String &errorMessage);

bool connectPrinterClient(NimBLEClient *client, const String &mac, int &lastError, String &modeUsed) {
  if (!client || !mac.length()) {
    lastError = -1;
    return false;
  }

  client->setConnectTimeout(5000);
  client->setConnectionParams(12, 24, 0, 60);

  NimBLEAddress publicAddr(std::string(mac.c_str()), BLE_ADDR_PUBLIC);
  if (client->connect(publicAddr, true, false, true)) {
    modeUsed = "public";
    lastError = 0;
    return true;
  }
  lastError = client->getLastError();

  NimBLEAddress randomAddr(std::string(mac.c_str()), BLE_ADDR_RANDOM);
  if (client->connect(randomAddr, true, false, true)) {
    modeUsed = "random";
    lastError = 0;
    return true;
  }
  lastError = client->getLastError();
  return false;
}

void clearSerialPrinterSession();
void disconnectSerialPrinterSession();
void settleAndDisconnectSerialPrinterSession(uint32_t settleMs = 45000);
void evictSerialPrinterPeerRecords();
bool connectSerialPrinterSession(String &message);

WebServer server(80);
DNSServer dnsServer;
NimBLEScan *victronScan = nullptr;
AppSettings settings;
LiveTelemetry telemetry;
std::vector<DiscoveredDevice> lastVictronScan;
std::vector<DiscoveredDevice> lastPrinterScan;
std::array<uint8_t, 16> bindKey{};
bool bindKeyLoaded = false;
uint32_t lastWiFiAttemptMs = 0;
uint32_t lastNtpAttemptMs = 0;
uint32_t lastVictronSweepMs = 0;
uint32_t stationConnectedSinceMs = 0;
bool mdnsStarted = false;
bool hadStationConnection = false;
bool setupApActive = false;
bool firmwareUpdateOk = false;
bool restartPending = false;
uint32_t restartAtMs = 0;
String firmwareUpdateMessage;

bool isLabelSizeToken(const String &value) {
  const int xIndex = value.indexOf('x');
  if (xIndex <= 0 || xIndex >= (value.length() - 1)) {
    return false;
  }
  for (int i = 0; i < value.length(); ++i) {
    if (i == xIndex) continue;
    if (value[i] < '0' || value[i] > '9') {
      return false;
    }
  }
  return true;
}

std::vector<String> configuredLabelSizeKeys() {
  std::vector<String> keys = labelPresetKeys();
  String custom = settings.customLabelSizes;
  custom.replace(';', ',');
  custom.replace('\n', ',');
  custom.replace('\r', ',');
  int start = 0;
  while (start <= custom.length()) {
    int comma = custom.indexOf(',', start);
    if (comma < 0) comma = custom.length();
    String token = custom.substring(start, comma);
    token.trim();
    token.toLowerCase();
    if (isLabelSizeToken(token)) {
      bool exists = false;
      for (const auto &key : keys) {
        if (key.equalsIgnoreCase(token)) {
          exists = true;
          break;
        }
      }
      if (!exists) {
        keys.push_back(token);
      }
    }
    start = comma + 1;
  }
  return keys;
}
size_t firmwareUploadBytes = 0;
size_t firmwareUploadLastLogged = 0;
bool firmwareUploadInProgress = false;
std::vector<RemoteLogEntry> remoteLogs;
uint32_t nextRemoteLogId = 1;
bool printerPrimed = false;
uint32_t printerBleCooldownUntilMs = 0;
uint32_t printerReadyAtMs = 0;
uint32_t printerDisconnectAtMs = 0;
bool captiveDnsActive = false;
bool bleControllerReady = false;
bool victronBleStarted = false;
bool printerBtStarted = false;
uint32_t lastWifiStatusLogMs = 0;
bool provisioningMode = true;
uint8_t wifiConnectFailures = 0;
bool staConnectRequested = false;
bool staJoinActive = false;
uint32_t staJoinStartedMs = 0;
volatile int printerFlowCredits = 1000;
volatile bool printerFlowControlEnabled = false;
volatile bool printerFlowControlSeen = false;
volatile uint32_t printerFlowLastNotifyMs = 0;
volatile bool printerBatteryKnown = false;
volatile uint8_t printerBatteryRaw = 0;
volatile uint32_t printerBatteryPacketMs = 0;
volatile uint32_t printerLastContactMs = 0;
uint32_t lastVictronDiagLogMs = 0;
PrintSessionState printSession;
String serialCommandBuffer;
uint16_t serialTestRows = 96;
uint16_t serialTestPadRows = 0;
SerialPrinterSession serialPrinter;
IPAddress setupApIp(192, 168, 4, 1);
IPAddress setupApGateway(192, 168, 4, 1);
IPAddress setupApSubnet(255, 255, 255, 0);

bool printerUsesM100LegacyMode() {
  return true;
}

String preferredHost() {
  return String("http://") + settings.hostname + "/";
}

void appendRemoteLogLine(const String &line) {
  if (remoteLogs.size() >= REMOTE_LOG_CAPACITY) {
    remoteLogs.erase(remoteLogs.begin());
  }
  RemoteLogEntry entry;
  entry.id = nextRemoteLogId++;
  entry.line = line;
  remoteLogs.push_back(entry);
}

void logLine(const String &line) {
  Serial.println(line);
  appendRemoteLogLine(line);
}

void logf(const char *format, ...) {
  char stackBuf[192];
  va_list args;
  va_start(args, format);
  int needed = vsnprintf(stackBuf, sizeof(stackBuf), format, args);
  va_end(args);
  if (needed < 0) {
    return;
  }

  String line;
  if (static_cast<size_t>(needed) < sizeof(stackBuf)) {
    line = String(stackBuf);
  } else {
    std::vector<char> heapBuf(static_cast<size_t>(needed) + 1);
    va_start(args, format);
    vsnprintf(heapBuf.data(), heapBuf.size(), format, args);
    va_end(args);
    line = String(heapBuf.data());
  }
  Serial.println(line);
  appendRemoteLogLine(line);
}

void clearSerialPrinterSession() {
  serialPrinter.service = nullptr;
  serialPrinter.writeChar = nullptr;
  serialPrinter.flowChar = nullptr;
  serialPrinter.ff03Subscribed = false;
  serialPrinter.connectedMode = "";
}

void disconnectSerialPrinterSession() {
  if (serialPrinter.flowChar && serialPrinter.ff03Subscribed) {
    serialPrinter.flowChar->unsubscribe(true);
  }
  if (serialPrinter.client) {
    if (serialPrinter.client->isConnected()) {
      serialPrinter.client->disconnect();
    }
    NimBLEDevice::deleteClient(serialPrinter.client);
    serialPrinter.client = nullptr;
  }
  clearSerialPrinterSession();
  printerDisconnectAtMs = 0;
  delay(250);
}

void settleAndDisconnectSerialPrinterSession(uint32_t settleMs) {
  printerDisconnectAtMs = settleMs > 0 ? (millis() + settleMs) : millis();
}

void evictSerialPrinterPeerRecords() {
  if (!settings.printerMac.length()) return;
  const std::string mac(settings.printerMac.c_str());
  NimBLEAddress publicAddr(mac, BLE_ADDR_PUBLIC);
  NimBLEAddress randomAddr(mac, BLE_ADDR_RANDOM);
  NimBLEClient *existing = NimBLEDevice::getClientByPeerAddress(publicAddr);
  if (!existing) existing = NimBLEDevice::getClientByPeerAddress(randomAddr);
  if (existing) {
    NimBLEDevice::deleteClient(existing);
    delay(250);
  }
}

bool connectSerialPrinterSession(String &message) {
  if (!settings.printerMac.length()) {
    message = "No printer MAC configured.";
    return false;
  }
  if (!initBleController() || !victronScan) {
    message = "BLE runtime is not ready.";
    return false;
  }
  if (serialPrinter.client && serialPrinter.client->isConnected() && serialPrinter.writeChar) {
    message = "Serial printer session already connected.";
    return true;
  }
  disconnectSerialPrinterSession();
  evictSerialPrinterPeerRecords();
  if (victronScan && victronScan->isScanning()) {
    victronScan->stop();
    delay(100);
    victronScan->clearResults();
  }
  victronBleStarted = false;
  printerBleCooldownUntilMs = millis() + 5000UL;

  serialPrinter.client = NimBLEDevice::getDisconnectedClient();
  if (!serialPrinter.client) {
    serialPrinter.client = NimBLEDevice::createClient();
  }
  if (!serialPrinter.client) {
    message = "Could not create a BLE client.";
    return false;
  }

  int lastError = 0;
  String addressMode;
  if (!connectPrinterClient(serialPrinter.client, settings.printerMac, lastError, addressMode)) {
    message = String("Could not connect to printer at ") + settings.printerMac + ". NimBLE error " + lastError + ".";
    disconnectSerialPrinterSession();
    return false;
  }

  serialPrinter.service = serialPrinter.client->getService("ff00");
  serialPrinter.writeChar = serialPrinter.service ? serialPrinter.service->getCharacteristic("ff02") : nullptr;
  serialPrinter.flowChar = serialPrinter.service ? serialPrinter.service->getCharacteristic("ff03") : nullptr;
  if (!serialPrinter.service || !serialPrinter.writeChar) {
    message = "Printer service FF00 or write characteristic FF02 was not found.";
    disconnectSerialPrinterSession();
    return false;
  }
  if (serialPrinter.flowChar && serialPrinter.flowChar->canNotify()) {
    if (!serialPrinter.flowChar->subscribe(true, printerFlowNotifyCB)) {
      message = "Could not subscribe to FF03 notifications.";
      disconnectSerialPrinterSession();
      return false;
    }
    serialPrinter.ff03Subscribed = true;
  }
  resetPrinterFlowControl();
  serialPrinter.connectedMode = addressMode;
  delay(250);
  message = String("Serial printer session connected via ") + addressMode + ".";
  return true;
}

String setupApName() {
  return String(DEFAULT_HOSTNAME) + DEFAULT_SETUP_AP_SUFFIX;
}

String setupPortalUrl() {
  return String("http://") + setupApIp.toString() + "/admin";
}

bool wifiLooksConfigured() {
  if (!settings.wifiSsid.length() || settings.wifiSsid == DEFAULT_WIFI_SSID) {
    return false;
  }
  return true;
}

void startSetupAp() {
  if (setupApActive) {
    return;
  }
  logf("Starting setup AP: %s", setupApName().c_str());
  WiFi.disconnect(true, true);
  delay(150);
  WiFi.mode(WIFI_OFF);
  delay(150);
  WiFi.mode(WIFI_AP);
  delay(100);
  WiFi.softAPConfig(setupApIp, setupApGateway, setupApSubnet);
  setupApActive = WiFi.softAP(setupApName().c_str(), nullptr, 6, false, 4);
  if (setupApActive && !captiveDnsActive) {
    dnsServer.start(53, "*", setupApIp);
    captiveDnsActive = true;
  }
  logf("Setup AP %s at %s", setupApActive ? "active" : "failed", WiFi.softAPIP().toString().c_str());
}

void stopSetupAp() {
  if (!setupApActive) {
    return;
  }
  if (captiveDnsActive) {
    dnsServer.stop();
    captiveDnsActive = false;
  }
  WiFi.softAPdisconnect(true);
  setupApActive = false;
}

bool parseMacAddress(const String &text, uint8_t out[6]) {
  unsigned int parts[6];
  if (sscanf(text.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
             &parts[0], &parts[1], &parts[2], &parts[3], &parts[4], &parts[5]) != 6) {
    return false;
  }
  for (size_t i = 0; i < 6; ++i) {
    out[i] = static_cast<uint8_t>(parts[i]);
  }
  return true;
}

bool parseHexNibble(char c, uint8_t &value) {
  if (c >= '0' && c <= '9') { value = static_cast<uint8_t>(c - '0'); return true; }
  if (c >= 'a' && c <= 'f') { value = static_cast<uint8_t>(10 + c - 'a'); return true; }
  if (c >= 'A' && c <= 'F') { value = static_cast<uint8_t>(10 + c - 'A'); return true; }
  return false;
}

bool parseBindKey(const String &hex, std::array<uint8_t, 16> &out) {
  String normalized;
  normalized.reserve(hex.length());
  for (size_t i = 0; i < hex.length(); ++i) {
    const char c = hex[i];
    if (isspace(static_cast<unsigned char>(c)) || c == ':' || c == '-') {
      continue;
    }
    normalized += c;
  }
  if (normalized.length() != 32) return false;
  for (size_t i = 0; i < 16; ++i) {
    uint8_t hi = 0, lo = 0;
    if (!parseHexNibble(normalized[i * 2], hi) || !parseHexNibble(normalized[i * 2 + 1], lo)) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

String isoTime(time_t value) {
  if (value <= 0) return String();
  struct tm t {};
  gmtime_r(&value, &t);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &t);
  return String(buf);
}

String localTimeString(time_t value) {
  if (value <= 0) return String();
  struct tm t {};
  localtime_r(&value, &t);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}

String chargerStateLabel(uint8_t state) {
  switch (state) {
    case 0: return "Off";
    case 2: return "Fault";
    case 3: return "Bulk";
    case 4: return "Absorption";
    case 5: return "Float";
    case 6: return "Storage";
    case 7: return "Equalize";
    case 11: return "Other Hub-1";
    case 245: return "Wake-up";
    case 252: return "External control";
    default: return String("State ") + state;
  }
}

String batteryHealthLabel() {
  const uint32_t age = millis() - telemetry.lastPacketMillis;
  if (!telemetry.valid || age > BLE_STALE_MS || !isfinite(telemetry.batteryVoltage)) return "stale";
  if (telemetry.batteryVoltage >= 14.7f) return "high / charger-check";
  if (telemetry.batteryVoltage >= 13.6f) {
    return telemetry.batteryCurrent >= 0.05f ? "full / charging" : "full";
  }
  if (telemetry.batteryVoltage >= BATTERY_OK_VOLTS) return "healthy";
  if (telemetry.batteryVoltage >= BATTERY_LOW_VOLTS) return "reserve";
  if (telemetry.batteryVoltage <= BATTERY_CRITICAL_VOLTS) return "critical";
  return "low";
}

bool looksGenericStorageLabel(const String &normalized) {
  return normalized.indexOf("MISC") >= 0
      || normalized.indexOf("GENERAL") >= 0
      || normalized.indexOf("ADAPTER") >= 0
      || normalized.indexOf("CABLE") >= 0
      || normalized.indexOf("PARTS") >= 0
      || normalized.indexOf("TOOLS") >= 0;
}

String creativeThemeHint(const String &normalized) {
  if (looksGenericStorageLabel(normalized)) {
    return "Because the category is broad or generic, invent an unexpected but tasteful theme rather than literally illustrating the object. Be creative, go with adventourous, science, garden, kids, or funny/passive aggressive themes to make it as unique as possible";
  }
  return "Let the illustration draw strongly from the label subject while still feeling like a one-off designed storage label.";
}

String openAiImageSizeForLabel(int widthPx, int heightPx) {
  if (widthPx == heightPx) {
    return "1024x1024";
  }
  return widthPx > heightPx ? "1536x1024" : "1024x1536";
}

String effectiveImageProvider() {
  if (settings.imageGenProvider.length()) {
    return settings.imageGenProvider;
  }
  return DEFAULT_IMAGE_GEN_PROVIDER;
}

String effectiveImageModel() {
  if (settings.imageGenModel.length()) {
    return settings.imageGenModel;
  }
  return effectiveImageProvider() == "huggingface" ? DEFAULT_HF_IMAGE_MODEL : DEFAULT_IMAGE_GEN_MODEL;
}

String effectiveImageUrl() {
  if (effectiveImageProvider() == "openai") {
    return "https://api.openai.com/v1/images/generations";
  }
  if (effectiveImageProvider() == "huggingface") {
    return String("https://router.huggingface.co/hf-inference/models/") + effectiveImageModel();
  }
  return settings.imageGenUrl;
}

std::vector<String> configuredLabelSizeKeys();

bool isCircleLabelKey(const String &key) {
  return key.equalsIgnoreCase("50-circle");
}

bool configuredLabelSizeExists(const String &key) {
  if (!key.length()) {
    return false;
  }
  for (const auto &candidate : configuredLabelSizeKeys()) {
    if (candidate.equalsIgnoreCase(key)) {
      return true;
    }
  }
  return false;
}

const char *labelShapeForKey(const String &key) {
  return isCircleLabelKey(key) ? "circle" : "rect";
}

String buildCrazyPrompt(const LabelSpec &spec, const LabelRender &fallback, const String &normalized) {
  String prompt =
      String("I'm making thermal printer labels with ")
      + spec.sizeKey + " size, "
      + spec.orientation + " orientation, "
      + spec.appearance + " appearance, "
      + fallback.widthPx + "x" + fallback.heightPx + " pixel canvas, "
      + "and monochrome thermal-print constraints. "
      + "I'd like you to generate an extremely unique label to go with this text: "
      + normalized + ". "
      + "Acceptable labels may be based on the text itself, or if that's not sufficient, go crazy with something out there and unique. ";

  if (isCircleLabelKey(spec.sizeKey)) {
    prompt += "The label stock is a 50 mm round circle sticker, so keep the composition comfortably inside a circular printable area. ";
  }

  if (spec.shelfHint.length()) {
    prompt += "Shelf hint or placement note: " + spec.shelfHint + ". ";
  }

  prompt +=
      "The final image must fit cleanly on exactly one thermal storage label, with a strong centered title area, clean margins, and composition that survives monochrome printing. "
      "Design for a black-and-white thermal printer first, not for color screens. Use pure black and white shapes with little to no gray. "
      "Keep the artwork bold, simplified, and high contrast with thick shapes, clean silhouettes, and strong foreground/background separation. "
      "Details should be reflecive of the size of the label, with smaller 40mmx30mm labels avoiding intricate details that won't print and larger 50mmx80mm labels using more details and/or dithering for cooler designs. "
      "Keep everything balanced inside the exact canvas bounds so it prints on one label only. "
      "Use blue or floral pastel inspiration only as a creative seed, then translate that into crisp printable black-and-white art. "
      "Make this feel custom and distinct rather than template-like. "
      + creativeThemeHint(normalized) + " "
      + "The title should remain readable and dominant. Return only one finished PNG composition.";

  return prompt;
}

String sanitizeUpstreamErrorMessage(String message) {
  message.trim();
  const int apiKeyPos = message.indexOf("API key provided:");
  if (apiKeyPos >= 0) {
    const int valueStart = apiKeyPos + static_cast<int>(strlen("API key provided:"));
    int valueEnd = message.indexOf('.', valueStart);
    if (valueEnd < 0) {
      valueEnd = message.length();
    }
    message = message.substring(0, valueStart) + " [redacted]" + message.substring(valueEnd);
  }
  return message;
}

bool readHttpResponseBody(HTTPClient &http, String &textBody, std::vector<uint8_t> &binaryBody, bool binaryExpected) {
  WiFiClient *stream = http.getStreamPtr();
  if (!stream) {
    return false;
  }

  const int reportedLength = http.getSize();
  const uint32_t started = millis();
  const uint32_t timeoutMs = 30000;

  if (reportedLength > 0) {
    if (binaryExpected) {
      binaryBody.reserve(reportedLength);
    } else {
      textBody.reserve(reportedLength);
    }
  }

  while (http.connected() || stream->available()) {
    const size_t available = stream->available();
    if (available) {
      uint8_t buffer[512];
      const size_t toRead = std::min(sizeof(buffer), available);
      const size_t read = stream->readBytes(buffer, toRead);
      if (!read) {
        continue;
      }
      if (binaryExpected) {
        binaryBody.insert(binaryBody.end(), buffer, buffer + read);
      } else {
        for (size_t i = 0; i < read; ++i) {
          textBody += static_cast<char>(buffer[i]);
        }
      }
    } else {
      if (millis() - started > timeoutMs) {
        break;
      }
      delay(10);
    }
  }

  return binaryExpected ? !binaryBody.empty() : textBody.length() > 0;
}

void streamHttpImageResponse(HTTPClient &http, const String &normalized, int widthPx, int heightPx) {
  WiFiClient downstream = server.client();
  WiFiClient *upstream = http.getStreamPtr();
  if (!upstream) {
    server.send(502, "application/json", "{\"message\":\"Image upstream stream was unavailable.\"}");
    return;
  }

  downstream.print("HTTP/1.1 200 OK\r\n");
  downstream.print("Content-Type: image/png\r\n");
  downstream.print("Cache-Control: no-store\r\n");
  downstream.print("X-Mervyns-Mode: ai-image\r\n");
  downstream.print("X-Mervyns-Normalized: ");
  downstream.print(normalized);
  downstream.print("\r\n");
  downstream.print("X-Mervyns-Width: ");
  downstream.print(widthPx);
  downstream.print("\r\n");
  downstream.print("X-Mervyns-Height: ");
  downstream.print(heightPx);
  downstream.print("\r\n");
  downstream.print("Connection: close\r\n\r\n");

  uint8_t buffer[1024];
  const uint32_t started = millis();
  while (http.connected() || upstream->available()) {
    const size_t available = upstream->available();
    if (available) {
      const size_t toRead = std::min(sizeof(buffer), available);
      const size_t read = upstream->readBytes(buffer, toRead);
      if (read) {
        downstream.write(buffer, read);
      }
    } else {
      if (millis() - started > HTTP_TIMEOUT_MS) {
        break;
      }
      delay(10);
    }
  }
  downstream.flush();
}

String base64EncodeBytes(const std::vector<uint8_t> &bytes) {
  if (bytes.empty()) {
    return String();
  }
  size_t encodedLen = 0;
  if (mbedtls_base64_encode(nullptr, 0, &encodedLen, bytes.data(), bytes.size()) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
    return String();
  }
  std::vector<unsigned char> encoded(encodedLen + 1, 0);
  if (mbedtls_base64_encode(encoded.data(), encoded.size(), &encodedLen, bytes.data(), bytes.size()) != 0) {
    return String();
  }
  return String(reinterpret_cast<const char *>(encoded.data()));
}

bool base64DecodeToBuffer(const unsigned char *encoded,
                          size_t encodedLen,
                          uint8_t *decodedOut,
                          size_t decodedCapacity,
                          size_t &decodedLenOut) {
  decodedLenOut = 0;
  if (!encoded || encodedLen == 0) {
    return true;
  }
  size_t requiredLen = 0;
  if (mbedtls_base64_decode(nullptr, 0, &requiredLen, encoded, encodedLen) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
    return false;
  }
  if (!decodedOut || requiredLen > decodedCapacity) {
    return false;
  }
  if (mbedtls_base64_decode(decodedOut, decodedCapacity, &decodedLenOut, encoded, encodedLen) != 0) {
    decodedLenOut = 0;
    return false;
  }
  return true;
}

void resetPrinterFlowControl() {
  printerFlowCredits = 1000;
  printerFlowControlEnabled = false;
  printerFlowControlSeen = false;
  printerFlowLastNotifyMs = 0;
}

void printerFlowNotifyCB(NimBLERemoteCharacteristic *characteristic, uint8_t *data, size_t length, bool isNotify) {
  (void)characteristic;
  if (!data || !length) {
    return;
  }
  printerLastContactMs = millis();
  logf("Printer FF03 %s: %s", isNotify ? "notify" : "indicate", hexBytes(data, length).c_str());
  if (data[0] == 0x02 && length >= 2) {
    printerBatteryKnown = true;
    printerBatteryRaw = data[1];
    printerBatteryPacketMs = printerLastContactMs;
  }
  if (length < 2 || data[0] != 0x01) {
    return;
  }
  const int addedCredits = data[1] == 0x04 ? 4 : data[1];
  printerFlowCredits += addedCredits;
  printerFlowControlSeen = true;
  printerFlowLastNotifyMs = millis();
}

bool enablePrinterFlowControl(const NimBLERemoteCharacteristic *flowChar) {
  if (printerUsesM100LegacyMode()) {
    resetPrinterFlowControl();
    return true;
  }
  resetPrinterFlowControl();
  if (!flowChar || !flowChar->canNotify()) {
    return true;
  }
  if (!flowChar->subscribe(true, printerFlowNotifyCB, true)) {
    logLine("Printer flow-control subscribe failed. Using fallback mode.");
    return true;
  }
  printerFlowControlEnabled = true;
  printerFlowCredits = 0;
  delay(50);
  return true;
}

void disablePrinterFlowControl(const NimBLERemoteCharacteristic *flowChar) {
  if (flowChar && printerFlowControlEnabled) {
    flowChar->unsubscribe(true);
  }
  resetPrinterFlowControl();
}

bool waitForPrinterCredit() {
  if (!printerFlowControlEnabled) {
    return true;
  }

  const uint32_t started = millis();
  while (printerFlowCredits <= 0 && millis() - started < PRINTER_FLOW_WAIT_MS) {
    delay(2);
  }

  if (printerFlowCredits > 0) {
    return true;
  }

  if (!printerFlowControlSeen) {
    logLine("Printer flow-control credits never arrived. Falling back to timed writes.");
    printerFlowControlEnabled = false;
    printerFlowCredits = 1000;
    return true;
  }

  return false;
}

uint32_t updateAdler32(uint32_t adler, const uint8_t *data, size_t length) {
  uint32_t s1 = adler & 0xFFFFU;
  uint32_t s2 = (adler >> 16) & 0xFFFFU;
  for (size_t i = 0; i < length; ++i) {
    s1 = (s1 + data[i]) % 65521U;
    s2 = (s2 + s1) % 65521U;
  }
  return (s2 << 16) | s1;
}

size_t printMasterCompressedLength(size_t rasterLength) {
  const size_t blockCount = (rasterLength + PRINTER_ZLIB_BLOCK_SIZE - 1) / PRINTER_ZLIB_BLOCK_SIZE;
  return 2 + rasterLength + (blockCount * 5) + 4;
}

int printerBatteryPercent() {
  if (!printerBatteryKnown || printerBatteryPacketMs == 0) {
    return -1;
  }
  if ((millis() - printerBatteryPacketMs) > PRINTER_BATTERY_STALE_MS) {
    return -1;
  }
  return static_cast<int>((static_cast<uint32_t>(printerBatteryRaw) * 100U + 127U) / 255U);
}

String printerStatusSummary() {
  if (!settings.printerMac.length()) {
    return "No printer MAC saved yet. Use Admin to scan and select one.";
  }
  if (!printerBatteryKnown || printerBatteryPacketMs == 0 || (millis() - printerBatteryPacketMs) > PRINTER_BATTERY_STALE_MS) {
    return String("Printer target saved as ") + settings.printerMac + ". Reachable, but no recent battery packet yet.";
  }
  const uint32_t ageMs = millis() - printerBatteryPacketMs;
  return String("Printer target saved as ") + settings.printerMac + ". Last seen " + (ageMs / 1000UL) + "s ago.";
}

String hexBytes(const uint8_t *data, size_t length) {
  static const char *hex = "0123456789ABCDEF";
  String out;
  for (size_t i = 0; i < length; ++i) {
    if (i) out += ' ';
    out += hex[(data[i] >> 4) & 0x0F];
    out += hex[data[i] & 0x0F];
  }
  return out;
}

bool writePrinterBytes(const NimBLERemoteCharacteristic *characteristic,
                       const uint8_t *data,
                       size_t length,
                       size_t mtuPayload) {
  if (!characteristic || !data || !length) {
    return false;
  }
  const bool useResponse = characteristic->canWriteNoResponse() ? false : characteristic->canWrite();
  size_t offset = 0;
  const size_t chunkSize = std::min(mtuPayload > 0 ? mtuPayload : static_cast<size_t>(20), PRINTER_BLE_CHUNK_SIZE);
  while (offset < length) {
    if (!waitForPrinterCredit()) {
      return false;
    }
    if (printerFlowControlEnabled && printerFlowCredits > 0) {
      --printerFlowCredits;
    }
    const size_t remaining = length - offset;
    const size_t thisChunk = remaining < chunkSize ? remaining : chunkSize;
    bool wrote = false;
    for (int attempt = 0; attempt < 4 && !wrote; ++attempt) {
      wrote = characteristic->writeValue(data + offset, thisChunk, useResponse);
      if (!wrote) {
        delay(20 + attempt * 20);
      }
    }
    if (!wrote) {
      return false;
    }
    offset += thisChunk;
    delay(printerFlowControlEnabled ? 1 : (useResponse ? 10 : 6));
  }
  return true;
}

bool writePrinterBytesSimple(const NimBLERemoteCharacteristic *characteristic,
                             const uint8_t *data,
                             size_t length,
                             size_t mtuPayload) {
  if (!characteristic || !data || !length) {
    return false;
  }
  const bool useResponse = characteristic->canWriteNoResponse() ? false : characteristic->canWrite();
  const size_t chunkSize = std::min(mtuPayload > 0 ? mtuPayload : static_cast<size_t>(20), PRINTER_BLE_CHUNK_SIZE);
  size_t offset = 0;
  while (offset < length) {
    const size_t thisChunk = std::min(chunkSize, length - offset);
    bool wrote = false;
    for (int attempt = 0; attempt < 4 && !wrote; ++attempt) {
      wrote = characteristic->writeValue(data + offset, thisChunk, useResponse);
      if (!wrote) {
        delay(20 + attempt * 20);
      }
    }
    if (!wrote) {
      return false;
    }
    offset += thisChunk;
    delay(useResponse ? 20 : 10);
  }
  return true;
}

bool writePrinterCommand(const NimBLERemoteCharacteristic *characteristic,
                         const uint8_t *data,
                         size_t length,
                         const char *label) {
  if (!characteristic || !data || !length) {
    return false;
  }
  const size_t mtuPayload = std::max<int>(20, characteristic->getClient()->getMTU() - 3);
  const char *mode = characteristic->canWriteNoResponse() ? "no-rsp" : (characteristic->canWrite() ? "rsp" : "unknown");
  logf("Printer cmd %s (%s): %s", label ? label : "unnamed", mode, hexBytes(data, length).c_str());
  return writePrinterBytes(characteristic, data, length, mtuPayload);
}

bool requestPrinterBatteryPacket(const NimBLERemoteCharacteristic *writeChar,
                                 String &errorMessage,
                                 uint32_t waitMs = 1200) {
  static const uint8_t batteryCmd[] = {0x10, 0xFF, 0x50, 0xF1};
  const uint32_t startedMs = millis();
  if (!writePrinterCommand(writeChar, batteryCmd, sizeof(batteryCmd), "get-battery")) {
    errorMessage = "Could not request printer battery status.";
    return false;
  }
  while ((millis() - startedMs) <= waitMs) {
    if (printerBatteryKnown && printerBatteryPacketMs != 0 &&
        static_cast<int32_t>(printerBatteryPacketMs - startedMs) >= 0) {
      char rawHex[8];
      snprintf(rawHex, sizeof(rawHex), "%02X", printerBatteryRaw);
      errorMessage = String("Printer battery refreshed: raw=0x") + rawHex + " (" + printerBatteryPercent() + "%).";
      return true;
    }
    delay(20);
  }
  errorMessage = "Printer did not report a fresh battery packet.";
  return false;
}

void logPrinterReadCharacteristic(NimBLERemoteCharacteristic *readChar) {
  if (!readChar || !readChar->canRead()) {
    return;
  }
  std::string value = readChar->readValue();
  if (!value.empty()) {
    logf("Printer FF01 read: %s", hexBytes(reinterpret_cast<const uint8_t *>(value.data()), value.size()).c_str());
  } else {
    logLine("Printer FF01 read: <empty>");
  }
}

bool writeStoredDeflateBlock(const NimBLERemoteCharacteristic *writeChar,
                             const uint8_t *data,
                             size_t length,
                             bool isFinalBlock,
                             size_t mtuPayload) {
  if (!writeChar || !data || !length || length > 65535) {
    return false;
  }

  const uint16_t len = static_cast<uint16_t>(length);
  const uint16_t nlen = static_cast<uint16_t>(~len);
  const uint8_t header[5] = {
      static_cast<uint8_t>(isFinalBlock ? 0x01 : 0x00),
      static_cast<uint8_t>(len & 0xFF),
      static_cast<uint8_t>((len >> 8) & 0xFF),
      static_cast<uint8_t>(nlen & 0xFF),
      static_cast<uint8_t>((nlen >> 8) & 0xFF)
  };

  return writePrinterBytes(writeChar, header, sizeof(header), mtuPayload)
      && writePrinterBytes(writeChar, data, length, mtuPayload);
}

void cleanupPrintSession() {
  printSession = PrintSessionState{};
}

bool startPrintSession(int widthBytes, int height, size_t expectedRasterLength, String &errorMessage) {
  cleanupPrintSession();

  if (!settings.printerMac.length()) {
    errorMessage = "No printer MAC configured. Use Admin to scan and save your M100 first.";
    return false;
  }
  printSession.active = true;
  printSession.startedAtMs = millis();
  printSession.expectedRasterLength = expectedRasterLength;
  printSession.widthBytes = widthBytes;
  printSession.height = height;
  printSession.receivedRaw = 0;
  printSession.raster.clear();
  printSession.raster.reserve(expectedRasterLength);
  errorMessage = "Print session started.";
  return true;
}

bool appendPrintSessionRaster(const uint8_t *data, size_t length, String &errorMessage) {
  if (!printSession.active || !data || !length) {
    errorMessage = "No active print session.";
    return false;
  }
  if (printSession.receivedRaw + length > printSession.expectedRasterLength) {
    errorMessage = "Print raster exceeded expected size.";
    cleanupPrintSession();
    return false;
  }
  printSession.raster.insert(printSession.raster.end(), data, data + length);
  printSession.receivedRaw += length;
  printSession.startedAtMs = millis();
  errorMessage = "Print chunk accepted.";
  return true;
}

std::vector<String> splitTokens(const String &line) {
  std::vector<String> tokens;
  int start = 0;
  const int len = static_cast<int>(line.length());
  while (start < len) {
    while (start < len && isspace(static_cast<unsigned char>(line[start]))) {
      ++start;
    }
    if (start >= len) break;
    int end = start;
    while (end < len && !isspace(static_cast<unsigned char>(line[end]))) {
      ++end;
    }
    tokens.push_back(line.substring(start, end));
    start = end;
  }
  return tokens;
}

String joinTokens(const std::vector<String> &tokens, size_t startIndex) {
  String out;
  for (size_t i = startIndex; i < tokens.size(); ++i) {
    if (i > startIndex) out += ' ';
    out += tokens[i];
  }
  return out;
}

void setRasterPixel(std::vector<uint8_t> &raster, size_t widthBytes, size_t x, size_t y, bool on = true) {
  if (!on) return;
  const size_t widthPx = widthBytes * 8U;
  if (x >= widthPx) return;
  const size_t byteIndex = y * widthBytes + (x / 8U);
  if (byteIndex >= raster.size()) return;
  raster[byteIndex] |= static_cast<uint8_t>(0x80U >> (x % 8U));
}

const uint8_t *glyphForChar(char ch) {
  static const uint8_t SPACE[5] = {0x00,0x00,0x00,0x00,0x00};
  static const uint8_t DASH[5]  = {0x08,0x08,0x08,0x08,0x08};
  static const uint8_t QUESTION[5] = {0x02,0x01,0x11,0x09,0x06};
  static const uint8_t A[5] = {0x1E,0x05,0x05,0x05,0x1E};
  static const uint8_t B[5] = {0x1F,0x15,0x15,0x15,0x0A};
  static const uint8_t C[5] = {0x0E,0x11,0x11,0x11,0x0A};
  static const uint8_t D[5] = {0x1F,0x11,0x11,0x0A,0x04};
  static const uint8_t E[5] = {0x1F,0x15,0x15,0x15,0x11};
  static const uint8_t F[5] = {0x1F,0x05,0x05,0x05,0x01};
  static const uint8_t G[5] = {0x0E,0x11,0x15,0x15,0x1D};
  static const uint8_t H[5] = {0x1F,0x04,0x04,0x04,0x1F};
  static const uint8_t I[5] = {0x11,0x11,0x1F,0x11,0x11};
  static const uint8_t J[5] = {0x08,0x10,0x10,0x10,0x0F};
  static const uint8_t K[5] = {0x1F,0x04,0x0A,0x11,0x00};
  static const uint8_t L[5] = {0x1F,0x10,0x10,0x10,0x10};
  static const uint8_t M[5] = {0x1F,0x02,0x04,0x02,0x1F};
  static const uint8_t N[5] = {0x1F,0x02,0x04,0x08,0x1F};
  static const uint8_t O[5] = {0x0E,0x11,0x11,0x11,0x0E};
  static const uint8_t P[5] = {0x1F,0x05,0x05,0x05,0x02};
  static const uint8_t Q[5] = {0x0E,0x11,0x19,0x11,0x1E};
  static const uint8_t R[5] = {0x1F,0x05,0x0D,0x15,0x02};
  static const uint8_t S[5] = {0x12,0x15,0x15,0x15,0x09};
  static const uint8_t T[5] = {0x01,0x01,0x1F,0x01,0x01};
  static const uint8_t U[5] = {0x0F,0x10,0x10,0x10,0x0F};
  static const uint8_t V[5] = {0x07,0x08,0x10,0x08,0x07};
  static const uint8_t W[5] = {0x1F,0x08,0x04,0x08,0x1F};
  static const uint8_t X[5] = {0x11,0x0A,0x04,0x0A,0x11};
  static const uint8_t Y[5] = {0x01,0x02,0x1C,0x02,0x01};
  static const uint8_t Z[5] = {0x19,0x15,0x15,0x13,0x00};
  static const uint8_t N0[5] = {0x0E,0x19,0x15,0x13,0x0E};
  static const uint8_t N1[5] = {0x00,0x12,0x1F,0x10,0x00};
  static const uint8_t N2[5] = {0x12,0x19,0x15,0x15,0x12};
  static const uint8_t N3[5] = {0x09,0x11,0x15,0x15,0x0A};
  static const uint8_t N4[5] = {0x07,0x04,0x04,0x1F,0x04};
  static const uint8_t N5[5] = {0x17,0x15,0x15,0x15,0x09};
  static const uint8_t N6[5] = {0x0E,0x15,0x15,0x15,0x08};
  static const uint8_t N7[5] = {0x01,0x01,0x1D,0x03,0x01};
  static const uint8_t N8[5] = {0x0A,0x15,0x15,0x15,0x0A};
  static const uint8_t N9[5] = {0x02,0x05,0x05,0x05,0x1E};

  switch (toupper(static_cast<unsigned char>(ch))) {
    case 'A': return A; case 'B': return B; case 'C': return C; case 'D': return D;
    case 'E': return E; case 'F': return F; case 'G': return G; case 'H': return H;
    case 'I': return I; case 'J': return J; case 'K': return K; case 'L': return L;
    case 'M': return M; case 'N': return N; case 'O': return O; case 'P': return P;
    case 'Q': return Q; case 'R': return R; case 'S': return S; case 'T': return T;
    case 'U': return U; case 'V': return V; case 'W': return W; case 'X': return X;
    case 'Y': return Y; case 'Z': return Z;
    case '0': return N0; case '1': return N1; case '2': return N2; case '3': return N3;
    case '4': return N4; case '5': return N5; case '6': return N6; case '7': return N7;
    case '8': return N8; case '9': return N9;
    case '-': return DASH;
    case ' ': return SPACE;
    default: return QUESTION;
  }
}

std::vector<uint8_t> makeSolidRaster(size_t widthBytes, size_t height, uint8_t fillByte) {
  return std::vector<uint8_t>(widthBytes * height, fillByte);
}

std::vector<uint8_t> makeCheckerRaster(size_t widthBytes, size_t height) {
  std::vector<uint8_t> raster(widthBytes * height, 0x00);
  if (raster.empty()) return raster;
  const size_t widthPx = widthBytes * 8U;
  for (size_t y = 0; y < height; ++y) {
    for (size_t x = 0; x < widthPx; ++x) {
      if ((((x / 16U) + (y / 16U)) % 2U) == 0U) {
        setRasterPixel(raster, widthBytes, x, y);
      }
    }
  }
  return raster;
}

std::vector<uint8_t> makeCenterBarRaster(size_t widthBytes, size_t height) {
  std::vector<uint8_t> raster(widthBytes * height, 0x00);
  if (raster.empty()) return raster;
  const size_t widthPx = widthBytes * 8U;
  const size_t left = widthPx / 2U > 28U ? (widthPx / 2U) - 28U : 0U;
  const size_t right = std::min(widthPx, left + 56U);
  for (size_t y = 0; y < height; ++y) {
    if (y < 4 || y + 4 >= height) {
      for (size_t x = 0; x < widthPx; ++x) setRasterPixel(raster, widthBytes, x, y);
      continue;
    }
    for (size_t x = left; x < right; ++x) setRasterPixel(raster, widthBytes, x, y);
  }
  return raster;
}

std::vector<uint8_t> makeBoxRaster(size_t widthBytes, size_t height) {
  std::vector<uint8_t> raster(widthBytes * height, 0x00);
  if (raster.empty()) return raster;
  const size_t widthPx = widthBytes * 8U;
  const size_t left = widthPx / 8U;
  const size_t right = widthPx - left;
  const size_t top = std::max<size_t>(4, height / 6U);
  const size_t bottom = height > top + 4 ? height - top : height;
  for (size_t y = 0; y < height; ++y) {
    for (size_t x = left; x < right; ++x) {
      const bool edge = (y >= top && y < top + 4) || (y >= bottom - 4 && y < bottom) ||
                        ((x >= left && x < left + 4) || (x >= right - 4 && x < right)) && y >= top && y < bottom;
      if (edge) setRasterPixel(raster, widthBytes, x, y);
    }
  }
  return raster;
}

std::vector<uint8_t> makeTextRaster(size_t widthBytes, size_t height, String text) {
  std::vector<uint8_t> raster(widthBytes * height, 0x00);
  if (raster.empty()) return raster;
  text.trim();
  text.toUpperCase();
  if (!text.length()) text = "TEST";

  const size_t widthPx = widthBytes * 8U;
  int scale = std::max<int>(1, std::min<int>(6, static_cast<int>(height / 12U)));
  size_t textWidth = text.length() ? (text.length() * (6U * scale)) - scale : 0U;
  while (textWidth > widthPx - 16U && scale > 1) {
    --scale;
    textWidth = text.length() ? (text.length() * (6U * scale)) - scale : 0U;
  }
  const size_t glyphHeight = 7U * scale;
  const size_t startX = textWidth < widthPx ? (widthPx - textWidth) / 2U : 0U;
  const size_t startY = glyphHeight < height ? (height - glyphHeight) / 2U : 0U;

  size_t cursorX = startX;
  for (size_t i = 0; i < text.length(); ++i) {
    const uint8_t *glyph = glyphForChar(text[i]);
    for (size_t col = 0; col < 5; ++col) {
      for (size_t row = 0; row < 7; ++row) {
        if ((glyph[col] >> row) & 0x01U) {
          for (int dx = 0; dx < scale; ++dx) {
            for (int dy = 0; dy < scale; ++dy) {
              setRasterPixel(raster, widthBytes, cursorX + col * scale + dx, startY + row * scale + dy);
            }
          }
        }
      }
    }
    cursorX += 6U * scale;
  }

  for (size_t x = 0; x < widthPx; ++x) {
    setRasterPixel(raster, widthBytes, x, 0);
    if (height > 1) setRasterPixel(raster, widthBytes, x, height - 1);
  }
  return raster;
}

std::vector<uint8_t> prependBlankRows(const std::vector<uint8_t> &content, size_t widthBytes, size_t padRows) {
  if (!padRows || !widthBytes) {
    return content;
  }
  std::vector<uint8_t> raster(widthBytes * padRows, 0x00);
  raster.insert(raster.end(), content.begin(), content.end());
  return raster;
}

bool runDirectRasterPrint(const std::vector<uint8_t> &raster, int widthBytes, int height, const String &label, String &errorMessage) {
  if (printSession.active) {
    errorMessage = "A label upload is still active.";
    return false;
  }
  if (!serialPrinter.client || !serialPrinter.client->isConnected() || !serialPrinter.writeChar) {
    errorMessage = "Serial printer session is not connected. Run 'm100 connect' first.";
    return false;
  }
  if (raster.empty() || widthBytes <= 0 || height <= 0 || raster.size() != static_cast<size_t>(widthBytes) * static_cast<size_t>(height)) {
    errorMessage = "Debug raster dimensions were invalid.";
    return false;
  }
  if (printerReadyAtMs && static_cast<int32_t>(millis() - printerReadyAtMs) < 0) {
    const uint32_t waitMs = printerReadyAtMs - millis();
    logf("Waiting %u ms for printer to settle after calibration.", static_cast<unsigned>(waitMs));
    while (static_cast<int32_t>(millis() - printerReadyAtMs) < 0) {
      delay(20);
    }
  }
  logLine(String("Starting direct serial print: ") + label + " (" + (widthBytes * 8) + "x" + height + ")");
  const bool ok = sendBufferedLegacyBareHoldExact(serialPrinter.writeChar,
                                                  raster,
                                                  static_cast<uint16_t>(widthBytes),
                                                  static_cast<uint16_t>(height),
                                                  errorMessage);
  logLine(ok ? "Direct serial print completed." : "Direct serial print failed.");
  return ok;
}

std::vector<uint8_t> makeM100LabInkRaster(size_t widthBytes, size_t height) {
  std::vector<uint8_t> raster(widthBytes * height, 0x00);
  if (raster.empty() || widthBytes == 0) {
    return raster;
  }

  for (size_t y = 0; y < height; ++y) {
    uint8_t *row = raster.data() + (y * widthBytes);
    if (y < 2 || (y + 2) >= height) {
      memset(row, 0xFF, widthBytes);
      continue;
    }
    if (((y / 4U) % 2U) == 0U) {
      memset(row, 0xFF, widthBytes);
      continue;
    }

    row[0] = 0xFF;
    row[widthBytes - 1] = 0xFF;
    if (widthBytes > 8) {
      memset(row + (widthBytes / 3U), 0xFF, widthBytes / 3U);
    }
  }
  return raster;
}

bool writePrinterBytesLabExact(const NimBLERemoteCharacteristic *characteristic,
                               const uint8_t *data,
                               size_t length,
                               const char *label) {
  if (!characteristic || !data || !length) {
    return false;
  }
  const bool canUseNoRsp = characteristic->canWriteNoResponse();
  const bool canUseRsp = characteristic->canWrite();
  if (!canUseNoRsp && !canUseRsp) {
    return false;
  }
  const bool useResponse = canUseNoRsp ? false : true;
  const size_t chunkSize = std::min<size_t>(PRINTER_BLE_CHUNK_SIZE,
                                            std::max<int>(20, characteristic->getClient() ? characteristic->getClient()->getMTU() - 3 : 20));
  if (label && *label) {
    logf("Printer cmd %s (%s): %u bytes",
         label,
         useResponse ? "rsp-fallback" : "no-rsp",
         static_cast<unsigned>(length));
  }
  size_t offset = 0;
  while (offset < length) {
    const size_t thisChunk = std::min(chunkSize, length - offset);
    bool ok = false;
    if (!useResponse && canUseNoRsp) {
      ok = characteristic->writeValue(data + offset, thisChunk, false);
    } else {
      ok = characteristic->writeValue(data + offset, thisChunk, true);
    }
    if (!ok) {
      logf("Printer chunk write failed at %u/%u, rc=%d",
           static_cast<unsigned>(offset),
           static_cast<unsigned>(length),
           characteristic->getClient() ? characteristic->getClient()->getLastError() : -1);
      return false;
    }
    offset += thisChunk;
    delay(useResponse ? 20 : 10);
  }
  return true;
}

bool sendM100LabLegacyInkBareHoldExact(NimBLERemoteCharacteristic *writeChar, String &errorMessage) {
  if (!writeChar) {
    errorMessage = "Printer write characteristic was not ready.";
    return false;
  }
  constexpr uint16_t kWidthBytes = 48;
  constexpr uint16_t kHeight = 240;
  const std::vector<uint8_t> raster = makeM100LabInkRaster(kWidthBytes, kHeight);
  if (raster.empty()) {
    errorMessage = "M100 lab raster could not be prepared.";
    return false;
  }
  const uint8_t rasterHeader[] = {
      0x1D, 0x76, 0x30, 0x00,
      static_cast<uint8_t>(kWidthBytes & 0xFF), 0x00,
      static_cast<uint8_t>(kHeight & 0xFF),
      static_cast<uint8_t>((kHeight >> 8) & 0xFF)
  };
  if (!writePrinterBytesLabExact(writeChar, rasterHeader, sizeof(rasterHeader), "legacyinkbare bare-raster-header")) {
    errorMessage = "M100 lab bare header failed.";
    return false;
  }
  delay(80);
  if (!writePrinterBytesLabExact(writeChar, raster.data(), raster.size(), "legacyinkbare bare-raster")) {
    errorMessage = "M100 lab bare raster transfer failed.";
    return false;
  }
  logLine("legacyinkbare bare footer skipped");
  errorMessage = "M100 lab legacyinkbarehold sequence sent.";
  return true;
}

bool sendBufferedLegacyBareHoldExact(NimBLERemoteCharacteristic *writeChar,
                                     const std::vector<uint8_t> &raster,
                                     uint16_t widthBytes,
                                     uint16_t height,
                                     String &errorMessage) {
  if (!writeChar) {
    errorMessage = "Printer write characteristic was not ready.";
    return false;
  }
  if (raster.empty() || raster.size() != static_cast<size_t>(widthBytes) * static_cast<size_t>(height)) {
    errorMessage = "Buffered raster dimensions were invalid.";
    return false;
  }
  const uint8_t rasterHeader[] = {
      0x1D, 0x76, 0x30, 0x00,
      static_cast<uint8_t>(widthBytes & 0xFF), 0x00,
      static_cast<uint8_t>(height & 0xFF),
      static_cast<uint8_t>((height >> 8) & 0xFF)
  };
  if (!writePrinterBytesLabExact(writeChar, rasterHeader, sizeof(rasterHeader), "label bare-raster-header")) {
    errorMessage = "Label bare header failed.";
    return false;
  }
  delay(80);
  if (!writePrinterBytesLabExact(writeChar, raster.data(), raster.size(), "label bare-raster")) {
    errorMessage = "Label bare raster transfer failed.";
    return false;
  }
  logLine("label bare footer skipped");
  errorMessage = "Queued label print sent.";
  return true;
}

bool runM100CalibrationOnCharacteristic(NimBLERemoteCharacteristic *writeChar, String &errorMessage) {
  if (!writeChar) {
    errorMessage = "Printer write characteristic was not ready.";
    return false;
  }
  const auto sendCmd = [writeChar](const uint8_t *data,
                                   size_t length,
                                   const char *label,
                                   uint32_t settleMs) -> bool {
    if (!writePrinterBytesLabExact(writeChar, data, length, label)) {
      return false;
    }
    delay(settleMs);
    return true;
  };

  const uint8_t session[] = {0x1F, 0xB2, 0x00};
  const uint8_t setPaper[] = {0x1F, 0x80, 0x01, 0x0A};
  const uint8_t setLocation[] = {0x1F, 0x12, 0x20, 0x00};
  const uint8_t startPrint[] = {0x1F, 0xC0, 0x01, 0x00};
  const uint8_t alignStart[] = {0x1F, 0x11, 0x51};
  const uint8_t alignEnd[] = {0x1F, 0x11, 0x50};
  const uint8_t stopPrint[] = {0x1F, 0xC0, 0x01, 0x01};

  const bool ok = sendCmd(session, sizeof(session), "calibrate session", 120)
      && sendCmd(setPaper, sizeof(setPaper), "calibrate set-paper", 180)
      && sendCmd(setLocation, sizeof(setLocation), "calibrate set-location", 180)
      && sendCmd(startPrint, sizeof(startPrint), "calibrate start-print", 180)
      && sendCmd(alignStart, sizeof(alignStart), "calibrate align-start", 260)
      && sendCmd(alignEnd, sizeof(alignEnd), "calibrate align-end", 260)
      && sendCmd(stopPrint, sizeof(stopPrint), "calibrate stop-print", 200);
  if (ok) {
    printerReadyAtMs = millis() + 1500UL;
  }
  errorMessage = ok ? "M100 calibration sequence sent." : "M100 calibration sequence failed.";
  return ok;
}

bool finishPrintSession(String &errorMessage) {
  if (!printSession.active) {
    errorMessage = "No active print session.";
    return false;
  }
  if (printSession.receivedRaw != printSession.expectedRasterLength || printSession.raster.size() != printSession.expectedRasterLength) {
    errorMessage = "M100 raster upload was incomplete.";
    cleanupPrintSession();
    return false;
  }
  const std::vector<uint8_t> raster = printSession.raster;
  const int widthBytes = printSession.widthBytes;
  const int height = printSession.height;
  String connectMessage;
  if (!connectSerialPrinterSession(connectMessage)) {
    errorMessage = connectMessage;
    cleanupPrintSession();
    return false;
  }
  const bool ok = sendBufferedLegacyBareHoldExact(serialPrinter.writeChar,
                                                  raster,
                                                  static_cast<uint16_t>(widthBytes),
                                                  static_cast<uint16_t>(height),
                                                  errorMessage);
  settleAndDisconnectSerialPrinterSession();
  cleanupPrintSession();
  return ok;
}

bool withConnectedPrinter(const std::function<bool(NimBLEClient *, NimBLERemoteService *, NimBLERemoteCharacteristic *, NimBLERemoteCharacteristic *, String &)> &fn,
                          String &errorMessage) {
  if (!settings.printerMac.length()) {
    errorMessage = "No printer MAC configured. Save a printer address first.";
    return false;
  }
  if (!initBleController() || !victronScan) {
    errorMessage = "BLE runtime is not ready.";
    return false;
  }
  if (victronScan->isScanning()) {
    victronScan->stop();
    delay(100);
  }
  if (serialPrinter.client && serialPrinter.client->isConnected()) {
    if (printerDisconnectAtMs && static_cast<int32_t>(millis() - printerDisconnectAtMs) < 0) {
      errorMessage = "Printer is still finishing the last job.";
      return false;
    }
    logLine("Closing active serial printer session before transient printer operation.");
    disconnectSerialPrinterSession();
  }
  evictSerialPrinterPeerRecords();
  if (setupApActive && WiFi.status() == WL_CONNECTED) {
    logLine("Stopping setup AP for printer operation.");
    stopSetupAp();
    delay(200);
  }
  NimBLEClient *client = NimBLEDevice::createClient();
  if (!client) {
    errorMessage = "Could not create a BLE client.";
    return false;
  }
  int lastError = 0;
  String addressMode;
  if (!connectPrinterClient(client, settings.printerMac, lastError, addressMode)) {
    errorMessage = String("Could not connect to printer at ") + settings.printerMac + ". NimBLE error " + lastError + ".";
    NimBLEDevice::deleteClient(client);
    return false;
  }
  NimBLERemoteService *service = client->getService("ff00");
  NimBLERemoteCharacteristic *writeChar = service ? service->getCharacteristic("ff02") : nullptr;
  NimBLERemoteCharacteristic *flowChar = service ? service->getCharacteristic("ff03") : nullptr;
  if (!service || !writeChar) {
    errorMessage = "Printer service FF00 or write characteristic FF02 was not found.";
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    return false;
  }
  enablePrinterFlowControl(flowChar);
  String opMessage;
  const bool ok = fn(client, service, writeChar, flowChar, opMessage);
  disablePrinterFlowControl(flowChar);
  client->disconnect();
  NimBLEDevice::deleteClient(client);
  errorMessage = opMessage;
  return ok;
}

uint32_t extractBitsLE(const uint8_t *data, size_t len, size_t startBit, size_t bitCount) {
  uint32_t value = 0;
  for (size_t i = 0; i < bitCount; ++i) {
    const size_t bit = startBit + i;
    if ((bit / 8) >= len) break;
    value |= ((data[bit / 8] >> (bit % 8)) & 0x01) << i;
  }
  return value;
}

int32_t signExtend(uint32_t value, size_t bits) {
  const uint32_t mask = 1UL << (bits - 1);
  return static_cast<int32_t>((value ^ mask) - mask);
}

bool decryptVictronPayload(const uint8_t *record, size_t recordLen, std::vector<uint8_t> &payloadOut) {
  if (recordLen < 4 || !bindKeyLoaded) return false;
  if (record[0] != VICTRON_RECORD_SOLAR_CHARGER || record[3] != bindKey[0]) return false;

  const uint16_t nonce = static_cast<uint16_t>(record[1]) | (static_cast<uint16_t>(record[2]) << 8);
  payloadOut.resize(recordLen - 4);
  uint8_t nonceCounter[16] = {};
  uint8_t streamBlock[16] = {};
  size_t offset = 0;
  nonceCounter[0] = static_cast<uint8_t>(nonce & 0xFF);
  nonceCounter[1] = static_cast<uint8_t>((nonce >> 8) & 0xFF);

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  if (mbedtls_aes_setkey_enc(&aes, bindKey.data(), 128) != 0) {
    mbedtls_aes_free(&aes);
    return false;
  }
  const int rc = mbedtls_aes_crypt_ctr(&aes, payloadOut.size(), &offset, nonceCounter, streamBlock, record + 4, payloadOut.data());
  mbedtls_aes_free(&aes);
  return rc == 0;
}

void parseVictronPayload(const std::vector<uint8_t> &payload, int rssi) {
  if (payload.size() < 12) return;
  telemetry.deviceState = static_cast<uint8_t>(extractBitsLE(payload.data(), payload.size(), 0, 8));
  telemetry.errorCode = static_cast<uint8_t>(extractBitsLE(payload.data(), payload.size(), 8, 8));
  const int16_t bv = static_cast<int16_t>(signExtend(extractBitsLE(payload.data(), payload.size(), 16, 16), 16));
  const int16_t bc = static_cast<int16_t>(signExtend(extractBitsLE(payload.data(), payload.size(), 32, 16), 16));
  const uint16_t yd = static_cast<uint16_t>(extractBitsLE(payload.data(), payload.size(), 48, 16));
  const uint16_t pv = static_cast<uint16_t>(extractBitsLE(payload.data(), payload.size(), 64, 16));
  const uint16_t load = static_cast<uint16_t>(extractBitsLE(payload.data(), payload.size(), 80, 9));
  telemetry.batteryVoltage = bv == 0x7FFF ? NAN : bv * 0.01f;
  telemetry.batteryCurrent = bc == 0x7FFF ? NAN : bc * 0.1f;
  telemetry.yieldTodayKwh = yd == 0xFFFF ? NAN : yd * 0.01f;
  telemetry.pvPowerWatts = pv == 0xFFFF ? NAN : pv * 1.0f;
  telemetry.loadCurrent = load == 0x1FF ? NAN : load * 0.1f;
  telemetry.rssi = rssi;
  telemetry.valid = true;
  telemetry.packetCounter++;
  telemetry.lastPacketMillis = millis();
  telemetry.lastPacketEpoch = time(nullptr);
  telemetry.hasTime = telemetry.lastPacketEpoch > 1700000000;
}

bool processVictronAdvertisedDevice(const NimBLEAdvertisedDevice &advertisedDevice) {
  const String mac = String(advertisedDevice.getAddress().toString().c_str());
  if (!mac.equalsIgnoreCase(settings.victronMac)) return false;
  if (!advertisedDevice.haveManufacturerData()) return false;
  std::string manufacturer = advertisedDevice.getManufacturerData();
  if (manufacturer.size() < 10) return false;
  const uint8_t *raw = reinterpret_cast<const uint8_t *>(manufacturer.data());
  const uint16_t companyId = static_cast<uint16_t>(raw[0]) | (static_cast<uint16_t>(raw[1]) << 8);
  if (companyId != VICTRON_COMPANY_ID) return false;
  if (raw[2] != 0x10) {
    return false;
  }
  const uint8_t readoutType = raw[5];
  if (readoutType != 0xA0) {
    if (millis() - lastVictronDiagLogMs >= 5000) {
      lastVictronDiagLogMs = millis();
      logf("Victron advertisement matched %s but readout type was 0x%02X instead of 0xA0", mac.c_str(), readoutType);
    }
    return false;
  }
  std::vector<uint8_t> payload;
  if (!decryptVictronPayload(raw + 6, manufacturer.size() - 6, payload)) {
    if (millis() - lastVictronDiagLogMs >= 5000) {
      lastVictronDiagLogMs = millis();
      logf("Victron advertisement matched %s but decryption failed. Bind key loaded=%s readoutType=0x%02X recordLen=%u",
           mac.c_str(),
           bindKeyLoaded ? "yes" : "no",
           readoutType,
           static_cast<unsigned>(manufacturer.size() - 6));
    }
    return false;
  }
  parseVictronPayload(payload, advertisedDevice.getRSSI());
  if (millis() - lastVictronDiagLogMs >= 5000) {
    lastVictronDiagLogMs = millis();
    logf("Victron packet decoded from %s RSSI=%d battery=%.2fV pv=%.0fW",
         mac.c_str(),
         advertisedDevice.getRSSI(),
         telemetry.batteryVoltage,
         telemetry.pvPowerWatts);
  }
  return true;
}

void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    provisioningMode = false;
    wifiConnectFailures = 0;
    staJoinActive = false;
    hadStationConnection = true;
    if (stationConnectedSinceMs == 0) {
      stationConnectedSinceMs = millis();
    }
    if (!mdnsStarted) {
      if (MDNS.begin(settings.hostname.c_str())) {
        mdnsStarted = MDNS.addService("http", "tcp", 80);
        if (!mdnsStarted) {
          MDNS.end();
          logLine("mDNS start failed while adding http service.");
        }
      } else {
        logLine("mDNS host start failed.");
      }
    }
    if (setupApActive && millis() - stationConnectedSinceMs > 15000) {
      logLine("Stopping setup AP after stable station connection.");
      stopSetupAp();
    }
    return;
  }
  stationConnectedSinceMs = 0;
  if (mdnsStarted) {
    MDNS.end();
    mdnsStarted = false;
  }
  startSetupAp();
  if (!wifiLooksConfigured() || provisioningMode || !staConnectRequested) {
    return;
  }
  if (hadStationConnection && WiFi.status() != WL_CONNECTED && millis() - lastWiFiAttemptMs >= WIFI_RETRY_MS) {
    lastWiFiAttemptMs = millis();
    WiFi.mode(setupApActive ? WIFI_AP_STA : WIFI_STA);
    WiFi.setHostname(settings.hostname.c_str());
    logLine("Trying Wi-Fi reconnect...");
    if (WiFi.reconnect()) {
      staJoinActive = true;
      staJoinStartedMs = millis();
      return;
    }
  }
  if (staJoinActive) {
    return;
  }
  if (millis() - lastWiFiAttemptMs < WIFI_RETRY_MS) return;
  lastWiFiAttemptMs = millis();
  WiFi.mode(setupApActive ? WIFI_AP_STA : WIFI_STA);
  WiFi.setHostname(settings.hostname.c_str());
  WiFi.disconnect(false, false);
  delay(100);
  logf("Attempting Wi-Fi join: %s", settings.wifiSsid.c_str());
  WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPassword.c_str());
  staJoinActive = true;
  staJoinStartedMs = millis();
}

void ensureTime() {
  if (time(nullptr) > 1700000000) return;
  if (WiFi.status() != WL_CONNECTED || millis() - lastNtpAttemptMs < NTP_RETRY_MS) return;
  lastNtpAttemptMs = millis();
  configTzTime(LOS_ANGELES_TZ, "pool.ntp.org", "time.nist.gov");
}

void sendJson(JsonDocument &doc, int code = 200) {
  String body;
  serializeJson(doc, body);
  server.send(code, "application/json", body);
}

bool parseJsonBody(JsonDocument &doc) {
  if (!server.hasArg("plain")) return false;
  return deserializeJson(doc, server.arg("plain")) == DeserializationError::Ok;
}

bool locateJsonStringField(const String &body, const char *key, int &valueStart, int &valueEnd) {
  const String needle = String("\"") + key + "\":";
  const int keyPos = body.indexOf(needle);
  if (keyPos < 0) return false;
  valueStart = keyPos + needle.length();
  while (valueStart < static_cast<int>(body.length()) && isspace(static_cast<unsigned char>(body[valueStart]))) {
    ++valueStart;
  }
  if (valueStart >= static_cast<int>(body.length()) || body[valueStart] != '\"') {
    return false;
  }
  ++valueStart;
  valueEnd = body.indexOf('\"', valueStart);
  if (valueEnd < 0) return false;
  return true;
}

bool extractJsonIntField(const String &body, const char *key, int &out) {
  const String needle = String("\"") + key + "\":";
  const int keyPos = body.indexOf(needle);
  if (keyPos < 0) return false;
  int valuePos = keyPos + needle.length();
  while (valuePos < static_cast<int>(body.length()) && isspace(static_cast<unsigned char>(body[valuePos]))) {
    ++valuePos;
  }
  int endPos = valuePos;
  while (endPos < static_cast<int>(body.length()) && (body[endPos] == '-' || isdigit(static_cast<unsigned char>(body[endPos])))) {
    ++endPos;
  }
  if (endPos <= valuePos) return false;
  out = body.substring(valuePos, endPos).toInt();
  return true;
}

void loadSettingsIntoRuntime() {
  if (!settings.hostname.length() || settings.hostname.equalsIgnoreCase("PackRat")) {
    settings.hostname = DEFAULT_HOSTNAME;
  }
  if (!settings.defaultLabelSize.length()) {
    settings.defaultLabelSize = DEFAULT_LABEL_SIZE;
  }
  if (!settings.defaultOrientation.length()) {
    settings.defaultOrientation = DEFAULT_LABEL_ORIENTATION;
  }
  if (!settings.defaultAppearance.length()) {
    settings.defaultAppearance = DEFAULT_LABEL_APPEARANCE;
  }
  bindKeyLoaded = parseBindKey(settings.victronBindKey, bindKey);
  WiFi.setHostname(settings.hostname.c_str());
  WiFi.setSleep(BLE_RUNTIME_ENABLED);
  if (!settings.imageGenProvider.length()) {
    settings.imageGenProvider = DEFAULT_IMAGE_GEN_PROVIDER;
  }
  if (!settings.imageGenModel.length()) {
    settings.imageGenModel = DEFAULT_IMAGE_GEN_MODEL;
  }
}

bool initBleController() {
  if (!BLE_RUNTIME_ENABLED) {
    logLine("BLE runtime disabled for this build to avoid controller aborts on this board/core.");
    return false;
  }
  if (bleControllerReady) {
    return true;
  }
  logLine("Starting shared BLE service...");
  logLine("NimBLEDevice::init...");
  NimBLEDevice::init("");
  logLine("BLE scan setup...");
  victronScan = NimBLEDevice::getScan();
  victronScan->setActiveScan(false);
  victronScan->setInterval(80);
  victronScan->setWindow(40);
  victronScan->setDuplicateFilter(0);
  bleControllerReady = true;
  logLine("Shared BLE service ready.");
  return true;
}

bool ensureVictronBleStarted() {
  if (!initBleController()) {
    return false;
  }
  if (printerBleCooldownUntilMs && static_cast<int32_t>(millis() - printerBleCooldownUntilMs) < 0) {
    return false;
  }
  if (victronBleStarted) {
    return true;
  }
  victronBleStarted = true;
  logLine("Victron BLE scanning enabled.");
  return true;
}

bool ensurePrinterBtStarted() {
  (void)printerBtStarted;
  (void)PRINTER_CLASSIC_TRANSPORT_ENABLED;
  logLine("Printer Bluetooth Classic transport disabled in this build.");
  return false;
}

std::vector<DiscoveredDevice> scanVictronDevices() {
  std::vector<DiscoveredDevice> devices;
  if (!ensureVictronBleStarted()) {
    return devices;
  }
  NimBLEScanResults results = victronScan->getResults(4000, false);
  for (int i = 0; i < results.getCount(); ++i) {
    const NimBLEAdvertisedDevice *d = results.getDevice(i);
    if (!d || !d->haveManufacturerData()) continue;
    std::string m = d->getManufacturerData();
    if (m.size() < 2) continue;
    const uint8_t *raw = reinterpret_cast<const uint8_t *>(m.data());
    const uint16_t companyId = static_cast<uint16_t>(raw[0]) | (static_cast<uint16_t>(raw[1]) << 8);
    if (companyId != VICTRON_COMPANY_ID) continue;
    DiscoveredDevice item;
    item.name = String(d->getName().c_str());
    item.address = String(d->getAddress().toString().c_str());
    item.rssi = d->getRSSI();
    devices.push_back(item);
    processVictronAdvertisedDevice(*d);
  }
  victronScan->clearResults();
  return devices;
}

std::vector<DiscoveredDevice> scanPrinterDevices() {
  std::vector<DiscoveredDevice> devices;
  if (!initBleController() || !victronScan) {
    return devices;
  }
  if (victronScan->isScanning()) {
    victronScan->stop();
    delay(100);
  }
  victronScan->setActiveScan(true);
  victronScan->setInterval(120);
  victronScan->setWindow(110);
  NimBLEScanResults results = victronScan->getResults(5000, false);
  for (int i = 0; i < results.getCount(); ++i) {
    const NimBLEAdvertisedDevice *d = results.getDevice(i);
    if (!d) continue;
    const String name = String(d->getName().c_str());
    const String upper = [] (String value) {
      value.toUpperCase();
      return value;
    }(name);
    if (upper.length() && upper.indexOf("PHOMEMO") < 0 && upper.indexOf("M110") < 0
        && upper.indexOf("M100") < 0 && upper.indexOf("M120") < 0
        && upper.indexOf("M200") < 0 && upper.indexOf("M220") < 0) {
      continue;
    }
    DiscoveredDevice item;
    item.name = name;
    item.address = String(d->getAddress().toString().c_str());
    item.rssi = d->getRSSI();
    devices.push_back(item);
  }
  victronScan->clearResults();
  victronScan->setActiveScan(false);
  victronScan->setInterval(80);
  victronScan->setWindow(40);
  return devices;
}

void handleMeta() {
  JsonDocument doc;
  const bool batteryFresh = printerBatteryPercent() >= 0;
  doc["preferredHost"] = preferredHost();
  doc["mdnsHost"] = String("http://") + settings.hostname + ".local/";
  doc["wifiConnected"] = WiFi.status() == WL_CONNECTED;
  doc["wifiStatus"] = static_cast<int>(WiFi.status());
  doc["stationIp"] = WiFi.localIP().toString();
  doc["stationSsid"] = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "";
  doc["setupApActive"] = setupApActive;
  doc["setupApSsid"] = setupApName();
  doc["setupApIp"] = WiFi.softAPIP().toString();
  doc["setupPortalUrl"] = setupPortalUrl();
  doc["firmwareVersion"] = String(__DATE__) + " " + __TIME__;
  doc["timezone"] = "America/Los_Angeles";
  JsonObject defaults = doc["defaults"].to<JsonObject>();
  defaults["labelSize"] = settings.defaultLabelSize;
  defaults["orientation"] = settings.defaultOrientation;
  defaults["appearance"] = settings.defaultAppearance;
  JsonArray sizes = doc["labelSizes"].to<JsonArray>();
  for (const auto &key : configuredLabelSizeKeys()) sizes.add(key);
  JsonObject printer = doc["printer"].to<JsonObject>();
  printer["message"] = printerStatusSummary();
  printer["batteryKnown"] = batteryFresh;
  printer["batteryRaw"] = batteryFresh ? printerBatteryRaw : -1;
  printer["batteryPercent"] = printerBatteryPercent();
  printer["lastSeenMs"] = batteryFresh ? (millis() - printerBatteryPacketMs) : -1;
  JsonObject victron = doc["victron"].to<JsonObject>();
  victron["mac"] = settings.victronMac;
  victron["bindKeyLoaded"] = bindKeyLoaded;
  victron["scanEnabled"] = victronBleStarted;
  victron["telemetryValid"] = telemetry.valid;
  victron["lastSeenMs"] = telemetry.valid ? (millis() - telemetry.lastPacketMillis) : -1;
  JsonObject imageGen = doc["imageGen"].to<JsonObject>();
  imageGen["provider"] = effectiveImageProvider();
  imageGen["model"] = effectiveImageModel();
  imageGen["url"] = effectiveImageUrl();
  JsonObject s = doc["settings"].to<JsonObject>();
  s["wifiSsid"] = settings.wifiSsid;
  s["hostname"] = settings.hostname;
  s["victronMac"] = settings.victronMac;
  s["victronBindKey"] = settings.victronBindKey;
  s["printerMac"] = settings.printerMac;
  s["imageGenProvider"] = effectiveImageProvider();
  s["imageGenModel"] = effectiveImageModel();
  s["imageGenUrl"] = settings.imageGenUrl;
  s["defaultLabelSize"] = settings.defaultLabelSize;
  s["defaultOrientation"] = settings.defaultOrientation;
  s["defaultAppearance"] = settings.defaultAppearance;
  s["customLabelSizes"] = settings.customLabelSizes;
  sendJson(doc);
}

void handleWifiScan() {
  startSetupAp();
  WiFi.mode(setupApActive ? WIFI_AP_STA : WIFI_STA);
  const int count = WiFi.scanNetworks(false, true, false, 300, 0);
  JsonDocument doc;
  JsonArray networks = doc["networks"].to<JsonArray>();
  for (int i = 0; i < count; ++i) {
    JsonObject item = networks.add<JsonObject>();
    item["ssid"] = WiFi.SSID(i);
    item["rssi"] = WiFi.RSSI(i);
    item["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
  }
  doc["message"] = count >= 0 ? String("Found ") + count + " Wi-Fi networks." : "Wi-Fi scan failed.";
  WiFi.scanDelete();
  sendJson(doc, count >= 0 ? 200 : 500);
}

void handleWifiConnectTest() {
  JsonDocument req;
  if (!parseJsonBody(req)) {
    server.send(400, "application/json", "{\"message\":\"invalid json\"}");
    return;
  }

  const String ssid = req["wifiSsid"] | "";
  const String password = req["wifiPassword"] | "";
  if (!ssid.length()) {
    server.send(400, "application/json", "{\"message\":\"Wi-Fi SSID is required.\"}");
    return;
  }

  startSetupAp();
  provisioningMode = false;
  staConnectRequested = true;
  WiFi.mode(setupApActive ? WIFI_AP_STA : WIFI_STA);
  WiFi.setHostname(settings.hostname.c_str());
  WiFi.disconnect(false, true);
  delay(150);
  WiFi.begin(ssid.c_str(), password.c_str());

  const uint32_t started = millis();
  while (millis() - started < 12000 && WiFi.status() != WL_CONNECTED) {
    delay(200);
  }

  JsonDocument doc;
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnectFailures = 0;
    staJoinActive = false;
    settings.wifiSsid = ssid;
    settings.wifiPassword = password;
    settingsSave(settings);
    doc["message"] = String("Connected to ") + ssid + " at " + WiFi.localIP().toString();
    doc["ok"] = true;
  } else {
    provisioningMode = true;
    ++wifiConnectFailures;
    staJoinActive = false;
    doc["message"] = String("Could not connect to ") + ssid + ". Setup AP remains available at " + setupPortalUrl();
    doc["ok"] = false;
  }
  sendJson(doc, WiFi.status() == WL_CONNECTED ? 200 : 502);
}

void redirectToAdminPortal() {
  server.sendHeader("Location", setupPortalUrl(), true);
  server.send(302, "text/plain", "Redirecting to setup portal");
}

void handleNoContent() {
  server.send(200, "text/plain", "ok");
}

void handleCaptiveProbe() {
  redirectToAdminPortal();
}

void handleFirmwareUpdateFinished() {
  JsonDocument doc;
  doc["ok"] = firmwareUpdateOk;
  doc["message"] = firmwareUpdateMessage.length() ? firmwareUpdateMessage : (firmwareUpdateOk ? "Firmware uploaded successfully. Device will reboot in a few seconds." : "Firmware update failed.");
  sendJson(doc, firmwareUpdateOk ? 200 : 500);

  if (firmwareUpdateOk) {
    restartPending = true;
    restartAtMs = millis() + 5000;
  }
}

void handleFirmwareUpload() {
  HTTPUpload &upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    firmwareUploadInProgress = true;
    firmwareUpdateOk = false;
    firmwareUpdateMessage = "";
    firmwareUploadBytes = 0;
    firmwareUploadLastLogged = 0;
    logf("Firmware upload start: %s", upload.filename.c_str());
    if (printSession.active) {
      logLine("Stopping active printer session for OTA upload.");
      cleanupPrintSession();
    }
    if (victronScan && victronScan->isScanning()) {
      logLine("Stopping BLE scan for OTA upload.");
      victronScan->stop();
      delay(50);
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      firmwareUpdateMessage = String("Update begin failed: ") + Update.errorString();
      logLine(firmwareUpdateMessage);
      firmwareUploadInProgress = false;
      return;
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    firmwareUploadBytes += upload.currentSize;
    if (firmwareUploadBytes - firmwareUploadLastLogged >= 65536) {
      firmwareUploadLastLogged = firmwareUploadBytes;
      logf("Firmware upload progress: %u bytes", static_cast<unsigned>(firmwareUploadBytes));
    }
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      firmwareUpdateMessage = String("Update write failed: ") + Update.errorString();
      logLine(firmwareUpdateMessage);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      firmwareUpdateOk = true;
      firmwareUpdateMessage = String("Firmware uploaded: ") + upload.totalSize + " bytes. Rebooting.";
      logLine(firmwareUpdateMessage);
    } else {
      firmwareUpdateMessage = String("Update finalize failed: ") + Update.errorString();
      logLine(firmwareUpdateMessage);
    }
    firmwareUploadInProgress = false;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    firmwareUpdateMessage = "Firmware upload aborted.";
    logLine(firmwareUpdateMessage);
    firmwareUploadInProgress = false;
  }
}

void handleRemoteLogs() {
  JsonDocument doc;
  const long requestedSince = server.hasArg("since") ? server.arg("since").toInt() : 0L;
  const uint32_t since = requestedSince > 0 ? static_cast<uint32_t>(requestedSince) : 0;
  JsonArray lines = doc["lines"].to<JsonArray>();
  uint32_t nextId = nextRemoteLogId;
  for (const auto &entry : remoteLogs) {
    if (entry.id <= since) {
      continue;
    }
    JsonObject item = lines.add<JsonObject>();
    item["id"] = entry.id;
    item["line"] = entry.line;
    nextId = entry.id + 1;
  }
  doc["nextId"] = nextId;
  doc["total"] = remoteLogs.size();
  sendJson(doc);
}

void handleLive() {
  JsonDocument doc;
  if (!BLE_RUNTIME_ENABLED) {
    doc["batteryHealth"] = "ble-disabled";
    doc["chargeStage"] = "BLE disabled";
    doc["message"] = "Victron BLE is disabled in this build because BLE controller startup aborts on this board/core.";
    doc["timezone"] = "America/Los_Angeles";
    sendJson(doc, 503);
    return;
  }
  if (!settings.victronMac.length()) {
    doc["batteryHealth"] = "unconfigured";
    doc["chargeStage"] = "Victron MAC missing";
    doc["message"] = "No Victron MAC is saved yet. Use Admin to scan and select the SmartSolar address.";
    doc["timezone"] = "America/Los_Angeles";
    doc["bindKeyLoaded"] = bindKeyLoaded;
    doc["victronMac"] = settings.victronMac;
    sendJson(doc, 200);
    return;
  }
  if (!bindKeyLoaded) {
    doc["batteryHealth"] = "unconfigured";
    doc["chargeStage"] = "Bind key invalid";
    doc["message"] = "Victron bind key is missing or invalid. It must be exactly 32 hex characters from Victron Instant Readout.";
    doc["timezone"] = "America/Los_Angeles";
    doc["bindKeyLoaded"] = false;
    doc["victronMac"] = settings.victronMac;
    sendJson(doc, 200);
    return;
  }
  if (!victronBleStarted && WiFi.status() == WL_CONNECTED && settings.victronMac.length() && bindKeyLoaded) {
    if (!ensureVictronBleStarted()) {
      doc["batteryHealth"] = "ble-unavailable";
      doc["chargeStage"] = "BLE unavailable";
      doc["message"] = "Victron BLE could not be started.";
      doc["timezone"] = "America/Los_Angeles";
      sendJson(doc, 503);
      return;
    }
  }
  doc["victronMac"] = settings.victronMac;
  doc["bindKeyLoaded"] = bindKeyLoaded;
  doc["scanEnabled"] = victronBleStarted;
  doc["batteryHealth"] = batteryHealthLabel();
  doc["deviceState"] = telemetry.deviceState;
  doc["chargeStage"] = chargerStateLabel(telemetry.deviceState);
  doc["errorCode"] = telemetry.errorCode;
  doc["batteryVoltage"] = telemetry.batteryVoltage;
  doc["batteryCurrent"] = telemetry.batteryCurrent;
  doc["pvPowerWatts"] = telemetry.pvPowerWatts;
  doc["loadCurrent"] = telemetry.loadCurrent;
  doc["lastPacketEpoch"] = telemetry.hasTime ? telemetry.lastPacketEpoch : 0;
  doc["yieldTodayKwh"] = telemetry.yieldTodayKwh;
  doc["rssi"] = telemetry.rssi;
  doc["packetCounter"] = telemetry.packetCounter;
  doc["lastPacketIso"] = telemetry.hasTime ? isoTime(telemetry.lastPacketEpoch) : "";
  doc["lastPacketLocal"] = telemetry.hasTime ? localTimeString(telemetry.lastPacketEpoch) : "";
  doc["timezone"] = "America/Los_Angeles";
  doc["batteryNominalWh"] = 156;
  doc["batteryNominalVolts"] = 12;
  doc["batteryGaugeMinVolts"] = 12.0f;
  doc["batteryGaugeMaxVolts"] = 14.6f;
  doc["batteryReserveVolts"] = 12.8f;
  doc["batteryCriticalVolts"] = 12.2f;
  doc["pvArrayVolts"] = 48;
  doc["pvArrayWatts"] = 225;
  doc["controllerChargeLimitAmps"] = 15;
  doc["loadDesignLimitAmps"] = 8;
  doc["batteryCurrentGaugeMin"] = -8;
  doc["batteryCurrentGaugeMax"] = 15;
  doc["dataAgeMs"] = telemetry.valid ? millis() - telemetry.lastPacketMillis : UINT32_MAX;
  if (!telemetry.valid) {
    doc["message"] = "Victron scan is active, but no valid Instant Readout packets have been decrypted yet. Recheck the MAC and 32-character bind key.";
  }
  sendJson(doc);
}

void handleHistory() {
  JsonDocument doc;
  doc["message"] = "History storage is disabled. Power page is live-feed only.";
  doc["samples"].to<JsonArray>();
  sendJson(doc);
}

void handleLabel(bool wild) {
  JsonDocument req;
  if (!parseJsonBody(req)) {
    server.send(400, "application/json", "{\"error\":\"invalid json\"}");
    return;
  }
  LabelSpec spec;
  spec.text = req["text"] | "";
  spec.shelfHint = req["shelfHint"] | "";
  spec.sizeKey = req["sizeKey"] | settings.defaultLabelSize;
  spec.orientation = req["orientation"] | settings.defaultOrientation;
  spec.appearance = req["appearance"] | settings.defaultAppearance;
  spec.wild = wild;
  spec.creativeMix = req["creativeMix"] | false;
  spec.variantSeed = req["variantSeed"] | 0U;
  LabelRender render = renderLabel(spec);
  JsonDocument doc;
  doc["mode"] = render.mode;
  doc["normalized"] = render.normalized;
  doc["svg"] = render.svg;
  doc["explanation"] = render.explanation;
  doc["widthPx"] = render.widthPx;
  doc["heightPx"] = render.heightPx;
  sendJson(doc);
}

void handleCrazyLabel() {
  JsonDocument req;
  if (!parseJsonBody(req)) {
    server.send(400, "application/json", "{\"error\":\"invalid json\"}");
    return;
  }

  LabelSpec spec;
  spec.text = req["text"] | "";
  spec.shelfHint = req["shelfHint"] | "";
  spec.sizeKey = req["sizeKey"] | settings.defaultLabelSize;
  spec.orientation = req["orientation"] | settings.defaultOrientation;
  spec.appearance = req["appearance"] | settings.defaultAppearance;

  LabelRender fallback = renderLabel(spec);
  const String normalized = fallback.normalized;
  const String prompt = buildCrazyPrompt(spec, fallback, normalized);

  const String imageProvider = effectiveImageProvider();
  const String imageModel = effectiveImageModel();
  const String imageUrl = effectiveImageUrl();
  if (!imageUrl.length()) {
    JsonDocument doc;
    doc["mode"] = "ai-image";
    doc["normalized"] = normalized;
    doc["prompt"] = prompt;
      doc["widthPx"] = fallback.widthPx;
      doc["heightPx"] = fallback.heightPx;
      doc["message"] = "No AI image generator is configured yet. Add one on the admin page.";
      sendJson(doc, 501);
      return;
  }
  if (!settings.imageGenToken.length()) {
    JsonDocument doc;
    doc["mode"] = "ai-image";
    doc["normalized"] = normalized;
    doc["prompt"] = prompt;
    doc["widthPx"] = fallback.widthPx;
    doc["heightPx"] = fallback.heightPx;
    doc["message"] = imageProvider == "openai"
        ? "OpenAI direct is selected, but no API key is saved in Admin."
        : "No API token is saved for the configured image generator.";
    sendJson(doc, 401);
    return;
  }

  String responseBody;
  int statusCode = -1;
  if (imageUrl.startsWith("https://")) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    if (http.begin(client, imageUrl)) {
      http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
      http.setTimeout(HTTP_TIMEOUT_MS);
      http.addHeader("Content-Type", "application/json");
      if (settings.imageGenToken.length()) {
        http.addHeader("Authorization", String("Bearer ") + settings.imageGenToken);
      }
      if (imageProvider == "huggingface") {
        http.addHeader("Accept", "image/png");
      }
      JsonDocument payload;
      if (imageProvider == "openai") {
        payload["model"] = imageModel;
        payload["prompt"] = prompt;
        payload["size"] = openAiImageSizeForLabel(fallback.widthPx, fallback.heightPx);
        payload["quality"] = "low";
        payload["output_format"] = "png";
        payload["background"] = "opaque";
      } else if (imageProvider == "huggingface") {
        payload["inputs"] = prompt;
        JsonObject parameters = payload["parameters"].to<JsonObject>();
        parameters["negative_prompt"] = "low contrast, photo, watercolor wash, gray background, tiny text, clutter, realistic scene, soft gradients";
        parameters["width"] = fallback.widthPx;
        parameters["height"] = fallback.heightPx;
        parameters["num_inference_steps"] = 8;
        parameters["guidance_scale"] = 4.0;
      } else {
        payload["prompt"] = prompt;
        payload["title"] = normalized;
        payload["shelfHint"] = spec.shelfHint;
        payload["width"] = fallback.widthPx;
        payload["height"] = fallback.heightPx;
        payload["appearance"] = spec.appearance;
        payload["orientation"] = spec.orientation;
        payload["mode"] = "storage-label";
        payload["style"] = "creative-unique-monochrome";
        payload["fit"] = "contain";
        payload["themePolicy"] = looksGenericStorageLabel(normalized) ? "surprise-me" : "subject-led";
        payload["negativePrompt"] = "low contrast, photo, watercolor wash, gray background, tiny text, clutter, realistic scene, soft gradients";
      }
      String body;
      serializeJson(payload, body);
      statusCode = http.POST(body);
      if (statusCode >= 200 && statusCode < 300 && imageProvider == "huggingface") {
        streamHttpImageResponse(http, normalized, fallback.widthPx, fallback.heightPx);
        http.end();
        return;
      } else {
        std::vector<uint8_t> ignored;
        readHttpResponseBody(http, responseBody, ignored, false);
      }
      http.end();
    }
  } else {
    HTTPClient http;
    if (http.begin(imageUrl)) {
      http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
      http.setTimeout(HTTP_TIMEOUT_MS);
      http.addHeader("Content-Type", "application/json");
      if (settings.imageGenToken.length()) {
        http.addHeader("Authorization", String("Bearer ") + settings.imageGenToken);
      }
      if (imageProvider == "huggingface") {
        http.addHeader("Accept", "image/png");
      }
      JsonDocument payload;
      if (imageProvider == "openai") {
        payload["model"] = imageModel;
        payload["prompt"] = prompt;
        payload["size"] = openAiImageSizeForLabel(fallback.widthPx, fallback.heightPx);
        payload["quality"] = "low";
        payload["output_format"] = "png";
        payload["background"] = "opaque";
      } else if (imageProvider == "huggingface") {
        payload["inputs"] = prompt;
        JsonObject parameters = payload["parameters"].to<JsonObject>();
        parameters["negative_prompt"] = "low contrast, photo, watercolor wash, gray background, tiny text, clutter, realistic scene, soft gradients";
        parameters["width"] = fallback.widthPx;
        parameters["height"] = fallback.heightPx;
        parameters["num_inference_steps"] = 8;
        parameters["guidance_scale"] = 4.0;
      } else {
        payload["prompt"] = prompt;
        payload["title"] = normalized;
        payload["shelfHint"] = spec.shelfHint;
        payload["width"] = fallback.widthPx;
        payload["height"] = fallback.heightPx;
        payload["appearance"] = spec.appearance;
        payload["orientation"] = spec.orientation;
        payload["mode"] = "storage-label";
        payload["style"] = "creative-unique-monochrome";
        payload["fit"] = "contain";
        payload["themePolicy"] = looksGenericStorageLabel(normalized) ? "surprise-me" : "subject-led";
        payload["negativePrompt"] = "low contrast, photo, watercolor wash, gray background, tiny text, clutter, realistic scene, soft gradients";
      }
      String body;
      serializeJson(payload, body);
      statusCode = http.POST(body);
      if (statusCode >= 200 && statusCode < 300 && imageProvider == "huggingface") {
        streamHttpImageResponse(http, normalized, fallback.widthPx, fallback.heightPx);
        http.end();
        return;
      } else {
        std::vector<uint8_t> ignored;
        readHttpResponseBody(http, responseBody, ignored, false);
      }
      http.end();
    }
  }

  if (statusCode < 200 || statusCode >= 300) {
    String upstreamMessage;
    JsonDocument upstreamError;
    if (deserializeJson(upstreamError, responseBody) == DeserializationError::Ok) {
      upstreamMessage = upstreamError["error"]["message"] | upstreamError["message"] | "";
      upstreamMessage = sanitizeUpstreamErrorMessage(upstreamMessage);
    }
    JsonDocument doc;
    doc["mode"] = "ai-image";
    doc["normalized"] = normalized;
    doc["prompt"] = prompt;
    doc["widthPx"] = fallback.widthPx;
    doc["heightPx"] = fallback.heightPx;
    doc["message"] = upstreamMessage.length()
        ? String("AI image generator request failed with status ") + statusCode + ": " + upstreamMessage
        : String("AI image generator request failed with status ") + statusCode;
    sendJson(doc, 502);
    return;
  }

  JsonDocument upstream;
  if (deserializeJson(upstream, responseBody) != DeserializationError::Ok) {
    JsonDocument doc;
    doc["mode"] = "ai-image";
    doc["normalized"] = normalized;
    doc["prompt"] = prompt;
    doc["widthPx"] = fallback.widthPx;
    doc["heightPx"] = fallback.heightPx;
    doc["message"] = "AI image generator response was not valid JSON.";
    sendJson(doc, 502);
    return;
  }

  String pngBase64;
  if (imageProvider == "openai") {
    JsonArray data = upstream["data"].as<JsonArray>();
    if (!data.isNull() && !data[0].isNull()) {
      pngBase64 = data[0]["b64_json"] | "";
    }
  }
  if (!pngBase64.length()) {
    pngBase64 = upstream["pngBase64"] | "";
  }
  if (!pngBase64.length()) {
    pngBase64 = upstream["image_base64"] | "";
  }
  if (!pngBase64.length()) {
    pngBase64 = upstream["b64_json"] | "";
  }
  if (!pngBase64.length()) {
    JsonDocument doc;
    doc["mode"] = "ai-image";
    doc["normalized"] = normalized;
    doc["prompt"] = prompt;
    doc["widthPx"] = fallback.widthPx;
    doc["heightPx"] = fallback.heightPx;
    doc["message"] = "AI image generator did not return a PNG base64 payload.";
    sendJson(doc, 502);
    return;
  }

  JsonDocument doc;
  doc["mode"] = "ai-image";
  doc["normalized"] = normalized;
  doc["prompt"] = prompt;
  doc["pngBase64"] = pngBase64;
  doc["widthPx"] = fallback.widthPx;
  doc["heightPx"] = fallback.heightPx;
  doc["message"] = "AI image label generated.";
  sendJson(doc);
}

void handleSaveSettings() {
  JsonDocument req;
  if (!parseJsonBody(req)) {
    server.send(400, "application/json", "{\"message\":\"invalid json\"}");
    return;
  }
  const String oldWifiSsid = settings.wifiSsid;
  const String oldWifiPassword = settings.wifiPassword;
  const String oldHostname = settings.hostname;
  settings.wifiSsid = req["wifiSsid"] | settings.wifiSsid;
  const String requestedWifiPassword = req["wifiPassword"] | "";
  if (requestedWifiPassword.length()) {
    settings.wifiPassword = requestedWifiPassword;
  }
  settings.hostname = req["hostname"] | settings.hostname;
  settings.victronMac = req["victronMac"] | settings.victronMac;
  settings.victronBindKey = req["victronBindKey"] | settings.victronBindKey;
  settings.printerMac = req["printerMac"] | settings.printerMac;
  settings.imageGenProvider = req["imageGenProvider"] | settings.imageGenProvider;
  settings.imageGenModel = req["imageGenModel"] | settings.imageGenModel;
  settings.imageGenProvider.trim();
  settings.imageGenModel.trim();
  settings.imageGenUrl = req["imageGenUrl"] | settings.imageGenUrl;
  String requestedImageGenToken = req["imageGenToken"] | "";
  requestedImageGenToken.trim();
  if (requestedImageGenToken.length()) {
    settings.imageGenToken = requestedImageGenToken;
  }
  settings.defaultLabelSize = req["defaultLabelSize"] | settings.defaultLabelSize;
  settings.defaultOrientation = req["defaultOrientation"] | settings.defaultOrientation;
  settings.defaultAppearance = req["defaultAppearance"] | settings.defaultAppearance;
  settings.customLabelSizes = req["customLabelSizes"] | settings.customLabelSizes;
  settings.customLabelSizes.trim();
  const bool reboot = req["reboot"] | false;
  const bool wifiConfigChanged =
      settings.wifiSsid != oldWifiSsid
      || settings.wifiPassword != oldWifiPassword
      || settings.hostname != oldHostname;
  provisioningMode = false;
  staConnectRequested = true;
  const bool saveOk = settingsSave(settings);
  loadSettingsIntoRuntime();
  JsonDocument doc;
  if (!saveOk) {
    doc["message"] = "Settings could not be written to non-volatile storage. Provisioning remains active, but settings will not persist across reboot.";
  } else {
    doc["message"] = reboot
        ? "Settings saved. Rebooting now."
        : wifiConfigChanged
            ? String("Settings saved. Wi-Fi settings changed, so Mervyns will reconnect. Setup portal remains at ") + setupPortalUrl() + "."
            : "Settings saved.";
  }
  sendJson(doc);
  if (wifiConfigChanged && !reboot) {
    delay(200);
    WiFi.disconnect(true, true);
    delay(150);
    hadStationConnection = false;
    staJoinActive = false;
    lastWiFiAttemptMs = millis() - WIFI_RETRY_MS;
    ensureWifi();
  }
  if (reboot) {
    delay(300);
    ESP.restart();
  }
}

void handleLabelPreferencesSave() {
  JsonDocument req;
  if (!parseJsonBody(req)) {
    server.send(400, "application/json", "{\"message\":\"invalid json\"}");
    return;
  }

  String requestedSize = req["defaultLabelSize"] | settings.defaultLabelSize;
  requestedSize.trim();
  if (!configuredLabelSizeExists(requestedSize)) {
    requestedSize = settings.defaultLabelSize;
  }

  String requestedOrientation = req["defaultOrientation"] | settings.defaultOrientation;
  requestedOrientation.trim();
  if (!requestedOrientation.equalsIgnoreCase("portrait") && !requestedOrientation.equalsIgnoreCase("landscape")) {
    requestedOrientation = settings.defaultOrientation;
  }

  String requestedAppearance = req["defaultAppearance"] | settings.defaultAppearance;
  requestedAppearance.trim();
  if (!requestedAppearance.equalsIgnoreCase("light") && !requestedAppearance.equalsIgnoreCase("dark")) {
    requestedAppearance = settings.defaultAppearance;
  }

  settings.defaultLabelSize = requestedSize;
  settings.defaultOrientation = requestedOrientation;
  settings.defaultAppearance = requestedAppearance;

  JsonDocument doc;
  if (!settingsSave(settings)) {
    doc["message"] = "Could not save label defaults.";
    sendJson(doc, 500);
    return;
  }

  loadSettingsIntoRuntime();
  doc["message"] = "Label defaults saved.";
  doc["defaultLabelSize"] = settings.defaultLabelSize;
  doc["defaultOrientation"] = settings.defaultOrientation;
  doc["defaultAppearance"] = settings.defaultAppearance;
  sendJson(doc, 200);
}

void emitDiscoveredDevices(const std::vector<DiscoveredDevice> &devices, const String &message) {
  JsonDocument doc;
  doc["message"] = message;
  JsonArray arr = doc["devices"].to<JsonArray>();
  for (const auto &d : devices) {
    JsonObject item = arr.add<JsonObject>();
    item["name"] = d.name;
    item["address"] = d.address;
    item["rssi"] = d.rssi;
    JsonArray channels = item["channels"].to<JsonArray>();
    for (int ch : d.channels) channels.add(ch);
  }
  sendJson(doc);
}

void handleScanVictron() {
  lastVictronScan = scanVictronDevices();
  emitDiscoveredDevices(lastVictronScan, String("Found ") + lastVictronScan.size() + " Victron BLE candidates.");
}

void handleScanPrinter() {
  lastPrinterScan = scanPrinterDevices();
  emitDiscoveredDevices(lastPrinterScan, String("Found ") + lastPrinterScan.size() + " BLE printer candidates.");
}

void handlePrinterStatus() {
  JsonDocument doc;
  String message;
  const bool ok = withConnectedPrinter(
      [] (NimBLEClient *client, NimBLERemoteService *service, NimBLERemoteCharacteristic *writeChar, NimBLERemoteCharacteristic *flowChar, String &opMessage) -> bool {
        (void)client;
        (void)service;
        (void)writeChar;
        (void)flowChar;
        opMessage = String("Printer target saved as ") + settings.printerMac + ". Reachable.";
        return true;
      },
      message);

  doc["message"] = message;
  doc["reachable"] = ok;
  doc["available"] = ok;
  const bool batteryFresh = printerBatteryPercent() >= 0;
  doc["batteryKnown"] = batteryFresh;
  doc["batteryRaw"] = batteryFresh ? printerBatteryRaw : -1;
  doc["batteryPercent"] = printerBatteryPercent();
  doc["lastSeenMs"] = batteryFresh ? (millis() - printerBatteryPacketMs) : -1;
  sendJson(doc, ok ? 200 : 502);
}

void handleBridgeInfo() {
  JsonDocument doc;
  const bool batteryFresh = printerBatteryPercent() >= 0;
  const LabelPreset nativePreset = labelPresetNativeByKey(settings.defaultLabelSize);
  const LabelPreset requestedPreset = labelPresetByKey(settings.defaultLabelSize, settings.defaultOrientation);
  doc["preferredHost"] = preferredHost();
  doc["hostname"] = settings.hostname;
  doc["printerMac"] = settings.printerMac;
  doc["defaultLabelSize"] = settings.defaultLabelSize;
  doc["defaultOrientation"] = settings.defaultOrientation;
  doc["defaultAppearance"] = settings.defaultAppearance;
  doc["shape"] = labelShapeForKey(settings.defaultLabelSize);
  doc["stockWidthPx"] = nativePreset.widthPx;
  doc["stockHeightPx"] = nativePreset.heightPx;
  doc["jobWidthPx"] = requestedPreset.widthPx;
  doc["jobHeightPx"] = requestedPreset.heightPx;
  doc["targetPrintWidthPx"] = 384;
  doc["fitMode"] = isCircleLabelKey(settings.defaultLabelSize) ? "circle-inscribed" : "rect-contain";
  doc["mask"] = isCircleLabelKey(settings.defaultLabelSize) ? "circle" : "none";
  doc["largestCircleDiameterPx"] = std::min(nativePreset.widthPx, nativePreset.heightPx);
  doc["chunkBytes"] = 1536;
  doc["printerConfigured"] = settings.printerMac.length() > 0;
  doc["batteryKnown"] = batteryFresh;
  doc["batteryPercent"] = printerBatteryPercent();
  doc["lastSeenMs"] = batteryFresh ? (millis() - printerBatteryPacketMs) : -1;
  JsonArray sizes = doc["labelSizes"].to<JsonArray>();
  for (const auto &key : configuredLabelSizeKeys()) {
    sizes.add(key);
  }
  sendJson(doc);
}

void handleBridgeJobCancel() {
  JsonDocument doc;
  if (printSession.active) {
    cleanupPrintSession();
    doc["message"] = "Bridge print session canceled.";
  } else {
    doc["message"] = "No active bridge print session.";
  }
  sendJson(doc);
}

void handlePrinterTestPrint() {
  JsonDocument doc;
  if (printSession.active) {
    doc["message"] = "A label upload is already active.";
    sendJson(doc, 409);
    return;
  }
  const int widthBytes = 48;
  const int height = 96;
  String message;
  if (!connectSerialPrinterSession(message)) {
    doc["message"] = message;
    sendJson(doc, 502);
    return;
  }
  const std::vector<uint8_t> raster = makeTextRaster(widthBytes, height, "TEST");
  if (!runDirectRasterPrint(raster, widthBytes, height, "admin-test", message)) {
    settleAndDisconnectSerialPrinterSession(0);
    doc["message"] = message;
    sendJson(doc, 502);
    return;
  }
  settleAndDisconnectSerialPrinterSession();
  doc["message"] = "Printer TEST pattern sent.";
  doc["widthPx"] = 384;
  doc["heightPx"] = height;
  doc["batteryKnown"] = printerBatteryPercent() >= 0;
  doc["batteryPercent"] = printerBatteryPercent();
  sendJson(doc, 200);
}

void printSerialM100Help() {
  logLine("m100 commands:");
  logLine("  m100 help");
  logLine("  m100 status");
  logLine("  m100 battery");
  logLine("  m100 connect");
  logLine("  m100 disconnect");
  logLine("  m100 calibrate");
  logLine("  m100 rows <8-240>");
  logLine("  m100 pad <0-240>");
  logLine("  m100 text <message>");
  logLine("  m100 checker");
  logLine("  m100 bar");
  logLine("  m100 box");
  logLine("  m100 ink");
  logLine("  m100 solidff");
  logLine("  m100 solid00");
}

void handleSerialM100Command(const String &line) {
  const std::vector<String> tokens = splitTokens(line);
  if (tokens.empty() || !tokens[0].equalsIgnoreCase("m100")) {
    return;
  }
  if (tokens.size() == 1 || tokens[1].equalsIgnoreCase("help")) {
    printSerialM100Help();
    return;
  }

  const String command = tokens[1];
  if (command.equalsIgnoreCase("status")) {
    const int batteryPercent = printerBatteryPercent();
    const int batteryRaw = batteryPercent >= 0 ? printerBatteryRaw : -1;
    const uint32_t batteryAgeMs = batteryPercent >= 0 ? (millis() - printerBatteryPacketMs) : 0;
    logf("m100 status: upload=%s connected=%s rows=%u pad=%u battery=%d%% raw=%d age=%lus primed=%s",
         printSession.active ? "yes" : "no",
         (serialPrinter.client && serialPrinter.client->isConnected()) ? "yes" : "no",
         static_cast<unsigned>(serialTestRows),
         static_cast<unsigned>(serialTestPadRows),
         batteryPercent,
         batteryRaw,
         static_cast<unsigned long>(batteryAgeMs / 1000UL),
         printerPrimed ? "yes" : "no");
    return;
  }

  if (command.equalsIgnoreCase("battery")) {
    if (!serialPrinter.client || !serialPrinter.client->isConnected() || !serialPrinter.writeChar) {
      logLine("Serial printer session is not connected. Run 'm100 connect' first.");
      return;
    }
    String message;
    if (!requestPrinterBatteryPacket(serialPrinter.writeChar, message)) {
      logLine(message);
      return;
    }
    logLine(message);
    return;
  }

  if (command.equalsIgnoreCase("connect")) {
    String message;
    if (!connectSerialPrinterSession(message)) {
      logLine(message);
      return;
    }
    logLine(message);
    return;
  }

  if (command.equalsIgnoreCase("disconnect")) {
    disconnectSerialPrinterSession();
    logLine("Serial printer session disconnected.");
    return;
  }

  if (command.equalsIgnoreCase("calibrate")) {
    if (!serialPrinter.client || !serialPrinter.client->isConnected() || !serialPrinter.writeChar) {
      logLine("Serial printer session is not connected. Run 'm100 connect' first.");
      return;
    }
    String message;
    if (!runM100CalibrationOnCharacteristic(serialPrinter.writeChar, message)) {
      logLine(message);
      return;
    }
    printerPrimed = true;
    logLine(message);
    return;
  }

  if (command.equalsIgnoreCase("rows")) {
    if (tokens.size() < 3) {
      logLine("Usage: m100 rows <8-240>");
      return;
    }
    const uint32_t parsed = static_cast<uint32_t>(tokens[2].toInt());
    serialTestRows = static_cast<uint16_t>(std::min<uint32_t>(240, std::max<uint32_t>(8, parsed)));
    logf("m100 rows set to %u", static_cast<unsigned>(serialTestRows));
    return;
  }

  if (command.equalsIgnoreCase("pad")) {
    if (tokens.size() < 3) {
      logLine("Usage: m100 pad <0-240>");
      return;
    }
    const uint32_t parsed = static_cast<uint32_t>(tokens[2].toInt());
    serialTestPadRows = static_cast<uint16_t>(std::min<uint32_t>(240, parsed));
    logf("m100 pad set to %u", static_cast<unsigned>(serialTestPadRows));
    return;
  }

  constexpr int kWidthBytes = 48;
  std::vector<uint8_t> content;
  String label;
  int contentRows = std::max<int>(8, serialTestRows);

  if (command.equalsIgnoreCase("text")) {
    const String message = joinTokens(tokens, 2);
    label = String("text-") + (message.length() ? message : "TEST");
    contentRows = std::max<int>(24, serialTestRows);
    content = makeTextRaster(kWidthBytes, static_cast<size_t>(contentRows), message);
  } else if (command.equalsIgnoreCase("checker")) {
    label = "checker";
    content = makeCheckerRaster(kWidthBytes, static_cast<size_t>(contentRows));
  } else if (command.equalsIgnoreCase("bar")) {
    label = "bar";
    content = makeCenterBarRaster(kWidthBytes, static_cast<size_t>(contentRows));
  } else if (command.equalsIgnoreCase("box")) {
    label = "box";
    content = makeBoxRaster(kWidthBytes, static_cast<size_t>(contentRows));
  } else if (command.equalsIgnoreCase("ink")) {
    label = "ink";
    content = makeM100LabInkRaster(kWidthBytes, static_cast<size_t>(contentRows));
  } else if (command.equalsIgnoreCase("solidff")) {
    label = "solidff";
    content = makeSolidRaster(kWidthBytes, static_cast<size_t>(contentRows), 0xFF);
  } else if (command.equalsIgnoreCase("solid00")) {
    label = "solid00";
    content = makeSolidRaster(kWidthBytes, static_cast<size_t>(contentRows), 0x00);
  } else {
    logLine("Unknown m100 command. Use 'm100 help'.");
    return;
  }

  const std::vector<uint8_t> raster = prependBlankRows(content, kWidthBytes, serialTestPadRows);
  const int height = static_cast<int>(raster.size() / kWidthBytes);
  String message;
  if (!runDirectRasterPrint(raster, kWidthBytes, height, label, message)) {
    logLine(message);
    return;
  }
  logLine(message);
}

void processSerialConsole() {
  while (Serial.available() > 0) {
    const char ch = static_cast<char>(Serial.read());
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      String line = serialCommandBuffer;
      serialCommandBuffer = "";
      line.trim();
      if (line.length()) {
        handleSerialM100Command(line);
      }
      continue;
    }
    serialCommandBuffer += ch;
    if (serialCommandBuffer.length() > 240) {
      serialCommandBuffer.remove(0, serialCommandBuffer.length() - 240);
    }
  }
}

void handlePrintStart() {
  JsonDocument doc;
  JsonDocument req;
  if (!parseJsonBody(req)) {
    doc["message"] = "Invalid JSON.";
    sendJson(doc, 400);
    return;
  }

  const int widthBytes = req["widthBytes"] | 0;
  const int height = req["height"] | 0;
  const size_t totalBytes = static_cast<size_t>(req["totalBytes"] | 0);
  if (widthBytes <= 0 || height <= 0 || totalBytes == 0) {
    doc["message"] = "Print start requires widthBytes, height, and totalBytes.";
    sendJson(doc, 400);
    return;
  }
  const size_t expectedRasterLen = static_cast<size_t>(widthBytes) * static_cast<size_t>(height);
  if (expectedRasterLen != totalBytes || expectedRasterLen > 65535) {
    doc["message"] = "Print start dimensions do not match totalBytes or are too large.";
    sendJson(doc, 400);
    return;
  }

  String message;
  if (!startPrintSession(widthBytes, height, expectedRasterLen, message)) {
    doc["message"] = message;
    sendJson(doc, 502);
    return;
  }

  doc["message"] = "Print session started.";
  doc["widthBytes"] = widthBytes;
  doc["height"] = height;
  doc["totalBytes"] = totalBytes;
  sendJson(doc);
}

void handlePrintChunk() {
  JsonDocument doc;
  if (!printSession.active) {
    doc["message"] = "No active print session.";
    sendJson(doc, 400);
    return;
  }
  if (!server.hasArg("plain")) {
    doc["message"] = "Print chunk body was missing.";
    sendJson(doc, 400);
    return;
  }

  const String chunkB64 = server.arg("plain");
  if (!chunkB64.length()) {
    doc["message"] = "Print chunk was empty.";
    sendJson(doc, 400);
    return;
  }

  size_t decodedCapacity = ((chunkB64.length() + 3) / 4) * 3;
  std::vector<uint8_t> decoded(decodedCapacity);
  size_t decodedLen = 0;
  if (mbedtls_base64_decode(decoded.data(),
                            decoded.size(),
                            &decodedLen,
                            reinterpret_cast<const unsigned char *>(chunkB64.c_str()),
                            chunkB64.length()) != 0) {
    doc["message"] = "Print chunk could not be decoded.";
    cleanupPrintSession();
    sendJson(doc, 400);
    return;
  }

  String message;
  if (!appendPrintSessionRaster(decoded.data(), decodedLen, message)) {
    doc["message"] = message;
    sendJson(doc, 502);
    return;
  }

  doc["message"] = "Print chunk accepted.";
  doc["bytesReceived"] = printSession.receivedRaw;
  sendJson(doc);
}

void handlePrintFinish() {
  JsonDocument doc;
  String message;
  if (!finishPrintSession(message)) {
    doc["message"] = message;
    sendJson(doc, 502);
    return;
  }
  doc["message"] = message;
  sendJson(doc, 200);
}

void handlePrint() {
  JsonDocument doc;
  if (printSession.active) {
    doc["message"] = "A label upload is already active.";
    sendJson(doc, 409);
    return;
  }
  if (!server.hasArg("plain")) {
    doc["message"] = "Print request body was missing.";
    sendJson(doc, 400);
    return;
  }

  const String body = server.arg("plain");
  int widthBytes = 0;
  int height = 0;
  if (!extractJsonIntField(body, "widthBytes", widthBytes) || !extractJsonIntField(body, "height", height)) {
    doc["message"] = "Print request requires widthBytes and height.";
    sendJson(doc, 400);
    return;
  }
  if (widthBytes <= 0 || height <= 0) {
    doc["message"] = "Print dimensions were invalid.";
    sendJson(doc, 400);
    return;
  }

  const size_t expectedRasterLen = static_cast<size_t>(widthBytes) * static_cast<size_t>(height);
  if (expectedRasterLen == 0 || expectedRasterLen > 65535) {
    doc["message"] = "Print raster dimensions were too large.";
    sendJson(doc, 400);
    return;
  }

  int valueStart = 0;
  int valueEnd = 0;
  if (!locateJsonStringField(body, "rasterBase64", valueStart, valueEnd)
      && !locateJsonStringField(body, "raster", valueStart, valueEnd)) {
    doc["message"] = "Print request requires rasterBase64.";
    sendJson(doc, 400);
    return;
  }

  std::vector<uint8_t> raster(expectedRasterLen, 0x00);
  size_t decodedLen = 0;
  if (!base64DecodeToBuffer(reinterpret_cast<const unsigned char *>(body.c_str() + valueStart),
                            static_cast<size_t>(valueEnd - valueStart),
                            raster.data(),
                            raster.size(),
                            decodedLen)) {
    doc["message"] = "Print raster could not be decoded.";
    sendJson(doc, 400);
    return;
  }
  if (decodedLen != expectedRasterLen) {
    doc["message"] = "Decoded print raster size did not match widthBytes x height.";
    sendJson(doc, 400);
    return;
  }

  logf("Direct web print request: %ux%u (%u bytes)",
       static_cast<unsigned>(widthBytes * 8),
       static_cast<unsigned>(height),
       static_cast<unsigned>(decodedLen));

  String message;
  if (!connectSerialPrinterSession(message)) {
    doc["message"] = message;
    sendJson(doc, 502);
    return;
  }
  if (!runDirectRasterPrint(raster, widthBytes, height, "web-direct", message)) {
    settleAndDisconnectSerialPrinterSession(0);
    doc["message"] = message;
    sendJson(doc, 502);
    return;
  }

  settleAndDisconnectSerialPrinterSession();
  doc["message"] = "Direct label print sent.";
  doc["bytes"] = decodedLen;
  doc["widthPx"] = widthBytes * 8;
  doc["heightPx"] = height;
  sendJson(doc, 200);
}

void startServer() {
  server.on("/", HTTP_GET, []() {
    if (!wifiLooksConfigured()) {
      redirectToAdminPortal();
      return;
    }
    server.send_P(200, "text/html", LABELS_HTML);
  });
  server.on("/status", HTTP_GET, []() { server.send_P(200, "text/html", STATUS_HTML_LIVE); });
  server.on("/admin", HTTP_GET, []() { server.send_P(200, "text/html", ADMIN_HTML); });
  server.on("/generate_204", HTTP_GET, handleCaptiveProbe);
  server.on("/gen_204", HTTP_GET, handleCaptiveProbe);
  server.on("/hotspot-detect.html", HTTP_GET, handleCaptiveProbe);
  server.on("/redirect", HTTP_GET, handleCaptiveProbe);
  server.on("/fwlink", HTTP_GET, handleCaptiveProbe);
  server.on("/connecttest.txt", HTTP_GET, handleCaptiveProbe);
  server.on("/ncsi.txt", HTTP_GET, handleCaptiveProbe);
  server.on("/canonical.html", HTTP_GET, handleCaptiveProbe);
  server.on("/success.txt", HTTP_GET, handleCaptiveProbe);
  server.on("/favicon.ico", HTTP_GET, handleNoContent);
  server.on("/apple-touch-icon.png", HTTP_GET, handleNoContent);
  server.on("/site.webmanifest", HTTP_GET, handleNoContent);
  server.on("/api/meta", HTTP_GET, handleMeta);
  server.on("/api/live", HTTP_GET, handleLive);
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/api/labels/standard", HTTP_POST, []() { handleLabel(false); });
  server.on("/api/labels/art", HTTP_POST, handleCrazyLabel);
  server.on("/api/labels/ai-image", HTTP_POST, handleCrazyLabel);
  server.on("/api/labels/crazy-image", HTTP_POST, handleCrazyLabel);
  server.on("/api/labels/preferences", HTTP_POST, handleLabelPreferencesSave);
  server.on("/api/labels/print", HTTP_POST, handlePrint);
  server.on("/api/labels/print/start", HTTP_POST, handlePrintStart);
  server.on("/api/labels/print/chunk", HTTP_POST, handlePrintChunk);
  server.on("/api/labels/print/finish", HTTP_POST, handlePrintFinish);
  server.on("/api/bridge/info", HTTP_GET, handleBridgeInfo);
  server.on("/api/bridge/printer/status", HTTP_GET, handlePrinterStatus);
  server.on("/api/bridge/job/start", HTTP_POST, handlePrintStart);
  server.on("/api/bridge/job/chunk", HTTP_POST, handlePrintChunk);
  server.on("/api/bridge/job/finish", HTTP_POST, handlePrintFinish);
  server.on("/api/bridge/job/cancel", HTTP_POST, handleBridgeJobCancel);
  server.on("/api/admin/settings", HTTP_POST, handleSaveSettings);
  server.on("/api/admin/wifi/scan", HTTP_POST, handleWifiScan);
  server.on("/api/admin/wifi/connect", HTTP_POST, handleWifiConnectTest);
  server.on("/api/admin/scan/victron", HTTP_POST, handleScanVictron);
  server.on("/api/admin/scan/printer", HTTP_POST, handleScanPrinter);
  server.on("/api/admin/logs", HTTP_GET, handleRemoteLogs);
  server.on("/api/printer/status", HTTP_POST, handlePrinterStatus);
  server.on("/api/admin/printer/test", HTTP_POST, handlePrinterTestPrint);
  server.on("/api/admin/update", HTTP_POST, handleFirmwareUpdateFinished, handleFirmwareUpload);
  server.onNotFound([]() {
    if (setupApActive) {
      redirectToAdminPortal();
      return;
    }
    server.send(404, "text/plain", "Not found");
  });
  server.begin();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  logLine("Mainline serial console ready. Type 'm100 help'.");
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(150);
  settingsLoad(settings);
  loadSettingsIntoRuntime();
  provisioningMode = !wifiLooksConfigured();
  initBleController();
  startSetupAp();
  startServer();
  provisioningMode = !wifiLooksConfigured();
  staConnectRequested = wifiLooksConfigured();
  lastWiFiAttemptMs = millis() - WIFI_RETRY_MS;
  ensureWifi();
  if (!provisioningMode) {
    ensureTime();
  }
}

void loop() {
  processSerialConsole();
  ensureWifi();
  if (!provisioningMode) {
    ensureTime();
  }
  if (firmwareUploadInProgress) {
    processSerialConsole();
    if (captiveDnsActive) {
      dnsServer.processNextRequest();
    }
    server.handleClient();
    delay(2);
    return;
  }
  if (printSession.active && millis() - printSession.startedAtMs > 30000) {
    logLine("Print session timed out; clearing uploaded raster.");
    cleanupPrintSession();
  }
  if (!printSession.active && printerDisconnectAtMs && static_cast<int32_t>(millis() - printerDisconnectAtMs) >= 0) {
    logLine("Disconnecting idle printer session.");
    disconnectSerialPrinterSession();
  }
  if (captiveDnsActive) {
    dnsServer.processNextRequest();
  }
  server.handleClient();
  if (!printSession.active && victronBleStarted && victronScan && millis() - lastVictronSweepMs >= 5000) {
    lastVictronSweepMs = millis();
    NimBLEScanResults results = victronScan->getResults(1200, false);
    for (int i = 0; i < results.getCount(); ++i) {
      const NimBLEAdvertisedDevice *d = results.getDevice(i);
      if (d) {
        processVictronAdvertisedDevice(*d);
      }
    }
    victronScan->clearResults();
  }
  if (!printSession.active && millis() - lastWifiStatusLogMs >= 10000) {
    lastWifiStatusLogMs = millis();
    logf("WiFi status=%d sta=%s ap=%s ap_ip=%s sta_ip=%s bt=%s",
         static_cast<int>(WiFi.status()),
         settings.wifiSsid.c_str(),
         setupApActive ? setupApName().c_str() : "off",
         WiFi.softAPIP().toString().c_str(),
         WiFi.localIP().toString().c_str(),
         bleControllerReady ? "on" : "off");
  }
  if (staJoinActive && WiFi.status() != WL_CONNECTED && millis() - staJoinStartedMs >= WIFI_JOIN_TIMEOUT_MS) {
    staJoinActive = false;
    ++wifiConnectFailures;
    WiFi.disconnect(false, true);
    logf("Wi-Fi join timed out, count=%u", wifiConnectFailures);
    if (wifiConnectFailures >= 3) {
      provisioningMode = true;
      staConnectRequested = false;
      logLine("Falling back to provisioning mode.");
    }
  }
  if (restartPending && millis() >= restartAtMs) {
    ESP.restart();
  }
  processSerialConsole();
  delay(10);
}
