#pragma once

#include <Arduino.h>
#include <functional>

// Decent Scale BLE peripheral — Flowlog-compatible.
// Device name: "Decent Scale"
// Notify FFF4, write 36F5.

struct DecentCommand {
  enum class Type : uint8_t {
    None = 0,
    Tare,
    LedOn,
    LedOff,
    PowerOff,
    TimerStart,
    TimerStop,
    TimerReset,
    Heartbeat,
  };
  Type type = Type::None;
  bool heartbeat_aware = false;  // byte5 == 0x01 on tare/LED
};

class BleDecent {
 public:
  using CommandHandler = std::function<void(const DecentCommand&)>;

  bool begin(CommandHandler on_command);
  void update();

  bool isConnected() const { return connected_; }
  bool isAdvertising() const { return advertising_; }
  bool appMode() const { return app_mode_; }  // weight stream enabled
  bool displayEnabled() const { return display_on_; }

  // Call when local UI wants to show APP / enable stream (e.g. after LED on).
  void setAppMode(bool on) { app_mode_ = on; }

  // Notify weight at ~10 Hz when app_mode and a client is connected with CCCD.
  // weight_g: grams; is_stable: CE vs CA; timer fields for 10-byte packet.
  void notifyWeight(float weight_g, bool is_stable, uint8_t minutes,
                    uint8_t seconds, uint8_t deciseconds);

  // Button event: button 1=O/tare, 2=square/timer; press 1=short 2=long
  void notifyButton(uint8_t button, uint8_t press);

  // Acks
  void notifyTareAck(uint8_t counter = 0);
  // battery_byte: 0xFF = USB, else 3–100 percent
  void notifyLedAck(uint8_t battery_byte = 0xFF);

  // Heartbeat enforcement after client opts in via tare/LED byte5=1
  bool heartbeatRequired() const { return heartbeat_required_; }
  void noteHeartbeat();

 private:
  CommandHandler on_command_;
  bool connected_ = false;
  bool advertising_ = false;
  bool app_mode_ = false;
  bool display_on_ = true;
  bool notify_enabled_ = false;
  bool heartbeat_required_ = false;
  uint32_t last_heartbeat_ms_ = 0;

  friend class DecentServerCallbacks;
  friend class DecentCharCallbacks;

  void onConnect();
  void onDisconnect();
  void onWrite(const uint8_t* data, size_t len);
  void onNotifySubscribed(bool subscribed);
  void startAdvertising();
  bool sendNotify(const uint8_t* data, size_t len);
  static uint8_t xor6(const uint8_t* b);
};
