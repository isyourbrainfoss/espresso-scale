#pragma once

#include <cstdint>

// --- Product ---
static constexpr const char* kProductName = "Flowlog Scale";
static constexpr const char* kFirmwareVersion = "1.7.0";

// BLE still advertises as "Decent Scale" so Flowlog's existing pairing
// (Decent-compatible FFF0/FFF4/36F5) discovers the device without app changes.
static constexpr const char* kBleDeviceName = "Decent Scale";

// Standalone cup-target UI (mirrors Flowlog Live yield bar defaults).
static constexpr float kDefaultTargetYieldG = 36.0f;
// Soft buzzer when cup weight crosses this (wind-back cue on Coffeejack).
static constexpr float kDefaultYieldWarnG = 34.0f;

// OLED pressure bar window (bar). Fill is relative to this range; tick marks
// every 1 bar between min and max.
static constexpr float kPressureBarMinBar = 5.0f;
static constexpr float kPressureBarMaxBar = 10.0f;

// Phone-free brew: confirm prompt times out (no accidental start while
// weighing beans). Pressure auto-start is disabled — brew is intentional.
static constexpr uint32_t kBrewConfirmTimeoutMs = 10000;
// Clear phone-forwarded pressure if no update within this window.
static constexpr uint32_t kPhonePressureStaleMs = 2500;

// WiFi / OTA
static constexpr const char* kHostname = "half-decent";
// Setup AP SSID is "<prefix>-XXXX" (last two MAC bytes). There are no
// compiled-in passwords: the setup AP, web UI and ArduinoOTA all use a
// per-device random password generated on first boot and stored in NVS
// (see src/device_auth.*). It is shown on the OLED.
static constexpr const char* kApSsidPrefix = "HalfDecent";
// Setup AP closes after this long with no connected client / HTTP request.
static constexpr uint32_t kSetupApIdleTimeoutMs = 10UL * 60UL * 1000UL;
// How long the OLED shows the setup / password screen once triggered.
static constexpr uint32_t kSetupScreenMs = 60UL * 1000UL;
// Web UI brute-force throttle.
static constexpr uint8_t kAuthMaxFailures = 5;
static constexpr uint32_t kAuthLockoutMs = 60000;
// Join only after a short directed scan sees the saved SSID. No AP fallback.
static constexpr uint32_t kWifiConnectTimeoutMs = 12000;
static constexpr uint32_t kWifiScanMsPerChan = 80;
// Cold boot: hold Timer for this long after power-on to open the setup
// hotspot. Sampled from loop() while the weight is already live.
static constexpr uint32_t kWifiSetupHoldMs = 1200;
// Radios start after the boot zero (RF bursts add HX711 noise) or at the
// latest this long after boot. BLE comes up once the Wi-Fi scan is over.
static constexpr uint32_t kRadioStartMaxDelayMs = 2500;
// If STA drops (left the house), turn WiFi off instead of reconnecting forever.
static constexpr uint32_t kWifiStaGiveUpMs = 12000;

// --- Sampling / UI ---
static constexpr uint32_t kWeightNotifyHz = 10;
static constexpr uint32_t kWeightNotifyIntervalMs = 1000 / kWeightNotifyHz;
static constexpr uint32_t kDisplayRefreshMs = 100;
static constexpr uint32_t kButtonPollMs = 10;
// Sample recorder interval (matches weight notify rate).
static constexpr uint32_t kShotSampleIntervalMs = kWeightNotifyIntervalMs;

// OLED weight smoothing (BLE stays closer to raw readings).
static constexpr float kOledFilterAlpha = 0.35f;

// Flow rate: simple window derivative over this many samples at notify rate.
static constexpr int kFlowRateWindowSamples = 8;  // ~0.8 s at 10 Hz

// Auto shot-timer on the scale is off — Flowlog owns brew timing.
static constexpr bool kAutoTimerEnabled = false;
static constexpr float kAutoTimerThresholdG = 0.5f;
static constexpr float kAutoTimerNearZeroG = 0.3f;

// Buttons
static constexpr uint32_t kBtnDebounceMs = 30;
static constexpr uint32_t kBtnLongPressMs = 1000;
static constexpr uint32_t kBtnBothHoldCalMs = 3000;
// Hold Timer alone this long: show device password / open Wi-Fi setup AP.
static constexpr uint32_t kBtnSetupHoldMs = 5000;

// Zero / tare (see Scale). HX711 output settles ~400 ms after power-up.
static constexpr uint32_t kHx711SettleMs = 400;
// A tare averages one fresh sample window; it waits for a still window
// (spread below this) but never longer than kTareMaxWaitMs.
static constexpr float kTareStableSpreadG = 1.0f;
static constexpr uint32_t kTareMaxWaitMs = 1500;
// Boot: platform heavier than the stored empty zero by at least this much
// means something sits on it, so keep the empty zero instead of taring.
static constexpr float kBootZeroLoadG = 40.0f;
// Rewrite the stored empty zero only when it moved by more than this.
static constexpr float kEmptyZeroRewriteG = 0.5f;

// Calibration
static constexpr float kDefaultCalMassG = 100.0f;
// Fallback factor if NVS empty (raw counts per gram). Tune after first cal.
static constexpr float kDefaultCalFactor = 1000.0f;

// Heartbeat: disconnect only if client opted in and goes silent.
// 15 s gives Flowlog headroom (app sends every 2 s) and avoids mid-brew drops.
static constexpr uint32_t kHeartbeatTimeoutMs = 15000;

// Deep sleep (long-press Tare): ~µA–mA class draw; wake on Tare or Timer touch.
// Soft OLED-only standby is no longer used.

// Battery sense (only if PIN_BAT_ADC >= 0)
static constexpr float kBatVoltageDivider = 2.0f;  // Vbat = Vadc * this
static constexpr float kBatUsbThresholdV = 4.35f;  // above → treat as USB

// Serial
static constexpr uint32_t kSerialBaud = 115200;
