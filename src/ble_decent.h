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
    // Flowlog extensions (type 0xF0+)
    PhonePressure,   // phone → scale pressure mirror during app brew
    PhoneBrewStart,
    PhoneBrewEnd,
    ScaleDisplayConfig,  // target/warn g + pressure bar window
    ShotExport,          // request list / transfer / status over BLE
  };
  Type type = Type::None;
  bool heartbeat_aware = false;  // byte5 == 0x01 on tare/LED
  // PhonePressure payload
  int16_t pressure_mbar = 0;
  // ScaleDisplayConfig payload (uint8 grams / bar)
  uint8_t cfg_target_g = 36;
  uint8_t cfg_warn_g = 34;
  uint8_t cfg_p_min = 5;
  uint8_t cfg_p_max = 10;
  // ShotExport: opcode in d0, slot/age in d1
  //   0 = list metadata, 1 = transfer shot by age, 2 = status (IP)
  uint8_t shot_opcode = 0;
  uint8_t shot_slot = 0;
};

class BleDecent {
 public:
  using CommandHandler = std::function<void(const DecentCommand&)>;

  bool begin(CommandHandler on_command);
  void end();  // stop BLE before deep sleep
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

  // --- Shot export over FFF4 (type 0xF5 chunks / 0xF4 list / 0xF6 status) ---
  // Payload format (variable length up to negotiated MTU):
  //   list:   03 F4 count s0lo s0hi s1lo s1hi s2lo s2hi
  //   chunk:  03 F5 slot flags seq_lo seq_hi total_lo total_hi data...
  //           flags bit0 = last chunk
  //   status: 03 F6 ...ascii IP...
  bool notifyShotList(const uint16_t sizes[3], uint8_t count);
  bool notifyShotStatus(const char* ip_or_status);
  bool notifyShotChunk(uint8_t slot, uint16_t seq, uint16_t total_chunks,
                       const uint8_t* data, size_t len, bool last);
  bool isTransferBusy() const { return xfer_active_; }
  // Feed transfer from main loop: call with full JSON once, then pump().
  bool beginShotTransfer(uint8_t slot, const String& json);
  void pumpShotTransfer();  // send next chunk(s); safe every loop
  void cancelShotTransfer();

 private:
  // Active BLE shot transfer (JSON streamed as 0xF5 notifies).
  bool xfer_active_ = false;
  uint8_t xfer_slot_ = 0;
  String xfer_json_;
  size_t xfer_offset_ = 0;
  uint16_t xfer_seq_ = 0;
  uint16_t xfer_total_ = 0;
  uint32_t xfer_last_ms_ = 0;
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
