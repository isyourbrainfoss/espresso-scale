#pragma once

#include <cstdint>

// --- Product ---
static constexpr const char* kProductName = "Flowlog Scale";
static constexpr const char* kFirmwareVersion = "1.4.2";

// BLE still advertises as "Decent Scale" so Flowlog's existing pairing
// (Decent-compatible FFF0/FFF4/36F5) discovers the device without app changes.
static constexpr const char* kBleDeviceName = "Decent Scale";

// Standalone cup-target UI (mirrors Flowlog Live yield bar defaults).
static constexpr float kDefaultTargetYieldG = 36.0f;
static constexpr float kDefaultYieldWarnG = 32.0f;

// WiFi / OTA
static constexpr const char* kHostname = "half-decent";
static constexpr const char* kApSsid = "HalfDecent-Setup";
static constexpr const char* kApPassword = "scale1234";  // min 8 chars
// Optional password for ArduinoOTA / web OTA (empty = open on LAN)
static constexpr const char* kOtaPassword = "scaleota";
static constexpr uint32_t kWifiConnectTimeoutMs = 25000;

// --- Sampling / UI ---
static constexpr uint32_t kWeightNotifyHz = 10;
static constexpr uint32_t kWeightNotifyIntervalMs = 1000 / kWeightNotifyHz;
static constexpr uint32_t kDisplayRefreshMs = 100;
static constexpr uint32_t kButtonPollMs = 10;

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
