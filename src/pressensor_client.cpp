#include "pressensor_client.h"

#include <NimBLEDevice.h>
#include <cmath>

#include "config.h"

namespace {

// Match Flowlog packages/flowlog_sensors pressensor_protocol.dart
const char* kPressureService = "873ae82a-4c5a-4342-b539-9d900bf7ebd0";
const char* kPressureChar = "873ae82b-4c5a-4342-b539-9d900bf7ebd0";

PressensorClient* g_prs = nullptr;
NimBLEClient* g_client = nullptr;
NimBLERemoteCharacteristic* g_pressure_char = nullptr;
NimBLEAddress g_target_addr;
bool g_have_target = false;

int16_t be16(const uint8_t* p) {
  return static_cast<int16_t>((p[0] << 8) | p[1]);
}

void notifyCb(NimBLERemoteCharacteristic* /*c*/, uint8_t* data, size_t len,
              bool /*isNotify*/) {
  if (g_prs && data && len >= 2) {
    g_prs->onNotify(data, len);
  }
}

class PrsClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient* /*c*/) override {
    Serial.println("[prs] disconnected");
    g_pressure_char = nullptr;
    if (g_prs) g_prs->onLinkLost();
  }
};

class PrsScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* advertised) override {
    if (!advertised->haveName()) return;
    std::string name = advertised->getName();
    if (name.size() < 3) return;
    if (name[0] != 'P' || name[1] != 'R' || name[2] != 'S') return;
    Serial.printf("[prs] found %s %s\n", name.c_str(),
                  advertised->getAddress().toString().c_str());
    g_target_addr = advertised->getAddress();
    g_have_target = true;
    NimBLEDevice::getScan()->stop();
  }
};

bool connectToTarget() {
  if (!g_have_target) return false;
  if (!g_client) {
    g_client = NimBLEDevice::createClient();
    g_client->setClientCallbacks(new PrsClientCallbacks(), false);
    g_client->setConnectionParams(12, 24, 0, 200);
    g_client->setConnectTimeout(8);
  }
  if (g_client->isConnected()) {
    g_client->disconnect();
    delay(100);
  }
  Serial.printf("[prs] connecting %s\n", g_target_addr.toString().c_str());
  if (!g_client->connect(g_target_addr)) {
    Serial.println("[prs] connect failed");
    return false;
  }
  NimBLERemoteService* svc = g_client->getService(kPressureService);
  if (!svc) {
    Serial.println("[prs] pressure service missing");
    g_client->disconnect();
    return false;
  }
  g_pressure_char = svc->getCharacteristic(kPressureChar);
  if (!g_pressure_char || !g_pressure_char->canNotify()) {
    Serial.println("[prs] pressure char missing/notify");
    g_client->disconnect();
    return false;
  }
  if (!g_pressure_char->subscribe(true, notifyCb)) {
    Serial.println("[prs] subscribe failed");
    g_client->disconnect();
    return false;
  }
  Serial.println("[prs] connected + subscribed");
  return true;
}

}  // namespace

bool PressensorClient::begin() {
  g_prs = this;
  // Server (Decent peripheral) is started first by BleDecent::begin.
  // Client uses the same NimBLE stack.
  return true;
}

void PressensorClient::end() {
  disconnect();
  stopScan();
  g_prs = nullptr;
}

void PressensorClient::requestConnect() {
  connect_requested_ = true;
  last_connect_try_ms_ = 0;  // try soon
}

void PressensorClient::disconnect() {
  connect_requested_ = false;
  stopScan();
  if (g_client && g_client->isConnected()) {
    g_client->disconnect();
  }
  g_pressure_char = nullptr;
  connected_ = false;
  has_reading_ = false;
}

void PressensorClient::onLinkLost() {
  connected_ = false;
  g_pressure_char = nullptr;
}

void PressensorClient::onNotify(const uint8_t* data, size_t len) {
  if (len < 2) return;
  const int16_t mbar = be16(data);
  pressure_bar_ = mbar / 1000.0f;
  has_reading_ = true;
  if (len >= 4) {
    temp_c_ = be16(data + 2) / 10.0f;
    has_temp_ = true;
  }
}

bool PressensorClient::startScan() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (!scan) return false;
  g_have_target = false;
  scan->setAdvertisedDeviceCallbacks(new PrsScanCallbacks(), false);
  scan->setActiveScan(true);
  scan->setInterval(80);
  scan->setWindow(40);
  scanning_ = true;
  // Async scan 4 seconds
  scan->start(4, [](NimBLEScanResults) {
    if (g_prs) g_prs->setScanning(false);
  }, false);
  Serial.println("[prs] scanning for PRS* …");
  return true;
}

void PressensorClient::stopScan() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan && scan->isScanning()) {
    scan->stop();
  }
  scanning_ = false;
}

void PressensorClient::update() {
  if (!connect_requested_) return;
  if (connected_ || scanning_) return;

  const uint32_t now = millis();
  if (now - last_connect_try_ms_ < 8000) return;
  last_connect_try_ms_ = now;

  if (g_have_target) {
    if (connectToTarget()) {
      connected_ = true;
      connect_requested_ = false;
    } else {
      g_have_target = false;
      startScan();
    }
    return;
  }
  startScan();
}
