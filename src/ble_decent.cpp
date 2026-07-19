#include "ble_decent.h"

#include <NimBLEDevice.h>

#include "config.h"

namespace {
// UUIDs match Flowlog / Decent Scale API (case-insensitive in NimBLE).
constexpr const char* kServiceUuid = "0000fff0-0000-1000-8000-00805f9b34fb";
constexpr const char* kNotifyUuid = "0000fff4-0000-1000-8000-00805f9b34fb";
constexpr const char* kWriteUuid = "000036f5-0000-1000-8000-00805f9b34fb";

NimBLEServer* server = nullptr;
NimBLECharacteristic* notify_char = nullptr;
NimBLECharacteristic* write_char = nullptr;
BleDecent* g_ble = nullptr;
}  // namespace

class DecentServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* /*s*/) override {
    if (g_ble) g_ble->onConnect();
  }
  void onDisconnect(NimBLEServer* /*s*/) override {
    if (g_ble) g_ble->onDisconnect();
  }
};

class DecentCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    if (!g_ble) return;
    std::string v = c->getValue();
    if (v.empty()) return;
    g_ble->onWrite(reinterpret_cast<const uint8_t*>(v.data()), v.size());
  }
};

uint8_t BleDecent::xor6(const uint8_t* b) {
  return b[0] ^ b[1] ^ b[2] ^ b[3] ^ b[4] ^ b[5];
}

