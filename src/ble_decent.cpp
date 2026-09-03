#include "ble_decent.h"

#include <NimBLEDevice.h>
#include <vector>

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

void BleDecent::end() {
  if (server) {
    NimBLEDevice::getAdvertising()->stop();
  }
  NimBLEDevice::deinit(true);
  connected_ = false;
  advertising_ = false;
  app_mode_ = false;
  notify_enabled_ = false;
  g_ble = nullptr;
  server = nullptr;
  notify_char = nullptr;
  write_char = nullptr;
  Serial.println("[ble] stopped");
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
  // Runs on the NimBLE host task. Parse + enqueue only — no Serial, notify,
  // NVS, or client disconnect from this path (those reboot ESP32-S3 + CDC).
  if (len < 7) {
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
      noteHeartbeat();
      return; // timestamp only — do not enqueue a no-op onto the command queue
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
  } else if (type == 0xF0) {
    // Flowlog → scale: live pressure (mbar big-endian in d0,d1)
    cmd.type = DecentCommand::Type::PhonePressure;
    cmd.pressure_mbar = static_cast<int16_t>((d0 << 8) | d1);
  } else if (type == 0xF1) {
    cmd.type = DecentCommand::Type::PhoneBrewStart;
  } else if (type == 0xF2) {
    cmd.type = DecentCommand::Type::PhoneBrewEnd;
  } else if (type == 0xF3) {
    // Flowlog → scale display config:
    // d0 = target yield g, d1 = warn g, d2 = P min bar, d3 = P max bar
    cmd.type = DecentCommand::Type::ScaleDisplayConfig;
    cmd.cfg_target_g = d0;
    cmd.cfg_warn_g = d1;
    cmd.cfg_p_min = d2;
    cmd.cfg_p_max = d3;
  } else if (type == 0xF4) {
    // Shot export: d0=opcode (0=list,1=get,2=status), d1=slot/age
    cmd.type = DecentCommand::Type::ShotExport;
    cmd.shot_opcode = d0;
    cmd.shot_slot = d1;
  } else {
    return;
  }

  if (cmd.type != DecentCommand::Type::None) {
    enqueueCommand(cmd);
  }
}

void BleDecent::enqueueCommand(const DecentCommand& cmd) {
  portENTER_CRITICAL(&cmd_mux_);
  // Coalesce live pressure so a 1 Hz stream cannot fill the queue.
  if (cmd.type == DecentCommand::Type::PhonePressure && cmd_count_ > 0) {
    for (uint8_t i = 0; i < cmd_count_; i++) {
      const uint8_t idx = (cmd_head_ + i) % kCmdQueue;
      if (cmd_queue_[idx].type == DecentCommand::Type::PhonePressure) {
        cmd_queue_[idx] = cmd;
        portEXIT_CRITICAL(&cmd_mux_);
        return;
      }
    }
  }
  // Drop duplicate LED-on / heartbeat — brew-start bursts send extras.
  if (cmd.type == DecentCommand::Type::Heartbeat ||
      cmd.type == DecentCommand::Type::LedOn) {
    for (uint8_t i = 0; i < cmd_count_; i++) {
      const uint8_t idx = (cmd_head_ + i) % kCmdQueue;
      if (cmd_queue_[idx].type == cmd.type) {
        portEXIT_CRITICAL(&cmd_mux_);
        return;
      }
    }
  }
  if (cmd_count_ >= kCmdQueue) {
    portEXIT_CRITICAL(&cmd_mux_);
    return;
  }
  cmd_queue_[cmd_tail_] = cmd;
  cmd_tail_ = (cmd_tail_ + 1) % kCmdQueue;
  cmd_count_++;
  portEXIT_CRITICAL(&cmd_mux_);
}

void BleDecent::pumpCommands() {
  // One or two per loop() so NVS / PRS disconnect cannot stall the OLED.
  for (int n = 0; n < 2; n++) {
    DecentCommand cmd;
    portENTER_CRITICAL(&cmd_mux_);
    if (cmd_count_ == 0) {
      portEXIT_CRITICAL(&cmd_mux_);
      return;
    }
    cmd = cmd_queue_[cmd_head_];
    cmd_head_ = (cmd_head_ + 1) % kCmdQueue;
    cmd_count_--;
    portEXIT_CRITICAL(&cmd_mux_);
    if (on_command_ && cmd.type != DecentCommand::Type::None) {
      on_command_(cmd);
    }
  }
}

void BleDecent::disconnectPeer() {
  if (!server) return;
  const std::vector<uint16_t> peers = server->getPeerDevices();
  for (uint16_t handle : peers) {
    server->disconnect(handle);
  }
}

void BleDecent::noteHeartbeat() {
  last_heartbeat_ms_ = millis();
}

