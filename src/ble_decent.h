#pragma once

#include <Arduino.h>
#include <atomic>
#include <functional>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

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
    Heartbeat,  // never queued: handled in the BLE callback (see onWrite)
  };
  Type type = Type::None;
  bool heartbeat_aware = false;  // byte5 == 0x01 on tare/LED
};

class BleDecent {
 public:
  using CommandHandler = std::function<void(const DecentCommand&)>;

  bool begin(CommandHandler on_command);
  // Stop BLE before deep sleep. Waits briefly so already-queued notifies
  // (e.g. the PowerOff ack) reach the client before the stack is torn down.
  void end();
  // Call from loop(): runs heartbeat checks and dispatches queued commands.
  // Heartbeats are recorded directly in the BLE callback and never queued, so
  // a blocked loop cannot let them crowd tare/timer commands out of the queue.
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
  // Writes arrive on the NimBLE host task; commands are queued there and
  // run on the Arduino loop task so tare (HX711), buzzer and deep sleep
  // (which deinits NimBLE) never execute inside a BLE callback.
  static constexpr UBaseType_t kCmdQueueDepth = 16;
  QueueHandle_t cmd_queue_ = nullptr;
  // Written from the NimBLE host task (callbacks) and read/written from the
  // Arduino loop task, so they must be atomic.
  std::atomic<bool> connected_{false};
  std::atomic<bool> advertising_{false};
  std::atomic<bool> app_mode_{false};
  std::atomic<bool> display_on_{true};
  std::atomic<bool> notify_enabled_{false};
  std::atomic<bool> heartbeat_required_{false};
  std::atomic<uint32_t> last_heartbeat_ms_{0};
  std::atomic<bool> stopping_{false};  // end() in progress: don't re-advertise

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