bool BleDecent::begin(CommandHandler on_command) {
  on_command_ = std::move(on_command);
  g_ble = this;

  NimBLEDevice::init(kBleDeviceName);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  server = NimBLEDevice::createServer();
  server->setCallbacks(new DecentServerCallbacks());

  // FFF0 service with FFF4 (notify) + 36F5 (write) — matches Flowlog docs.
  NimBLEService* svc = server->createService(kServiceUuid);

  notify_char = svc->createCharacteristic(
      kNotifyUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  notify_char->setCallbacks(new DecentCharCallbacks());

  write_char = svc->createCharacteristic(
      kWriteUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  write_char->setCallbacks(new DecentCharCallbacks());

  svc->start();
  startAdvertising();
  Serial.printf("[ble] advertising as \"%s\"\n", kBleDeviceName);
  return true;
}

void BleDecent::startAdvertising() {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->reset();
  adv->setName(kBleDeviceName);
  adv->addServiceUUID(kServiceUuid);
  adv->addServiceUUID(kNotifyUuid);
  adv->setScanResponse(true);
  adv->start();
  advertising_ = true;
}

void BleDecent::onConnect() {
  connected_ = true;
  advertising_ = false;
  // NimBLE 1.4 has no onSubscribe callback; allow notify while connected.
  // Client CCCD still gates delivery; app_mode gates when we emit weights.
  notify_enabled_ = true;
  last_heartbeat_ms_ = millis();
  Serial.println("[ble] connected");
}

void BleDecent::onDisconnect() {
  connected_ = false;
  app_mode_ = false;
  notify_enabled_ = false;
  heartbeat_required_ = false;
  Serial.println("[ble] disconnected — re-advertising");
  startAdvertising();
}

void BleDecent::onNotifySubscribed(bool subscribed) {
  notify_enabled_ = subscribed;
}

void BleDecent::onWrite(const uint8_t* data, size_t len) {
  if (len < 7) {
    Serial.printf("[ble] short write len=%u\n", static_cast<unsigned>(len));
    return;
  }
  if (data[0] != 0x03) return;

  const uint8_t type = data[1];
  const uint8_t d0 = data[2];
  const uint8_t d1 = data[3];
  const uint8_t d2 = data[4];
  const uint8_t d3 = data[5];

  DecentCommand cmd;
  cmd.heartbeat_aware = (d3 == 0x01);

  if (type == 0x0F) {
    cmd.type = DecentCommand::Type::Tare;
    if (cmd.heartbeat_aware) {
      heartbeat_required_ = true;
      last_heartbeat_ms_ = millis();
    }
  } else if (type == 0x0A) {
    if (d0 == 0x03 && d1 == 0xFF && d2 == 0xFF) {
      cmd.type = DecentCommand::Type::Heartbeat;
      noteHeartbeat();
    } else if (d0 == 0x02) {
      cmd.type = DecentCommand::Type::PowerOff;
    } else if (d0 == 0x00) {
      cmd.type = DecentCommand::Type::LedOff;
      display_on_ = false;
      // LED off still may keep stream; Flowlog uses LED on to start.
    } else if (d0 == 0x01) {
      cmd.type = DecentCommand::Type::LedOn;
      display_on_ = true;
      app_mode_ = true;
      if (cmd.heartbeat_aware) {
        heartbeat_required_ = true;
        last_heartbeat_ms_ = millis();
      }
    } else {
      return;
    }
  } else if (type == 0x0B) {
    if (d0 == 0x03)
      cmd.type = DecentCommand::Type::TimerStart;
    else if (d0 == 0x00)
      cmd.type = DecentCommand::Type::TimerStop;
    else if (d0 == 0x02)
      cmd.type = DecentCommand::Type::TimerReset;
    else
      return;
  } else {
    return;
  }

  if (on_command_ && cmd.type != DecentCommand::Type::None) {
    on_command_(cmd);
  }
}

void BleDecent::noteHeartbeat() {
  last_heartbeat_ms_ = millis();
}

void BleDecent::update() {
  if (connected_ && heartbeat_required_) {
    if (millis() - last_heartbeat_ms_ > kHeartbeatTimeoutMs) {
      Serial.println("[ble] heartbeat timeout — disconnect");
      if (server) server->disconnect(0);
      heartbeat_required_ = false;
    }
  }
}

bool BleDecent::sendNotify(const uint8_t* data, size_t len) {
  if (!connected_ || !notify_char) return false;
  notify_char->setValue(data, len);
  notify_char->notify(true);  // only to clients that subscribed
  return true;
}

void BleDecent::notifyWeight(float weight_g, bool is_stable, uint8_t minutes,
                             uint8_t seconds, uint8_t deciseconds) {
  // Only stream after app has started the session (LED on / tare with stream).
  if (!app_mode_) return;

  int16_t w10 = static_cast<int16_t>(lroundf(weight_g * 10.0f));
  // Clamp to int16
  if (weight_g * 10.0f > 32767.0f) w10 = 32767;
  if (weight_g * 10.0f < -32768.0f) w10 = -32768;

  uint8_t pkt[10];
  pkt[0] = 0x03;
  pkt[1] = is_stable ? 0xCE : 0xCA;
  pkt[2] = static_cast<uint8_t>((w10 >> 8) & 0xFF);
  pkt[3] = static_cast<uint8_t>(w10 & 0xFF);
  pkt[4] = minutes;
  pkt[5] = seconds;
  pkt[6] = deciseconds;
  pkt[7] = 0x00;
  pkt[8] = 0x00;
  // XOR of first 6 bytes per classic Decent docs; HDS deprecates but we send.
  pkt[9] = xor6(pkt);

  sendNotify(pkt, sizeof(pkt));
}

void BleDecent::notifyButton(uint8_t button, uint8_t press) {
  uint8_t pkt[7] = {0x03, 0xAA, button, press, 0x00, 0x00, 0x00};
  pkt[6] = xor6(pkt);
  sendNotify(pkt, sizeof(pkt));
}

void BleDecent::notifyTareAck(uint8_t counter) {
  uint8_t pkt[7] = {0x03, 0x0F, counter, 0x00, 0x00, 0xFE, 0x00};
  pkt[6] = xor6(pkt);
  sendNotify(pkt, sizeof(pkt));
}

void BleDecent::notifyLedAck(uint8_t battery_byte) {
  // firmware 03 = v1.2 packet format
  uint8_t pkt[7] = {0x03, 0x0A, 0x00, 0x00, battery_byte, 0x03, 0x00};
  pkt[6] = xor6(pkt);
  sendNotify(pkt, sizeof(pkt));
}