void BleDecent::update() {
  pumpCommands();
  if (connected_ && heartbeat_required_) {
    if (millis() - last_heartbeat_ms_ > kHeartbeatTimeoutMs) {
      Serial.println("[ble] heartbeat timeout — disconnect");
      disconnectPeer();
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

bool BleDecent::notifyShotList(const uint16_t sizes[3], uint8_t count) {
  uint8_t pkt[10];
  pkt[0] = 0x03;
  pkt[1] = 0xF4;
  pkt[2] = count > 3 ? 3 : count;
  for (int i = 0; i < 3; i++) {
    pkt[3 + i * 2] = static_cast<uint8_t>(sizes[i] & 0xFF);
    pkt[4 + i * 2] = static_cast<uint8_t>((sizes[i] >> 8) & 0xFF);
  }
  return sendNotify(pkt, sizeof(pkt));
}

bool BleDecent::notifyShotStatus(const char* ip_or_status) {
  if (!ip_or_status) ip_or_status = "";
  size_t n = strlen(ip_or_status);
  if (n > 16) n = 16;
  uint8_t pkt[20];
  pkt[0] = 0x03;
  pkt[1] = 0xF6;
  pkt[2] = static_cast<uint8_t>(n);
  for (size_t i = 0; i < n; i++) {
    pkt[3 + i] = static_cast<uint8_t>(ip_or_status[i]);
  }
  return sendNotify(pkt, 3 + n);
}

bool BleDecent::notifyShotChunk(uint8_t slot, uint16_t seq, uint16_t total_chunks,
                                const uint8_t* data, size_t len, bool last) {
  // Header 8 bytes + payload (keep under ~20 for default ATT MTU).
  constexpr size_t kHdr = 8;
  constexpr size_t kMaxPayload = 12;
  if (len > kMaxPayload) len = kMaxPayload;
  uint8_t pkt[kHdr + kMaxPayload];
  pkt[0] = 0x03;
  pkt[1] = 0xF5;
  pkt[2] = slot;
  pkt[3] = last ? 0x01 : 0x00;
  pkt[4] = static_cast<uint8_t>(seq & 0xFF);
  pkt[5] = static_cast<uint8_t>((seq >> 8) & 0xFF);
  pkt[6] = static_cast<uint8_t>(total_chunks & 0xFF);
  pkt[7] = static_cast<uint8_t>((total_chunks >> 8) & 0xFF);
  for (size_t i = 0; i < len; i++) {
    pkt[kHdr + i] = data[i];
  }
  return sendNotify(pkt, kHdr + len);
}

bool BleDecent::beginShotTransfer(uint8_t slot, const String& json) {
  if (json.isEmpty() || !connected_) return false;
  cancelShotTransfer();
  xfer_json_ = json;
  xfer_slot_ = slot;
  xfer_offset_ = 0;
  xfer_seq_ = 0;
  constexpr size_t kPayload = 12;
  xfer_total_ = static_cast<uint16_t>((json.length() + kPayload - 1) / kPayload);
  if (xfer_total_ == 0) xfer_total_ = 1;
  xfer_active_ = true;
  xfer_last_ms_ = 0;
  Serial.printf("[ble] shot xfer start slot=%u bytes=%u chunks=%u\n",
                static_cast<unsigned>(slot),
                static_cast<unsigned>(json.length()),
                static_cast<unsigned>(xfer_total_));
  return true;
}

void BleDecent::cancelShotTransfer() {
  xfer_active_ = false;
  xfer_json_ = String();
  xfer_offset_ = 0;
  xfer_seq_ = 0;
  xfer_total_ = 0;
}

void BleDecent::pumpShotTransfer() {
  if (!xfer_active_ || !connected_) {
    if (xfer_active_ && !connected_) cancelShotTransfer();
    return;
  }
  // Pace chunks so we don't flood the phone (~40 notifies/s).
  const uint32_t now = millis();
  if (now - xfer_last_ms_ < 25) return;
  xfer_last_ms_ = now;

  constexpr size_t kPayload = 12;
  size_t remaining = xfer_json_.length() - xfer_offset_;
  size_t n = remaining > kPayload ? kPayload : remaining;
  const auto* data =
      reinterpret_cast<const uint8_t*>(xfer_json_.c_str() + xfer_offset_);
  bool last = (xfer_offset_ + n) >= xfer_json_.length();
  if (!notifyShotChunk(xfer_slot_, xfer_seq_, xfer_total_, data, n, last)) {
    Serial.println("[ble] shot chunk notify failed — abort");
    cancelShotTransfer();
    return;
  }
  xfer_offset_ += n;
  xfer_seq_++;
  if (last) {
    Serial.println("[ble] shot xfer complete");
    cancelShotTransfer();
  }
}

void BleDecent::notifyWeight(float weight_g, bool is_stable, uint8_t minutes,
                             uint8_t seconds, uint8_t deciseconds) {
  // Stream whenever a BLE client is connected. Flowlog is the primary host;
  // requiring app_mode (LED-on) caused silent weight freezes when the app
  // stayed "connected" after heartbeat/LED state drifted.
  if (!connected_ || !notify_enabled_) return;

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
