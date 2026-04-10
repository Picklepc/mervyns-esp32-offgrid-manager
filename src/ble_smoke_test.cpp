#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_bt.h>

#include <vector>

namespace {

constexpr size_t LAB_PRINTER_CHUNK_SIZE = 180;
constexpr size_t LAB_ZLIB_BLOCK_SIZE = 1024;

enum class AddrMode {
  Auto,
  Public,
  Random,
};

struct LabState {
  NimBLEScan *scan = nullptr;
  NimBLEClient *client = nullptr;
  NimBLERemoteService *ff00 = nullptr;
  NimBLERemoteCharacteristic *ff01 = nullptr;
  NimBLERemoteCharacteristic *ff02 = nullptr;
  NimBLERemoteCharacteristic *ff03 = nullptr;
  String targetMac;
  AddrMode targetMode = AddrMode::Auto;
  String connectedMode;
  bool ff03Subscribed = false;
  int32_t lastRssi = 0;
  int32_t lastMtu = 0;
  uint32_t lastBatteryRaw = 0;
  bool batteryKnown = false;
  uint8_t paperValue = 0x20;
  uint8_t locationValue = 0x20;
  uint16_t testRows = 32;
  uint16_t padRows = 0;
  String bannerText = "TEST";
} lab;

String inputLine;

String addrModeName(AddrMode mode) {
  switch (mode) {
    case AddrMode::Public: return "public";
    case AddrMode::Random: return "random";
    case AddrMode::Auto:
    default: return "auto";
  }
}

void prompt() {
  Serial.print("\r\nm100> ");
}

void logLine(const String &line) {
  Serial.println(line);
}

String hexBytes(const uint8_t *data, size_t length) {
  static const char kHexChars[] = "0123456789ABCDEF";
  String out;
  out.reserve(length * 3U);
  for (size_t i = 0; i < length; ++i) {
    if (i) out += ' ';
    out += kHexChars[(data[i] >> 4) & 0x0F];
    out += kHexChars[data[i] & 0x0F];
  }
  return out;
}

String propertySummary(const NimBLERemoteCharacteristic *characteristic) {
  if (!characteristic) return "";
  String summary;
  if (characteristic->canRead()) summary += "read ";
  if (characteristic->canWrite()) summary += "write ";
  if (characteristic->canWriteNoResponse()) summary += "write-no-rsp ";
  if (characteristic->canNotify()) summary += "notify ";
  if (characteristic->canIndicate()) summary += "indicate ";
  summary.trim();
  return summary;
}

std::vector<String> splitTokens(const String &line) {
  std::vector<String> tokens;
  int start = 0;
  const int len = line.length();
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

bool parseUint(const String &raw, uint32_t &valueOut) {
  char *end = nullptr;
  valueOut = static_cast<uint32_t>(strtoul(raw.c_str(), &end, 10));
  return end && *end == '\0';
}

bool parseHexBlob(const String &raw, std::vector<uint8_t> &out) {
  String compact;
  compact.reserve(raw.length());
  for (size_t i = 0; i < raw.length(); ++i) {
    const char ch = raw[i];
    if (isxdigit(static_cast<unsigned char>(ch))) {
      compact += ch;
    }
  }
  if (compact.isEmpty() || (compact.length() % 2) != 0) {
    return false;
  }
  out.clear();
  out.reserve(compact.length() / 2);
  for (size_t i = 0; i < compact.length(); i += 2) {
    const char hi = compact[i];
    const char lo = compact[i + 1];
    const auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    const int a = nibble(hi);
    const int b = nibble(lo);
    if (a < 0 || b < 0) return false;
    out.push_back(static_cast<uint8_t>((a << 4) | b));
  }
  return true;
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

size_t printMasterCompressedLength(size_t rasterLength) {
  const size_t blockCount = (rasterLength + LAB_ZLIB_BLOCK_SIZE - 1) / LAB_ZLIB_BLOCK_SIZE;
  return 2 + rasterLength + (blockCount * 5) + 4;
}

void clearConnectionState() {
  lab.ff00 = nullptr;
  lab.ff01 = nullptr;
  lab.ff02 = nullptr;
  lab.ff03 = nullptr;
  lab.connectedMode = "";
  lab.ff03Subscribed = false;
  lab.lastRssi = 0;
  lab.lastMtu = 0;
}

void disconnectPrinter() {
  if (lab.ff03 && lab.ff03Subscribed) {
    lab.ff03->unsubscribe(true);
  }
  if (lab.client) {
    if (lab.client->isConnected()) {
      lab.client->disconnect();
    }
    NimBLEDevice::deleteClient(lab.client);
    lab.client = nullptr;
  }
  clearConnectionState();
  delay(250);
}

void ff03NotifyCB(NimBLERemoteCharacteristic *characteristic, uint8_t *data, size_t length, bool isNotify) {
  (void)characteristic;
  if (!data || !length) {
    logLine("FF03 notify with empty payload");
    return;
  }
  if (length >= 2 && data[0] == 0x02) {
    lab.batteryKnown = true;
    lab.lastBatteryRaw = data[1];
  }
  Serial.printf("\r\nFF03 %s: %s\r\n", isNotify ? "notify" : "indicate", hexBytes(data, length).c_str());
  prompt();
}

bool refreshPrinterHandles(bool forceDiscover) {
  if (!lab.client || !lab.client->isConnected()) {
    clearConnectionState();
    return false;
  }
  const auto &services = lab.client->getServices(forceDiscover);
  (void)services;
  lab.ff00 = lab.client->getService("ff00");
  lab.ff01 = lab.ff00 ? lab.ff00->getCharacteristic("ff01") : nullptr;
  lab.ff02 = lab.ff00 ? lab.ff00->getCharacteristic("ff02") : nullptr;
  lab.ff03 = lab.ff00 ? lab.ff00->getCharacteristic("ff03") : nullptr;
  lab.lastRssi = lab.client->getRssi();
  lab.lastMtu = lab.client->getMTU();
  return lab.ff00 && lab.ff02;
}

void evictPeerClientRecords() {
  if (!lab.targetMac.length()) return;
  const std::string mac(lab.targetMac.c_str());
  NimBLEAddress publicAddr(mac, BLE_ADDR_PUBLIC);
  NimBLEAddress randomAddr(mac, BLE_ADDR_RANDOM);
  NimBLEClient *existing = NimBLEDevice::getClientByPeerAddress(publicAddr);
  if (!existing) existing = NimBLEDevice::getClientByPeerAddress(randomAddr);
  if (existing) {
    NimBLEDevice::deleteClient(existing);
    delay(250);
  }
}

bool connectAttempt(NimBLEClient *client, AddrMode mode, int &lastError) {
  if (!client || !lab.targetMac.length()) {
    lastError = -1;
    return false;
  }
  const std::string mac(lab.targetMac.c_str());
  if (mode == AddrMode::Public) {
    NimBLEAddress address(mac, BLE_ADDR_PUBLIC);
    if (client->connect(address, true, false, true)) return true;
    lastError = client->getLastError();
    return false;
  }
  if (mode == AddrMode::Random) {
    NimBLEAddress address(mac, BLE_ADDR_RANDOM);
    if (client->connect(address, true, false, true)) return true;
    lastError = client->getLastError();
    return false;
  }
  NimBLEAddress publicAddress(mac, BLE_ADDR_PUBLIC);
  if (client->connect(publicAddress, true, false, true)) {
    lab.connectedMode = "public";
    lastError = 0;
    return true;
  }
  lastError = client->getLastError();
  NimBLEAddress randomAddress(mac, BLE_ADDR_RANDOM);
  if (client->connect(randomAddress, true, false, true)) {
    lab.connectedMode = "random";
    lastError = 0;
    return true;
  }
  lastError = client->getLastError();
  return false;
}

NimBLERemoteCharacteristic *resolveCharacteristicToken(const String &token) {
  const String lowered = String(token);
  if (lowered.equalsIgnoreCase("ff01")) return lab.ff01;
  if (lowered.equalsIgnoreCase("ff02")) return lab.ff02;
  if (lowered.equalsIgnoreCase("ff03")) return lab.ff03;
  if (!lab.ff00) return nullptr;
  NimBLEUUID uuid(std::string(token.c_str()));
  return lab.ff00->getCharacteristic(uuid);
}

void printServiceTree() {
  if (!lab.client || !lab.client->isConnected()) {
    logLine("Not connected.");
    return;
  }
  if (!refreshPrinterHandles(true)) {
    logLine("Connected, but FF00 / FF02 could not be refreshed.");
    return;
  }
  const auto &services = lab.client->getServices(false);
  Serial.printf("Connected via %s, RSSI=%d dBm, MTU=%d\r\n",
                lab.connectedMode.length() ? lab.connectedMode.c_str() : addrModeName(lab.targetMode).c_str(),
                lab.lastRssi,
                lab.lastMtu);
  for (const auto *service : services) {
    if (!service) continue;
    Serial.printf("  service %s handles %u-%u\r\n",
                  service->getUUID().toString().c_str(),
                  service->getStartHandle(),
                  service->getEndHandle());
    const auto &characteristics = service->getCharacteristics(true);
    for (const auto *characteristic : characteristics) {
      if (!characteristic) continue;
      Serial.printf("    char %s handle %u %s\r\n",
                    characteristic->getUUID().toString().c_str(),
                    characteristic->getHandle(),
                    propertySummary(characteristic).c_str());
    }
  }
}

void printHelp() {
  logLine("M100 BLE lab commands:");
  logLine("  help");
  logLine("  scan [seconds] [active|passive]");
  logLine("  target <mac> [auto|public|random]");
  logLine("  connect");
  logLine("  disconnect");
  logLine("  probe");
  logLine("  read <ff01|ff02|ff03|uuid>");
  logLine("  subscribe <ff03|uuid>");
  logLine("  unsubscribe <ff03|uuid>");
  logLine("  paper <hex>");
  logLine("  location <hex>");
  logLine("  rows <decimal>");
  logLine("  pad <decimal>");
  logLine("  text <message>");
  logLine("  compact <checker|bar|box|ink|solidff|solid00>");
  logLine("  write <ff02|uuid> <nr|rsp> <hex bytes>");
  logLine("  repeat <count> <delayMs> <ff02|uuid> <nr|rsp> <hex bytes>");
  logLine("  preset <battery|model|status|paper|getdensity|getspeed|feed|selfcheck|session|stop|align>");
  logLine("  sequence <info|startjob|nudge|calibrate|seekprint|seekink|seeksolidff|seeksolid00|seeklegacyff|seeklegacy00|legacyff|legacy00|legacyink|legacyffraw|legacy00raw|legacyinkraw|legacyffhold|legacy00hold|legacyinkhold|legacyffbare|legacy00bare|legacyinkbare|legacyffbarehold|legacy00barehold|legacyinkbarehold|tinyjob|inkjob>");
  logLine("  combo <paperHex> <locationHex> [calibrate|tinyjob|inkjob|seekprint|seekink|seeksolidff|seeksolid00|seeklegacyff|seeklegacy00|legacyff|legacy00|legacyink|legacyffraw|legacy00raw|legacyinkraw|legacyffhold|legacy00hold|legacyinkhold|legacyffbare|legacy00bare|legacyinkbare|legacyffbarehold|legacy00barehold|legacyinkbarehold]");
  logLine("  sweep11 <fromHex> <toHex> [delayMs=160]");
  logLine("  sweepc0 <fromHex> <toHex> [delayMs=160]");
  logLine("  sweep12 <fromHex> <toHex> [lowHex=00] [delayMs=180]");
  logLine("  sweep80 <fromHex> <toHex> [delayMs=180]");
  logLine("  sweep1f <fromHex> <toHex> [argHex=00] [delayMs=120]");
  logLine("  sweep10ff <fromHex> <toHex> [tailHex=F0] [delayMs=120]");
  logLine("Examples:");
  logLine("  target 63:26:ab:63:ab:5f public");
  logLine("  connect");
  logLine("  paper 20");
  logLine("  location 20");
  logLine("  rows 32");
  logLine("  pad 48");
  logLine("  text MERVYNS");
  logLine("  compact checker");
  logLine("  write ff02 nr 1F 11 50");
  logLine("  repeat 10 80 ff02 nr 1F 11 50");
  logLine("  sweep1f 10 90 00 150");
  logLine("  sequence seekprint");
  logLine("  combo 0A 20 seekink");
  logLine("  combo 0A 20 seeksolid00");
  logLine("  combo 0A 20 seeklegacyff");
  logLine("  sequence legacyinkraw");
  logLine("  sequence legacyinkhold");
  logLine("  sequence legacyinkbare");
}

bool writeCharacteristic(NimBLERemoteCharacteristic *characteristic,
                         const std::vector<uint8_t> &payload,
                         bool withResponse,
                         const String &label) {
  if (!characteristic) {
    logLine("Characteristic not found.");
    return false;
  }
  if (payload.empty()) {
    logLine("Payload is empty.");
    return false;
  }
  const bool canUseNoRsp = characteristic->canWriteNoResponse();
  const bool canUseRsp = characteristic->canWrite();
  if (withResponse && !canUseRsp) {
    logLine("Characteristic does not support write-with-response.");
    return false;
  }
  if (!withResponse && !canUseNoRsp && !canUseRsp) {
    logLine("Characteristic is not writable.");
    return false;
  }

  Serial.printf("%s (%s): %s\r\n",
                label.c_str(),
                withResponse ? "rsp" : (canUseNoRsp ? "no-rsp" : "rsp-fallback"),
                hexBytes(payload.data(), payload.size()).c_str());

  bool ok = false;
  if (!withResponse && canUseNoRsp) {
    ok = characteristic->writeValue(payload.data(), payload.size(), false);
  } else {
    ok = characteristic->writeValue(payload.data(), payload.size(), true);
  }
  if (!ok) {
    Serial.printf("write failed, rc=%d\r\n", lab.client ? lab.client->getLastError() : -1);
    return false;
  }
  delay(withResponse ? 20 : 10);
  return true;
}

bool writeCharacteristicBytes(NimBLERemoteCharacteristic *characteristic,
                              const uint8_t *data,
                              size_t length,
                              bool withResponse,
                              const String &label) {
  if (!characteristic) {
    logLine("Characteristic not found.");
    return false;
  }
  if (!data || !length) {
    logLine("Payload is empty.");
    return false;
  }
  const bool canUseNoRsp = characteristic->canWriteNoResponse();
  const bool canUseRsp = characteristic->canWrite();
  if (withResponse && !canUseRsp) {
    logLine("Characteristic does not support write-with-response.");
    return false;
  }
  if (!withResponse && !canUseNoRsp && !canUseRsp) {
    logLine("Characteristic is not writable.");
    return false;
  }

  Serial.printf("%s (%s): %u bytes\r\n",
                label.c_str(),
                withResponse ? "rsp" : (canUseNoRsp ? "no-rsp" : "rsp-fallback"),
                static_cast<unsigned>(length));

  const size_t chunkSize = std::min<size_t>(LAB_PRINTER_CHUNK_SIZE,
                                            std::max<int>(20, lab.client ? lab.client->getMTU() - 3 : 20));
  size_t offset = 0;
  while (offset < length) {
    const size_t thisChunk = std::min(chunkSize, length - offset);
    bool ok = false;
    if (!withResponse && canUseNoRsp) {
      ok = characteristic->writeValue(data + offset, thisChunk, false);
    } else {
      ok = characteristic->writeValue(data + offset, thisChunk, true);
    }
    if (!ok) {
      Serial.printf("chunk write failed at %u/%u, rc=%d\r\n",
                    static_cast<unsigned>(offset),
                    static_cast<unsigned>(length),
                    lab.client ? lab.client->getLastError() : -1);
      return false;
    }
    offset += thisChunk;
    delay(withResponse ? 20 : 10);
  }
  return true;
}

bool writeStoredDeflateBlock(NimBLERemoteCharacteristic *characteristic,
                             const uint8_t *data,
                             size_t length,
                             bool isFinalBlock,
                             const String &label) {
  if (!characteristic || !data || !length || length > 65535U) {
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
  return writeCharacteristicBytes(characteristic, header, sizeof(header), false, label + " deflate-header")
      && writeCharacteristicBytes(characteristic, data, length, false, label + " raw");
}

std::vector<uint8_t> makeTinyRaster(size_t widthBytes, size_t height) {
  std::vector<uint8_t> raster(widthBytes * height, 0x00);
  if (raster.empty()) return raster;

  for (size_t y = 0; y < height; ++y) {
    uint8_t *row = raster.data() + y * widthBytes;
    if (y == 0 || y + 1 == height) {
      memset(row, 0xFF, widthBytes);
      continue;
    }
    row[0] = 0xFF;
    row[widthBytes - 1] = 0xFF;
    if (y >= (height / 2U > 2U ? height / 2U - 2U : 0U) && y <= (height / 2U + 1U)) {
      memset(row + widthBytes / 4U, 0xFF, widthBytes / 2U);
    }
  }
  return raster;
}

std::vector<uint8_t> makeInkRaster(size_t widthBytes, size_t height) {
  std::vector<uint8_t> raster(widthBytes * height, 0x00);
  if (raster.empty()) return raster;

  for (size_t y = 0; y < height; ++y) {
    uint8_t *row = raster.data() + y * widthBytes;
    if (y < 2 || y + 2 >= height) {
      memset(row, 0xFF, widthBytes);
      continue;
    }
    if ((y / 4U) % 2U == 0U) {
      memset(row, 0xFF, widthBytes);
    } else {
      memset(row, 0x00, widthBytes);
      row[0] = 0xFF;
      row[widthBytes - 1] = 0xFF;
      if (widthBytes > 8) {
        memset(row + widthBytes / 3U, 0xFF, widthBytes / 3U);
      }
    }
  }
  return raster;
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

bool sendTinyJobWithRaster(const std::vector<uint8_t> &raster, uint16_t widthBytes, uint16_t height, const String &prefix) {
  if (!lab.ff02) {
    logLine("Not connected to FF02.");
    return false;
  }
  const size_t compressedLength = printMasterCompressedLength(raster.size());
  const uint32_t adler = updateAdler32(1U, raster.data(), raster.size());

  const uint8_t session[] = {0x1F, 0xB2, 0x00};
  const uint8_t setDensity[] = {0x1F, 0x70, 0x01, 0x08};
  const uint8_t setSpeed[] = {0x1F, 0x60, 0x01, 0x01};
  const uint8_t setPaper[] = {0x1F, 0x80, 0x01, lab.paperValue};
  const uint8_t setLocation[] = {0x1F, 0x12, lab.locationValue, 0x00};
  const uint8_t startPrint[] = {0x1F, 0xC0, 0x01, 0x00};
  const uint8_t alignStart[] = {0x1F, 0x11, 0x51};
  const uint8_t imageHeader[] = {
      0x1F, 0x10,
      static_cast<uint8_t>((widthBytes >> 8) & 0xFF),
      static_cast<uint8_t>(widthBytes & 0xFF),
      static_cast<uint8_t>((height >> 8) & 0xFF),
      static_cast<uint8_t>(height & 0xFF),
      static_cast<uint8_t>((compressedLength >> 24) & 0xFF),
      static_cast<uint8_t>((compressedLength >> 16) & 0xFF),
      static_cast<uint8_t>((compressedLength >> 8) & 0xFF),
      static_cast<uint8_t>(compressedLength & 0xFF)
  };
  const uint8_t zlibHeader[] = {0x28, 0x15};
  const uint8_t adlerBytes[] = {
      static_cast<uint8_t>((adler >> 24) & 0xFF),
      static_cast<uint8_t>((adler >> 16) & 0xFF),
      static_cast<uint8_t>((adler >> 8) & 0xFF),
      static_cast<uint8_t>(adler & 0xFF)
  };
  const uint8_t stopPrint[] = {0x1F, 0xC0, 0x01, 0x01};
  const uint8_t alignEnd[] = {0x1F, 0x11, 0x50};
  const auto sendCmd = [](const uint8_t *data, size_t length, const String &label, uint32_t settleMs) -> bool {
    if (!writeCharacteristicBytes(lab.ff02, data, length, false, label)) {
      return false;
    }
    delay(settleMs);
    return true;
  };

  return sendCmd(session, sizeof(session), prefix + " session", 120)
      && sendCmd(setDensity, sizeof(setDensity), prefix + " set-density", 120)
      && sendCmd(setSpeed, sizeof(setSpeed), prefix + " set-speed", 120)
      && sendCmd(setPaper, sizeof(setPaper), prefix + " set-paper", 120)
      && sendCmd(setLocation, sizeof(setLocation), prefix + " set-location", 120)
      && sendCmd(startPrint, sizeof(startPrint), prefix + " start-print", 120)
      && sendCmd(alignStart, sizeof(alignStart), prefix + " align-start", 180)
      && sendCmd(imageHeader, sizeof(imageHeader), prefix + " image-header", 120)
      && sendCmd(zlibHeader, sizeof(zlibHeader), prefix + " zlib-header", 80)
      && writeStoredDeflateBlock(lab.ff02, raster.data(), raster.size(), true, prefix + " image")
      && sendCmd(adlerBytes, sizeof(adlerBytes), prefix + " adler32", 80)
      && sendCmd(stopPrint, sizeof(stopPrint), prefix + " stop-print", 120)
      && sendCmd(alignEnd, sizeof(alignEnd), prefix + " align-end", 220);
}

bool sendTinyJob() {
  constexpr uint16_t widthBytes = 48;
  const uint16_t contentRows = std::max<uint16_t>(8, lab.testRows);
  const std::vector<uint8_t> raster = prependBlankRows(makeTinyRaster(widthBytes, contentRows), widthBytes, lab.padRows);
  const uint16_t height = static_cast<uint16_t>(raster.size() / widthBytes);
  return sendTinyJobWithRaster(raster, widthBytes, height, "tinyjob");
}

bool sendInkJob() {
  constexpr uint16_t widthBytes = 48;
  const uint16_t contentRows = std::max<uint16_t>(8, lab.testRows);
  const std::vector<uint8_t> raster = prependBlankRows(makeInkRaster(widthBytes, contentRows), widthBytes, lab.padRows);
  const uint16_t height = static_cast<uint16_t>(raster.size() / widthBytes);
  return sendTinyJobWithRaster(raster, widthBytes, height, "inkjob");
}

bool sendSolidJob(uint8_t fillByte, const String &prefix) {
  constexpr uint16_t widthBytes = 48;
  const uint16_t contentRows = std::max<uint16_t>(8, lab.testRows);
  const std::vector<uint8_t> raster = prependBlankRows(makeSolidRaster(widthBytes, contentRows, fillByte), widthBytes, lab.padRows);
  const uint16_t height = static_cast<uint16_t>(raster.size() / widthBytes);
  return sendTinyJobWithRaster(raster, widthBytes, height, prefix);
}

bool sendLegacyRasterJob(const std::vector<uint8_t> &raster,
                         uint16_t widthBytes,
                         uint16_t height,
                         const String &prefix,
                         bool includeSetup,
                         bool includeFooter) {
  if (!lab.ff02) {
    logLine("Not connected to FF02.");
    return false;
  }

  const uint8_t session[] = {0x1F, 0xB2, 0x00};
  const uint8_t setPaper[] = {0x1F, 0x80, 0x01, lab.paperValue};
  const uint8_t setLocation[] = {0x1F, 0x12, lab.locationValue, 0x00};
  const uint8_t setSpeed[] = {0x1B, 0x4E, 0x0D, 0x01};
  const uint8_t setDarkness[] = {0x1B, 0x4E, 0x04, 0x08};
  const uint8_t selectPaper[] = {0x1F, 0x11, lab.paperValue};
  const uint8_t rasterHeader[] = {
      0x1D, 0x76, 0x30, 0x00,
      static_cast<uint8_t>(widthBytes & 0xFF), 0x00,
      static_cast<uint8_t>(height & 0xFF),
      static_cast<uint8_t>((height >> 8) & 0xFF)
  };
  const uint8_t footerA[] = {0x1F, 0xF0, 0x05, 0x00};
  const uint8_t footerB[] = {0x1F, 0xF0, 0x03, 0x00};

  const auto sendCmd = [](const uint8_t *data, size_t length, const String &label, uint32_t settleMs) -> bool {
    if (!writeCharacteristicBytes(lab.ff02, data, length, false, label)) {
      return false;
    }
    delay(settleMs);
    return true;
  };

  if (includeSetup) {
    if (!sendCmd(session, sizeof(session), prefix + " session", 120)
        || !sendCmd(setPaper, sizeof(setPaper), prefix + " set-paper", 120)
        || !sendCmd(setLocation, sizeof(setLocation), prefix + " set-location", 120)
        || !sendCmd(setSpeed, sizeof(setSpeed), prefix + " legacy-set-speed", 120)
        || !sendCmd(setDarkness, sizeof(setDarkness), prefix + " legacy-set-darkness", 120)
        || !sendCmd(selectPaper, sizeof(selectPaper), prefix + " legacy-select-paper", 120)) {
      return false;
    }
  } else {
    if (!sendCmd(setSpeed, sizeof(setSpeed), prefix + " legacy-set-speed", 80)
        || !sendCmd(setDarkness, sizeof(setDarkness), prefix + " legacy-set-darkness", 80)) {
      return false;
    }
  }

  if (!sendCmd(rasterHeader, sizeof(rasterHeader), prefix + " legacy-raster-header", 120)
      || !writeCharacteristicBytes(lab.ff02, raster.data(), raster.size(), false, prefix + " legacy-raster")) {
    return false;
  }
  if (!includeFooter) {
    logLine(prefix + " footer skipped");
    return true;
  }
  return sendCmd(footerA, sizeof(footerA), prefix + " legacy-footer-a", 120)
      && sendCmd(footerB, sizeof(footerB), prefix + " legacy-footer-b", 260);
}

bool sendLegacyBareRasterJob(const std::vector<uint8_t> &raster,
                             uint16_t widthBytes,
                             uint16_t height,
                             const String &prefix,
                             bool includeFooter) {
  if (!lab.ff02) {
    logLine("Not connected to FF02.");
    return false;
  }
  const uint8_t rasterHeader[] = {
      0x1D, 0x76, 0x30, 0x00,
      static_cast<uint8_t>(widthBytes & 0xFF), 0x00,
      static_cast<uint8_t>(height & 0xFF),
      static_cast<uint8_t>((height >> 8) & 0xFF)
  };
  const uint8_t footerA[] = {0x1F, 0xF0, 0x05, 0x00};
  const uint8_t footerB[] = {0x1F, 0xF0, 0x03, 0x00};

  const auto sendCmd = [](const uint8_t *data, size_t length, const String &label, uint32_t settleMs) -> bool {
    if (!writeCharacteristicBytes(lab.ff02, data, length, false, label)) {
      return false;
    }
    delay(settleMs);
    return true;
  };

  if (!sendCmd(rasterHeader, sizeof(rasterHeader), prefix + " bare-raster-header", 80)
      || !writeCharacteristicBytes(lab.ff02, raster.data(), raster.size(), false, prefix + " bare-raster")) {
    return false;
  }
  if (!includeFooter) {
    logLine(prefix + " bare footer skipped");
    return true;
  }
  return sendCmd(footerA, sizeof(footerA), prefix + " bare-footer-a", 120)
      && sendCmd(footerB, sizeof(footerB), prefix + " bare-footer-b", 260);
}

bool sendLegacySolidJob(uint8_t fillByte, const String &prefix, bool includeSetup = true, bool includeFooter = true) {
  constexpr uint16_t widthBytes = 48;
  const uint16_t contentRows = std::max<uint16_t>(8, lab.testRows);
  const std::vector<uint8_t> raster = prependBlankRows(makeSolidRaster(widthBytes, contentRows, fillByte), widthBytes, lab.padRows);
  const uint16_t height = static_cast<uint16_t>(raster.size() / widthBytes);
  return sendLegacyRasterJob(raster, widthBytes, height, prefix, includeSetup, includeFooter);
}

bool sendLegacyInkJob() {
  constexpr uint16_t widthBytes = 48;
  const uint16_t contentRows = std::max<uint16_t>(8, lab.testRows);
  const std::vector<uint8_t> raster = prependBlankRows(makeInkRaster(widthBytes, contentRows), widthBytes, lab.padRows);
  const uint16_t height = static_cast<uint16_t>(raster.size() / widthBytes);
  return sendLegacyRasterJob(raster, widthBytes, height, "legacyink", true, true);
}

bool sendLegacyInkRawJob() {
  constexpr uint16_t widthBytes = 48;
  const uint16_t contentRows = std::max<uint16_t>(8, lab.testRows);
  const std::vector<uint8_t> raster = prependBlankRows(makeInkRaster(widthBytes, contentRows), widthBytes, lab.padRows);
  const uint16_t height = static_cast<uint16_t>(raster.size() / widthBytes);
  return sendLegacyRasterJob(raster, widthBytes, height, "legacyinkraw", false, true);
}

bool sendLegacyInkHoldJob() {
  constexpr uint16_t widthBytes = 48;
  const uint16_t contentRows = std::max<uint16_t>(8, lab.testRows);
  const std::vector<uint8_t> raster = prependBlankRows(makeInkRaster(widthBytes, contentRows), widthBytes, lab.padRows);
  const uint16_t height = static_cast<uint16_t>(raster.size() / widthBytes);
  return sendLegacyRasterJob(raster, widthBytes, height, "legacyinkhold", false, false);
}

bool sendLegacySolidBareJob(uint8_t fillByte, const String &prefix, bool includeFooter) {
  constexpr uint16_t widthBytes = 48;
  const uint16_t contentRows = std::max<uint16_t>(8, lab.testRows);
  const std::vector<uint8_t> raster = prependBlankRows(makeSolidRaster(widthBytes, contentRows, fillByte), widthBytes, lab.padRows);
  const uint16_t height = static_cast<uint16_t>(raster.size() / widthBytes);
  return sendLegacyBareRasterJob(raster, widthBytes, height, prefix, includeFooter);
}

bool sendLegacyInkBareJob(bool includeFooter) {
  constexpr uint16_t widthBytes = 48;
  const uint16_t contentRows = std::max<uint16_t>(8, lab.testRows);
  const std::vector<uint8_t> raster = prependBlankRows(makeInkRaster(widthBytes, contentRows), widthBytes, lab.padRows);
  const uint16_t height = static_cast<uint16_t>(raster.size() / widthBytes);
  return sendLegacyBareRasterJob(raster, widthBytes, height, "legacyinkbare", includeFooter);
}

bool sendCustomBareRaster(const std::vector<uint8_t> &raster, const String &prefix) {
  constexpr uint16_t widthBytes = 48;
  if (raster.empty() || (raster.size() % widthBytes) != 0) {
    logLine("Custom raster was invalid.");
    return false;
  }
  const uint16_t height = static_cast<uint16_t>(raster.size() / widthBytes);
  return sendLegacyBareRasterJob(raster, widthBytes, height, prefix, false);
}

bool runCalibrationSequence() {
  if (!lab.ff02) {
    logLine("Not connected to FF02.");
    return false;
  }
  const uint8_t session[] = {0x1F, 0xB2, 0x00};
  const uint8_t setPaper[] = {0x1F, 0x80, 0x01, lab.paperValue};
  const uint8_t setLocation[] = {0x1F, 0x12, lab.locationValue, 0x00};
  const uint8_t startPrint[] = {0x1F, 0xC0, 0x01, 0x00};
  const uint8_t alignStart[] = {0x1F, 0x11, 0x51};
  const uint8_t alignEnd[] = {0x1F, 0x11, 0x50};
  const uint8_t stopPrint[] = {0x1F, 0xC0, 0x01, 0x01};

  const auto sendCmd = [](const uint8_t *data, size_t length, const String &label, uint32_t settleMs) -> bool {
    if (!writeCharacteristicBytes(lab.ff02, data, length, false, label)) {
      return false;
    }
    delay(settleMs);
    return true;
  };

  return sendCmd(session, sizeof(session), "calibrate session", 120)
      && sendCmd(setPaper, sizeof(setPaper), "calibrate set-paper", 180)
      && sendCmd(setLocation, sizeof(setLocation), "calibrate set-location", 180)
      && sendCmd(startPrint, sizeof(startPrint), "calibrate start-print", 180)
      && sendCmd(alignStart, sizeof(alignStart), "calibrate align-start", 260)
      && sendCmd(alignEnd, sizeof(alignEnd), "calibrate align-end", 260)
      && sendCmd(stopPrint, sizeof(stopPrint), "calibrate stop-print", 200);
}

bool runSeekAndTinyJob() {
  return runCalibrationSequence() && sendTinyJob();
}

bool runSeekAndInkJob() {
  return runCalibrationSequence() && sendInkJob();
}

bool runSeekAndSolidJob(uint8_t fillByte, const String &prefix) {
  return runCalibrationSequence() && sendSolidJob(fillByte, prefix);
}

bool runSeekAndLegacySolidJob(uint8_t fillByte, const String &prefix) {
  return runCalibrationSequence() && sendLegacySolidJob(fillByte, prefix);
}

bool runSeekAndLegacyInkJob() {
  return runCalibrationSequence() && sendLegacyInkJob();
}

bool sendPreset(const String &name) {
  if (!lab.ff02) {
    logLine("Not connected to FF02.");
    return false;
  }
  std::vector<uint8_t> payload;
  String label = name;
  bool withResponse = false;

  if (name.equalsIgnoreCase("battery")) payload = {0x10, 0xFF, 0x50, 0xF1};
  else if (name.equalsIgnoreCase("model")) payload = {0x10, 0xFF, 0x20, 0xF0};
  else if (name.equalsIgnoreCase("status")) payload = {0x1F, 0x20, 0x00};
  else if (name.equalsIgnoreCase("paper")) payload = {0x1F, 0x80, 0x00};
  else if (name.equalsIgnoreCase("getdensity")) payload = {0x1F, 0x70, 0x00};
  else if (name.equalsIgnoreCase("getspeed")) payload = {0x1F, 0x60, 0x00};
  else if (name.equalsIgnoreCase("feed")) payload = {0x1F, 0x11, 0x50};
  else if (name.equalsIgnoreCase("selfcheck")) payload = {0x1F, 0x40};
  else if (name.equalsIgnoreCase("session")) payload = {0x1F, 0xB2, 0x00};
  else if (name.equalsIgnoreCase("stop")) payload = {0x1F, 0xC0, 0x01, 0x01};
  else if (name.equalsIgnoreCase("align")) payload = {0x1F, 0x11, 0x51};
  else {
    logLine("Unknown preset.");
    return false;
  }
  return writeCharacteristic(lab.ff02, payload, withResponse, String("preset ") + label);
}

bool sendSequence(const String &name) {
  if (!lab.ff02) {
    logLine("Not connected to FF02.");
    return false;
  }

  struct Step {
    const char *label;
    std::vector<uint8_t> payload;
    uint32_t delayMs;
  };

  std::vector<Step> steps;
  if (name.equalsIgnoreCase("info")) {
    steps = {
      {"session", {0x1F, 0xB2, 0x00}, 120},
      {"battery", {0x10, 0xFF, 0x50, 0xF1}, 120},
      {"model", {0x10, 0xFF, 0x20, 0xF0}, 120},
      {"status", {0x1F, 0x20, 0x00}, 120},
      {"paper", {0x1F, 0x80, 0x00}, 120},
      {"getdensity", {0x1F, 0x70, 0x00}, 120},
      {"getspeed", {0x1F, 0x60, 0x00}, 120},
    };
  } else if (name.equalsIgnoreCase("startjob")) {
    steps = {
      {"session", {0x1F, 0xB2, 0x00}, 140},
      {"set-density", {0x1F, 0x70, 0x01, 0x08}, 140},
      {"set-speed", {0x1F, 0x60, 0x01, 0x01}, 140},
      {"set-paper", {0x1F, 0x80, 0x01, 0x20}, 140},
      {"set-location", {0x1F, 0x12, 0x20, 0x00}, 140},
      {"start-print", {0x1F, 0xC0, 0x01, 0x00}, 140},
      {"align-start", {0x1F, 0x11, 0x51}, 180},
    };
  } else if (name.equalsIgnoreCase("nudge")) {
    steps = {
      {"session", {0x1F, 0xB2, 0x00}, 140},
      {"start-print", {0x1F, 0xC0, 0x01, 0x00}, 140},
      {"align-start", {0x1F, 0x11, 0x51}, 180},
      {"feed", {0x1F, 0x11, 0x50}, 220},
      {"selfcheck", {0x1F, 0x40}, 220},
      {"stop-print", {0x1F, 0xC0, 0x01, 0x01}, 140},
    };
  } else if (name.equalsIgnoreCase("calibrate")) {
    return runCalibrationSequence();
  } else if (name.equalsIgnoreCase("seekprint")) {
    return runSeekAndTinyJob();
  } else if (name.equalsIgnoreCase("seekink")) {
    return runSeekAndInkJob();
  } else if (name.equalsIgnoreCase("seeksolidff")) {
    return runSeekAndSolidJob(0xFF, "solidff");
  } else if (name.equalsIgnoreCase("seeksolid00")) {
    return runSeekAndSolidJob(0x00, "solid00");
  } else if (name.equalsIgnoreCase("seeklegacyff")) {
    return runSeekAndLegacySolidJob(0xFF, "legacyff");
  } else if (name.equalsIgnoreCase("seeklegacy00")) {
    return runSeekAndLegacySolidJob(0x00, "legacy00");
  } else if (name.equalsIgnoreCase("legacyff")) {
    return sendLegacySolidJob(0xFF, "legacyff");
  } else if (name.equalsIgnoreCase("legacy00")) {
    return sendLegacySolidJob(0x00, "legacy00");
  } else if (name.equalsIgnoreCase("legacyink")) {
    return sendLegacyInkJob();
  } else if (name.equalsIgnoreCase("legacyffraw")) {
    return sendLegacySolidJob(0xFF, "legacyffraw", false, true);
  } else if (name.equalsIgnoreCase("legacy00raw")) {
    return sendLegacySolidJob(0x00, "legacy00raw", false, true);
  } else if (name.equalsIgnoreCase("legacyinkraw")) {
    return sendLegacyInkRawJob();
  } else if (name.equalsIgnoreCase("legacyffhold")) {
    return sendLegacySolidJob(0xFF, "legacyffhold", false, false);
  } else if (name.equalsIgnoreCase("legacy00hold")) {
    return sendLegacySolidJob(0x00, "legacy00hold", false, false);
  } else if (name.equalsIgnoreCase("legacyinkhold")) {
    return sendLegacyInkHoldJob();
  } else if (name.equalsIgnoreCase("legacyffbare")) {
    return sendLegacySolidBareJob(0xFF, "legacyffbare", true);
  } else if (name.equalsIgnoreCase("legacy00bare")) {
    return sendLegacySolidBareJob(0x00, "legacy00bare", true);
  } else if (name.equalsIgnoreCase("legacyinkbare")) {
    return sendLegacyInkBareJob(true);
  } else if (name.equalsIgnoreCase("legacyffbarehold")) {
    return sendLegacySolidBareJob(0xFF, "legacyffbarehold", false);
  } else if (name.equalsIgnoreCase("legacy00barehold")) {
    return sendLegacySolidBareJob(0x00, "legacy00barehold", false);
  } else if (name.equalsIgnoreCase("legacyinkbarehold")) {
    return sendLegacyInkBareJob(false);
  } else if (name.equalsIgnoreCase("tinyjob")) {
    return sendTinyJob();
  } else if (name.equalsIgnoreCase("inkjob")) {
    return sendInkJob();
  } else {
    logLine("Unknown sequence. Use info, startjob, nudge, calibrate, seekprint, seekink, seeksolidff, seeksolid00, seeklegacyff, seeklegacy00, legacyff, legacy00, legacyink, legacyffraw, legacy00raw, legacyinkraw, legacyffhold, legacy00hold, legacyinkhold, legacyffbare, legacy00bare, legacyinkbare, legacyffbarehold, legacy00barehold, legacyinkbarehold, tinyjob, or inkjob.");
    return false;
  }

  for (const auto &step : steps) {
    if (!writeCharacteristic(lab.ff02, step.payload, false, String("sequence ") + step.label)) {
      return false;
    }
    delay(step.delayMs);
  }
  return true;
}

void scanForDevices(uint32_t seconds, bool active) {
  if (!lab.scan) {
    logLine("Scan object unavailable.");
    return;
  }
  if (lab.scan->isScanning()) {
    lab.scan->stop();
    delay(100);
  }
  lab.scan->setActiveScan(active);
  lab.scan->setInterval(80);
  lab.scan->setWindow(active ? 60 : 40);
  lab.scan->setDuplicateFilter(0);
  Serial.printf("Scanning for %lu second(s), %s...\r\n",
                static_cast<unsigned long>(seconds),
                active ? "active" : "passive");
  NimBLEScanResults results = lab.scan->getResults(seconds * 1000UL, false);
  Serial.printf("Scan complete, devices=%d\r\n", results.getCount());
  for (int i = 0; i < results.getCount(); ++i) {
    const NimBLEAdvertisedDevice *dev = results.getDevice(i);
    if (!dev) continue;
    Serial.printf("  %2d) %s  %s  RSSI=%d\r\n",
                  i + 1,
                  dev->getAddress().toString().c_str(),
                  dev->getName().c_str(),
                  dev->getRSSI());
  }
  lab.scan->clearResults();
}

bool connectPrinter() {
  if (!lab.targetMac.length()) {
    logLine("Set a target first. Example: target 63:26:ab:63:ab:5f public");
    return false;
  }
  disconnectPrinter();
  evictPeerClientRecords();
  lab.client = NimBLEDevice::getDisconnectedClient();
  if (!lab.client) {
    lab.client = NimBLEDevice::createClient();
  }
  if (!lab.client) {
    logLine("Could not create NimBLE client.");
    return false;
  }
  lab.client->setConnectTimeout(5000);
  lab.client->setConnectionParams(12, 24, 0, 60);

  int lastError = 0;
  bool connected = false;
  if (lab.targetMode == AddrMode::Auto) {
    connected = connectAttempt(lab.client, AddrMode::Auto, lastError);
  } else {
    connected = connectAttempt(lab.client, lab.targetMode, lastError);
    if (connected) {
      lab.connectedMode = addrModeName(lab.targetMode);
    }
  }
  if (!connected) {
    Serial.printf("Connect failed. NimBLE error %d\r\n", lastError);
    disconnectPrinter();
    return false;
  }

  lab.client->updateConnParams(12, 24, 0, 60);
  if (!refreshPrinterHandles(true)) {
    logLine("Connected, but FF00 / FF02 was not found.");
    disconnectPrinter();
    return false;
  }

  Serial.printf("Connected to %s using %s mode. RSSI=%d dBm MTU=%d\r\n",
                lab.targetMac.c_str(),
                lab.connectedMode.c_str(),
                lab.lastRssi,
                lab.lastMtu);
  logLine("Connect complete. Run 'subscribe ff03' if you want live notifications.");
  return true;
}

void readCharacteristicCommand(const String &token) {
  NimBLERemoteCharacteristic *characteristic = resolveCharacteristicToken(token);
  if (!characteristic) {
    logLine("Characteristic not found.");
    return;
  }
  if (!characteristic->canRead()) {
    logLine("Characteristic is not readable.");
    return;
  }
  std::string value = characteristic->readValue();
  if (lab.client && lab.client->getLastError() != 0 && value.empty()) {
    Serial.printf("Read failed, rc=%d\r\n", lab.client->getLastError());
    return;
  }
  if (value.empty()) {
    Serial.printf("%s read: <empty>\r\n", token.c_str());
    return;
  }
  Serial.printf("%s read: %s\r\n", token.c_str(), hexBytes(reinterpret_cast<const uint8_t *>(value.data()), value.size()).c_str());
}

void subscribeCommand(const String &token, bool enable) {
  NimBLERemoteCharacteristic *characteristic = resolveCharacteristicToken(token);
  if (!characteristic) {
    logLine("Characteristic not found.");
    return;
  }
  if (enable) {
    if (characteristic == lab.ff03 && lab.ff03Subscribed) {
      logLine("FF03 is already subscribed.");
      return;
    }
    if (!characteristic->canNotify() && !characteristic->canIndicate()) {
      logLine("Characteristic is not subscribable.");
      return;
    }
    if (characteristic->subscribe(true, ff03NotifyCB, true)) {
      lab.ff03Subscribed = (characteristic == lab.ff03);
      Serial.printf("Subscribed to %s\r\n", token.c_str());
    } else {
      logLine("Subscribe failed.");
    }
    return;
  }
  characteristic->unsubscribe(true);
  if (characteristic == lab.ff03) {
    lab.ff03Subscribed = false;
  }
  Serial.printf("Unsubscribed from %s\r\n", token.c_str());
}

void handleWrite(const std::vector<String> &tokens) {
  if (tokens.size() < 4) {
    logLine("Usage: write <ff02|uuid> <nr|rsp> <hex bytes>");
    return;
  }
  NimBLERemoteCharacteristic *characteristic = resolveCharacteristicToken(tokens[1]);
  const bool withResponse = tokens[2].equalsIgnoreCase("rsp");
  std::vector<uint8_t> payload;
  if (!parseHexBlob(joinTokens(tokens, 3), payload)) {
    logLine("Could not parse hex payload.");
    return;
  }
  writeCharacteristic(characteristic, payload, withResponse, String("write ") + tokens[1]);
}

void handleRepeat(const std::vector<String> &tokens) {
  if (tokens.size() < 6) {
    logLine("Usage: repeat <count> <delayMs> <ff02|uuid> <nr|rsp> <hex bytes>");
    return;
  }
  uint32_t count = 0;
  uint32_t delayMs = 0;
  if (!parseUint(tokens[1], count) || !parseUint(tokens[2], delayMs)) {
    logLine("repeat count and delay must be decimal integers.");
    return;
  }
  NimBLERemoteCharacteristic *characteristic = resolveCharacteristicToken(tokens[3]);
  const bool withResponse = tokens[4].equalsIgnoreCase("rsp");
  std::vector<uint8_t> payload;
  if (!parseHexBlob(joinTokens(tokens, 5), payload)) {
    logLine("Could not parse hex payload.");
    return;
  }
  for (uint32_t i = 0; i < count; ++i) {
    Serial.printf("repeat %lu/%lu\r\n",
                  static_cast<unsigned long>(i + 1),
                  static_cast<unsigned long>(count));
    if (!writeCharacteristic(characteristic, payload, withResponse, String("repeat ") + tokens[3])) {
      break;
    }
    delay(delayMs);
  }
}

void handleSweep1f(const std::vector<String> &tokens) {
  if (tokens.size() < 3) {
    logLine("Usage: sweep1f <fromHex> <toHex> [argHex=00] [delayMs=120]");
    return;
  }
  std::vector<uint8_t> fromVec;
  std::vector<uint8_t> toVec;
  std::vector<uint8_t> argVec = {0x00};
  if (!parseHexBlob(tokens[1], fromVec) || !parseHexBlob(tokens[2], toVec) ||
      fromVec.size() != 1 || toVec.size() != 1) {
    logLine("from/to must be one-byte hex values.");
    return;
  }
  if (tokens.size() >= 4) {
    if (!parseHexBlob(tokens[3], argVec) || argVec.size() != 1) {
      logLine("argHex must be one byte.");
      return;
    }
  }
  uint32_t delayMs = 120;
  if (tokens.size() >= 5 && !parseUint(tokens[4], delayMs)) {
    logLine("delayMs must be a decimal integer.");
    return;
  }
  for (uint16_t cmd = fromVec[0]; cmd <= toVec[0]; ++cmd) {
    std::vector<uint8_t> payload = {0x1F, static_cast<uint8_t>(cmd), argVec[0]};
    if (!writeCharacteristic(lab.ff02, payload, false, "sweep1f")) {
      break;
    }
    delay(delayMs);
    if (cmd == toVec[0]) break;
  }
}

void handleSweep10ff(const std::vector<String> &tokens) {
  if (tokens.size() < 3) {
    logLine("Usage: sweep10ff <fromHex> <toHex> [tailHex=F0] [delayMs=120]");
    return;
  }
  std::vector<uint8_t> fromVec;
  std::vector<uint8_t> toVec;
  std::vector<uint8_t> tailVec = {0xF0};
  if (!parseHexBlob(tokens[1], fromVec) || !parseHexBlob(tokens[2], toVec) ||
      fromVec.size() != 1 || toVec.size() != 1) {
    logLine("from/to must be one-byte hex values.");
    return;
  }
  if (tokens.size() >= 4) {
    if (!parseHexBlob(tokens[3], tailVec) || tailVec.size() != 1) {
      logLine("tailHex must be one byte.");
      return;
    }
  }
  uint32_t delayMs = 120;
  if (tokens.size() >= 5 && !parseUint(tokens[4], delayMs)) {
    logLine("delayMs must be a decimal integer.");
    return;
  }
  for (uint16_t cmd = fromVec[0]; cmd <= toVec[0]; ++cmd) {
    std::vector<uint8_t> payload = {0x10, 0xFF, static_cast<uint8_t>(cmd), tailVec[0]};
    if (!writeCharacteristic(lab.ff02, payload, false, "sweep10ff")) {
      break;
    }
    delay(delayMs);
    if (cmd == toVec[0]) break;
  }
}

void handleSweep11(const std::vector<String> &tokens) {
  if (tokens.size() < 3) {
    logLine("Usage: sweep11 <fromHex> <toHex> [delayMs=160]");
    return;
  }
  std::vector<uint8_t> fromVec;
  std::vector<uint8_t> toVec;
  if (!parseHexBlob(tokens[1], fromVec) || !parseHexBlob(tokens[2], toVec) ||
      fromVec.size() != 1 || toVec.size() != 1) {
    logLine("from/to must be one-byte hex values.");
    return;
  }
  uint32_t delayMs = 160;
  if (tokens.size() >= 4 && !parseUint(tokens[3], delayMs)) {
    logLine("delayMs must be a decimal integer.");
    return;
  }
  for (uint16_t value = fromVec[0]; value <= toVec[0]; ++value) {
    std::vector<uint8_t> payload = {0x1F, 0x11, static_cast<uint8_t>(value)};
    if (!writeCharacteristic(lab.ff02, payload, false, "sweep11")) {
      break;
    }
    delay(delayMs);
    if (value == toVec[0]) break;
  }
}

void handleSweepC0(const std::vector<String> &tokens) {
  if (tokens.size() < 3) {
    logLine("Usage: sweepc0 <fromHex> <toHex> [delayMs=160]");
    return;
  }
  std::vector<uint8_t> fromVec;
  std::vector<uint8_t> toVec;
  if (!parseHexBlob(tokens[1], fromVec) || !parseHexBlob(tokens[2], toVec) ||
      fromVec.size() != 1 || toVec.size() != 1) {
    logLine("from/to must be one-byte hex values.");
    return;
  }
  uint32_t delayMs = 160;
  if (tokens.size() >= 4 && !parseUint(tokens[3], delayMs)) {
    logLine("delayMs must be a decimal integer.");
    return;
  }
  for (uint16_t value = fromVec[0]; value <= toVec[0]; ++value) {
    std::vector<uint8_t> payload = {0x1F, 0xC0, 0x01, static_cast<uint8_t>(value)};
    if (!writeCharacteristic(lab.ff02, payload, false, "sweepc0")) {
      break;
    }
    delay(delayMs);
    if (value == toVec[0]) break;
  }
}

void handleSweep12(const std::vector<String> &tokens) {
  if (tokens.size() < 3) {
    logLine("Usage: sweep12 <fromHex> <toHex> [lowHex=00] [delayMs=180]");
    return;
  }
  std::vector<uint8_t> fromVec;
  std::vector<uint8_t> toVec;
  std::vector<uint8_t> lowVec = {0x00};
  if (!parseHexBlob(tokens[1], fromVec) || !parseHexBlob(tokens[2], toVec) ||
      fromVec.size() != 1 || toVec.size() != 1) {
    logLine("from/to must be one-byte hex values.");
    return;
  }
  if (tokens.size() >= 4) {
    if (!parseHexBlob(tokens[3], lowVec) || lowVec.size() != 1) {
      logLine("lowHex must be one byte.");
      return;
    }
  }
  uint32_t delayMs = 180;
  if (tokens.size() >= 5 && !parseUint(tokens[4], delayMs)) {
    logLine("delayMs must be a decimal integer.");
    return;
  }
  for (uint16_t value = fromVec[0]; value <= toVec[0]; ++value) {
    std::vector<uint8_t> payload = {0x1F, 0x12, static_cast<uint8_t>(value), lowVec[0]};
    if (!writeCharacteristic(lab.ff02, payload, false, "sweep12")) {
      break;
    }
    delay(delayMs);
    if (value == toVec[0]) break;
  }
}

void handleSweep80(const std::vector<String> &tokens) {
  if (tokens.size() < 3) {
    logLine("Usage: sweep80 <fromHex> <toHex> [delayMs=180]");
    return;
  }
  std::vector<uint8_t> fromVec;
  std::vector<uint8_t> toVec;
  if (!parseHexBlob(tokens[1], fromVec) || !parseHexBlob(tokens[2], toVec) ||
      fromVec.size() != 1 || toVec.size() != 1) {
    logLine("from/to must be one-byte hex values.");
    return;
  }
  uint32_t delayMs = 180;
  if (tokens.size() >= 4 && !parseUint(tokens[3], delayMs)) {
    logLine("delayMs must be a decimal integer.");
    return;
  }
  for (uint16_t value = fromVec[0]; value <= toVec[0]; ++value) {
    std::vector<uint8_t> payload = {0x1F, 0x80, 0x01, static_cast<uint8_t>(value)};
    if (!writeCharacteristic(lab.ff02, payload, false, "sweep80")) {
      break;
    }
    delay(delayMs);
    if (value == toVec[0]) break;
  }
}

bool runCompactCommand(const String &name) {
  constexpr uint16_t widthBytes = 48;
  const uint16_t contentRows = std::max<uint16_t>(8, lab.testRows);
  std::vector<uint8_t> content;
  String prefix = "compact-" + name;

  if (name.equalsIgnoreCase("checker")) {
    content = makeCheckerRaster(widthBytes, contentRows);
  } else if (name.equalsIgnoreCase("bar")) {
    content = makeCenterBarRaster(widthBytes, contentRows);
  } else if (name.equalsIgnoreCase("box")) {
    content = makeBoxRaster(widthBytes, contentRows);
  } else if (name.equalsIgnoreCase("ink")) {
    content = makeInkRaster(widthBytes, contentRows);
  } else if (name.equalsIgnoreCase("solidff")) {
    content = makeSolidRaster(widthBytes, contentRows, 0xFF);
  } else if (name.equalsIgnoreCase("solid00")) {
    content = makeSolidRaster(widthBytes, contentRows, 0x00);
  } else {
    logLine("Unknown compact pattern. Use checker, bar, box, ink, solidff, or solid00.");
    return false;
  }

  return sendCustomBareRaster(prependBlankRows(content, widthBytes, lab.padRows), prefix);
}

bool runTextCommand(const String &message) {
  constexpr uint16_t widthBytes = 48;
  const uint16_t contentRows = std::max<uint16_t>(24, lab.testRows);
  lab.bannerText = message;
  return sendCustomBareRaster(prependBlankRows(makeTextRaster(widthBytes, contentRows, lab.bannerText), widthBytes, lab.padRows),
                              "text-" + lab.bannerText);
}

void handleCommand(String line) {
  line.trim();
  if (!line.length()) {
    prompt();
    return;
  }
  const std::vector<String> tokens = splitTokens(line);
  if (tokens.empty()) {
    prompt();
    return;
  }

  const String &cmd = tokens[0];
  if (cmd.equalsIgnoreCase("help") || cmd == "?") {
    printHelp();
  } else if (cmd.equalsIgnoreCase("scan")) {
    uint32_t seconds = 4;
    bool active = true;
    if (tokens.size() >= 2 && !parseUint(tokens[1], seconds)) {
      logLine("scan seconds must be a decimal integer.");
      prompt();
      return;
    }
    if (tokens.size() >= 3) {
      active = !tokens[2].equalsIgnoreCase("passive");
    }
    scanForDevices(seconds, active);
  } else if (cmd.equalsIgnoreCase("target")) {
    if (tokens.size() < 2) {
      logLine("Usage: target <mac> [auto|public|random]");
    } else {
      lab.targetMac = tokens[1];
      if (tokens.size() >= 3) {
        if (tokens[2].equalsIgnoreCase("public")) lab.targetMode = AddrMode::Public;
        else if (tokens[2].equalsIgnoreCase("random")) lab.targetMode = AddrMode::Random;
        else lab.targetMode = AddrMode::Auto;
      }
      Serial.printf("Target set to %s (%s)\r\n", lab.targetMac.c_str(), addrModeName(lab.targetMode).c_str());
    }
  } else if (cmd.equalsIgnoreCase("paper")) {
    if (tokens.size() < 2) {
      logLine("Usage: paper <hex>");
    } else {
      std::vector<uint8_t> value;
      if (!parseHexBlob(tokens[1], value) || value.size() != 1) {
        logLine("paper expects one-byte hex, e.g. 20");
      } else {
        lab.paperValue = value[0];
        Serial.printf("Paper mode set to 0x%02X\r\n", lab.paperValue);
      }
    }
  } else if (cmd.equalsIgnoreCase("location")) {
    if (tokens.size() < 2) {
      logLine("Usage: location <hex>");
    } else {
      std::vector<uint8_t> value;
      if (!parseHexBlob(tokens[1], value) || value.size() != 1) {
        logLine("location expects one-byte hex, e.g. 20");
      } else {
        lab.locationValue = value[0];
        Serial.printf("Location mode set to 0x%02X\r\n", lab.locationValue);
      }
    }
  } else if (cmd.equalsIgnoreCase("rows")) {
    if (tokens.size() < 2) {
      logLine("Usage: rows <decimal>");
    } else {
      uint32_t value = 0;
      if (!parseUint(tokens[1], value)) {
        logLine("rows must be a decimal integer.");
      } else {
        lab.testRows = static_cast<uint16_t>(std::max<uint32_t>(8, std::min<uint32_t>(240, value)));
        Serial.printf("Test rows set to %u\r\n", static_cast<unsigned>(lab.testRows));
      }
    }
  } else if (cmd.equalsIgnoreCase("pad")) {
    if (tokens.size() < 2) {
      logLine("Usage: pad <decimal>");
    } else {
      uint32_t value = 0;
      if (!parseUint(tokens[1], value)) {
        logLine("pad must be a decimal integer.");
      } else {
        lab.padRows = static_cast<uint16_t>(std::min<uint32_t>(240, value));
        Serial.printf("Pad rows set to %u\r\n", static_cast<unsigned>(lab.padRows));
      }
    }
  } else if (cmd.equalsIgnoreCase("text")) {
    const String text = joinTokens(tokens, 1);
    if (!text.length()) {
      logLine("Usage: text <message>");
    } else {
      runTextCommand(text);
    }
  } else if (cmd.equalsIgnoreCase("compact")) {
    if (tokens.size() < 2) {
      logLine("Usage: compact <checker|bar|box|ink|solidff|solid00>");
    } else {
      runCompactCommand(tokens[1]);
    }
  } else if (cmd.equalsIgnoreCase("combo")) {
    if (tokens.size() < 3) {
      logLine("Usage: combo <paperHex> <locationHex> [calibrate|tinyjob|inkjob|seekprint|seekink|seeksolidff|seeksolid00|seeklegacyff|seeklegacy00|legacyff|legacy00|legacyink|legacyffraw|legacy00raw|legacyinkraw|legacyffhold|legacy00hold|legacyinkhold|legacyffbare|legacy00bare|legacyinkbare|legacyffbarehold|legacy00barehold|legacyinkbarehold]");
    } else {
      std::vector<uint8_t> paperVec;
      std::vector<uint8_t> locationVec;
      if (!parseHexBlob(tokens[1], paperVec) || paperVec.size() != 1) {
        logLine("paperHex must be one byte.");
      } else if (!parseHexBlob(tokens[2], locationVec) || locationVec.size() != 1) {
        logLine("locationHex must be one byte.");
      } else {
        lab.paperValue = paperVec[0];
        lab.locationValue = locationVec[0];
        Serial.printf("Combo set: paper=0x%02X location=0x%02X\r\n", lab.paperValue, lab.locationValue);
        const String action = tokens.size() >= 4 ? tokens[3] : "calibrate";
        sendSequence(action);
      }
    }
  } else if (cmd.equalsIgnoreCase("connect")) {
    connectPrinter();
  } else if (cmd.equalsIgnoreCase("disconnect")) {
    disconnectPrinter();
    logLine("Disconnected.");
  } else if (cmd.equalsIgnoreCase("probe")) {
    printServiceTree();
  } else if (cmd.equalsIgnoreCase("read")) {
    if (tokens.size() < 2) logLine("Usage: read <ff01|ff02|ff03|uuid>");
    else readCharacteristicCommand(tokens[1]);
  } else if (cmd.equalsIgnoreCase("subscribe")) {
    if (tokens.size() < 2) logLine("Usage: subscribe <ff03|uuid>");
    else subscribeCommand(tokens[1], true);
  } else if (cmd.equalsIgnoreCase("unsubscribe")) {
    if (tokens.size() < 2) logLine("Usage: unsubscribe <ff03|uuid>");
    else subscribeCommand(tokens[1], false);
  } else if (cmd.equalsIgnoreCase("write")) {
    handleWrite(tokens);
  } else if (cmd.equalsIgnoreCase("repeat")) {
    handleRepeat(tokens);
  } else if (cmd.equalsIgnoreCase("preset")) {
    if (tokens.size() < 2) logLine("Usage: preset <name>");
    else sendPreset(tokens[1]);
  } else if (cmd.equalsIgnoreCase("sequence")) {
    if (tokens.size() < 2) logLine("Usage: sequence <info|startjob|nudge|calibrate|seekprint|seekink|seeksolidff|seeksolid00|seeklegacyff|seeklegacy00|legacyff|legacy00|legacyink|legacyffraw|legacy00raw|legacyinkraw|legacyffhold|legacy00hold|legacyinkhold|legacyffbare|legacy00bare|legacyinkbare|legacyffbarehold|legacy00barehold|legacyinkbarehold|tinyjob|inkjob>");
    else sendSequence(tokens[1]);
  } else if (cmd.equalsIgnoreCase("sweep11")) {
    handleSweep11(tokens);
  } else if (cmd.equalsIgnoreCase("sweepc0")) {
    handleSweepC0(tokens);
  } else if (cmd.equalsIgnoreCase("sweep12")) {
    handleSweep12(tokens);
  } else if (cmd.equalsIgnoreCase("sweep80")) {
    handleSweep80(tokens);
  } else if (cmd.equalsIgnoreCase("sweep1f")) {
    handleSweep1f(tokens);
  } else if (cmd.equalsIgnoreCase("sweep10ff")) {
    handleSweep10ff(tokens);
  } else if (cmd.equalsIgnoreCase("battery")) {
    if (lab.batteryKnown) {
      Serial.printf("Last battery packet raw=0x%02lX approx=%lu%%\r\n",
                    static_cast<unsigned long>(lab.lastBatteryRaw),
                    static_cast<unsigned long>((lab.lastBatteryRaw * 100UL + 127UL) / 255UL));
    } else {
      logLine("No battery packet seen yet.");
    }
  } else if (cmd.equalsIgnoreCase("state")) {
    Serial.printf("target=%s mode=%s connected=%s ff00=%s ff01=%s ff02=%s ff03=%s subscribed=%s paper=0x%02X location=0x%02X rows=%u pad=%u\r\n",
                  lab.targetMac.c_str(),
                  addrModeName(lab.targetMode).c_str(),
                  (lab.client && lab.client->isConnected()) ? "yes" : "no",
                  lab.ff00 ? "yes" : "no",
                  lab.ff01 ? "yes" : "no",
                  lab.ff02 ? "yes" : "no",
                  lab.ff03 ? "yes" : "no",
                  lab.ff03Subscribed ? "yes" : "no",
                  lab.paperValue,
                  lab.locationValue,
                  static_cast<unsigned>(lab.testRows),
                  static_cast<unsigned>(lab.padRows));
  } else {
    logLine("Unknown command. Type help.");
  }

  prompt();
}

void printBanner() {
  Serial.println();
  Serial.println("mervyn's M100 BLE lab");
  Serial.println("--------------------");
  Serial.println("This build is for focused printer reverse engineering.");
  Serial.println("Use a serial terminal at 115200 baud and type 'help'.");
  Serial.println();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t serialStart = millis();
  while (!Serial && millis() - serialStart < 4000) {
    delay(10);
  }
  delay(250);
  WiFi.mode(WIFI_OFF);
  btStop();
  delay(200);

  printBanner();
  Serial.println("Calling NimBLEDevice::init...");
  NimBLEDevice::init("");
  Serial.println("NimBLE init ok.");

  lab.scan = NimBLEDevice::getScan();
  if (lab.scan) {
    lab.scan->setActiveScan(true);
    lab.scan->setInterval(80);
    lab.scan->setWindow(60);
    lab.scan->setDuplicateFilter(0);
  }

  printHelp();
  prompt();
}

void loop() {
  while (Serial.available() > 0) {
    const char ch = static_cast<char>(Serial.read());
    if (ch == '\r') continue;
    if (ch == '\n') {
      const String line = inputLine;
      inputLine = "";
      handleCommand(line);
      continue;
    }
    if (ch == '\b' || ch == 0x7F) {
      if (inputLine.length() > 0) {
        inputLine.remove(inputLine.length() - 1);
      }
      continue;
    }
    inputLine += ch;
  }
  delay(4);
}
