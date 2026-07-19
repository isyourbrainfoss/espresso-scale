#pragma once

#include <cstdint>

// --- Product ---
static constexpr const char* kProductName = "Half Decent Scale";
static constexpr const char* kFirmwareVersion = "1.1.0";

// BLE advertises as "Decent Scale" so Flowlog and other Decent-compatible
// apps discover the device without changes.
static constexpr const char* kBleDeviceName = "Decent Scale";

// WiFi / OTA
static constexpr const char* kHostname = "half-decent";
static constexpr const char* kApSsid = "HalfDecent-Setup";
static constexpr const char* kApPassword = "scale1234";  // min 8 chars
// Optional password for ArduinoOTA / web OTA (empty = open on LAN)
static constexpr const char* kOtaPassword = "scaleota";
static constexpr uint32_t kWifiConnectTimeoutMs = 15000;

// --- Sampling / UI ---
static constexpr uint32_t kWeightNotifyHz = 10;
static constexpr uint32_t kWeightNotifyIntervalMs = 1000 / kWeightNotifyHz;
static constexpr uint32_t kDisplayRefreshMs = 100;
static constexpr uint32_t kButtonPollMs = 10;

// OLED weight smoothing (BLE stays closer to raw readings).
static constexpr float kOledFilterAlpha = 0.35f;

// Flow rate: simple window derivative over this many samples at notify rate.
static constexpr int kFlowRateWindowSamples = 8;  // ~0.8 s at 10 Hz

// Auto-start shot timer when weight rises after a stable near-zero period.
static constexpr bool kAutoTimerEnabled = true;
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

// Half Decent heartbeat: disconnect if opted-in client misses this window.
static constexpr uint32_t kHeartbeatTimeoutMs = 5000;

// Serial
static constexpr uint32_t kSerialBaud = 115200;
