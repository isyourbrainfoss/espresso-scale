#pragma once

#include <cstdint>

// --- Product ---
static constexpr const char* kProductName = "Half Decent Scale";
static constexpr const char* kFirmwareVersion = "1.3.0";

// BLE advertises as "Decent Scale" so Flowlog and other Decent-compatible
// apps discover the device without changes.
static constexpr const char* kBleDeviceName = "Decent Scale";

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
// Background STA retry period when the saved network is unreachable
// (the setup AP is NOT opened automatically on Wi-Fi failure).
static constexpr uint32_t kWifiRetryIntervalMs = 30000;
// Web UI brute-force throttle.
static constexpr uint8_t kAuthMaxFailures = 5;
static constexpr uint32_t kAuthLockoutMs = 60000;
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

// Auto-start shot timer when weight rises after a stable near-zero period.
static constexpr bool kAutoTimerEnabled = true;
static constexpr float kAutoTimerThresholdG = 0.5f;
static constexpr float kAutoTimerNearZeroG = 0.3f;

// Buttons
static constexpr uint32_t kBtnDebounceMs = 30;
static constexpr uint32_t kBtnLongPressMs = 1000;
static constexpr uint32_t kBtnBothHoldCalMs = 3000;
// Hold Timer alone this long: show device password / open Wi-Fi setup AP.
static constexpr uint32_t kBtnSetupHoldMs = 5000;

// Calibration
static constexpr float kDefaultCalMassG = 100.0f;
// Fallback factor if NVS empty (raw counts per gram). Tune after first cal.
static constexpr float kDefaultCalFactor = 1000.0f;

// Half Decent heartbeat: disconnect if opted-in client misses this window.
static constexpr uint32_t kHeartbeatTimeoutMs = 5000;

// Deep sleep (long-press Tare): ~µA–mA class draw; wake on Tare or Timer touch.
// Soft OLED-only standby is no longer used.

// Battery sense (only if PIN_BAT_ADC >= 0)
static constexpr float kBatVoltageDivider = 2.0f;  // Vbat = Vadc * this
static constexpr float kBatUsbThresholdV = 4.35f;  // above → treat as USB

// Serial
static constexpr uint32_t kSerialBaud = 115200;
