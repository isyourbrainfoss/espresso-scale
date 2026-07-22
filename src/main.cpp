#include <Arduino.h>
#include <cmath>
#include <cstring>
#include <driver/gpio.h>
#include <esp_sleep.h>

#include "battery.h"
#include "ble_decent.h"
#include "buttons.h"
#include "buzzer.h"
#include "config.h"
#include "display.h"
#include "pins.h"
#include "scale.h"
#include "shot_timer.h"
#include "wifi_ota.h"

namespace {

Scale scale;
Display display;
Buttons buttons;
Buzzer buzzer;
ShotTimer shot_timer;
BleDecent ble;
WifiOta wifi_ota;
Battery battery;

enum class CalMode : uint8_t { Idle, WaitEmpty, WaitMass };
CalMode cal_mode = CalMode::Idle;
float cal_mass_g = kDefaultCalMassG;
const char* status_msg = nullptr;
uint32_t status_until_ms = 0;

float weight_hist[kFlowRateWindowSamples] = {};
int weight_hist_idx = 0;
int weight_hist_count = 0;
float flow_g_s = 0;

// Auto-timer armed after tare / near zero
bool auto_armed = true;

uint32_t last_notify_ms = 0;
uint32_t last_display_ms = 0;
float last_weight_for_stable = 0;
uint32_t last_weight_change_ms = 0;

String serial_line;

void setStatus(const char* msg, uint32_t ms = 2000) {
  status_msg = msg;
  status_until_ms = millis() + ms;
}

void pushWeightHistory(float g) {
  weight_hist[weight_hist_idx] = g;
  weight_hist_idx = (weight_hist_idx + 1) % kFlowRateWindowSamples;
  if (weight_hist_count < kFlowRateWindowSamples) weight_hist_count++;

  if (weight_hist_count >= 2) {
    int oldest = weight_hist_idx;
    if (weight_hist_count < kFlowRateWindowSamples) oldest = 0;
    float dt = (weight_hist_count - 1) * (kWeightNotifyIntervalMs / 1000.0f);
    if (dt > 0.01f) {
      float newest = g;
      float old = weight_hist[oldest];
      if (weight_hist_count == kFlowRateWindowSamples) {
        old = weight_hist[weight_hist_idx];
      }
      flow_g_s = (newest - old) / dt;
      if (fabsf(flow_g_s) < 0.02f) flow_g_s = 0;
    }
  }
}

// Quiesce Super Mini onboard LED / leftover GPIO drive before deep sleep.
// Does not control LEDs on an external BMS (those are often charge indicators).
void boardLedsOff() {
  // GPIO48: WS2812 RGB on many S3 Super Mini boards — hold low + disable.
  pinMode(PIN_BOARD_RGB, OUTPUT);
  digitalWrite(PIN_BOARD_RGB, LOW);
  // Send a few zero bits so a WS2812 latches “off” if it was left on by ROM.
  for (int i = 0; i < 24 * 3; ++i) {
    digitalWrite(PIN_BOARD_RGB, LOW);
    delayMicroseconds(1);
  }
  gpio_hold_en((gpio_num_t)PIN_BOARD_RGB);

  // Buzzer pin low (avoid phantom drive).
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  // I2C bus idle-high via pull-ups only — release MCU drive after OLED power-save.
  pinMode(PIN_OLED_SDA, INPUT);
  pinMode(PIN_OLED_SCL, INPUT);
}

// Full deep sleep: radios off, OLED off, HX711 PD. Wake on Tare or Timer HIGH.
void enterDeepSleep() {
  Serial.println("[power] deep sleep — touch Tare or Timer to wake");
  Serial.println("[power] note: red/blue on a BMS module often stay on with battery");
  shot_timer.stop();
  buzzer.sleepChime();
  // Let chime finish
  for (int i = 0; i < 80; ++i) {
    buzzer.update();
    delay(10);
  }

  DisplayState st{};
  st.status = "Sleep...";
  st.scale_ok = true;
  display.setPowerSave(false);
  display.render(st);
  delay(400);
  display.setPowerSave(true);

  scale.powerDown();
  wifi_ota.end();
  ble.end();
  btStop();
  boardLedsOff();

  // Wait until fingers off so we don't re-wake immediately
  pinMode(PIN_BTN_TARE, INPUT);
  pinMode(PIN_BTN_TIMER, INPUT);
  uint32_t t0 = millis();
  while (millis() - t0 < 5000) {
    if (digitalRead(PIN_BTN_TARE) == LOW && digitalRead(PIN_BTN_TIMER) == LOW) {
      delay(50);
      if (digitalRead(PIN_BTN_TARE) == LOW && digitalRead(PIN_BTN_TIMER) == LOW) {
        break;
      }
    }
    delay(20);
  }
  delay(100);

  // ESP32-S3: ext1 ANY_HIGH on RTC GPIOs 4 & 5 (TTP223 active high)
  const uint64_t wake_mask =
      (1ULL << PIN_BTN_TARE) | (1ULL << PIN_BTN_TIMER);
  esp_sleep_enable_ext1_wakeup(wake_mask, ESP_EXT1_WAKEUP_ANY_HIGH);

  // Keep RGB pin held through deep sleep so it does not float high.
  gpio_deep_sleep_hold_en();

  Serial.flush();
  esp_deep_sleep_start();
  // never returns
}

void doTare(bool from_ble) {
  scale.tare();
  buzzer.tareChime();
  auto_armed = true;
  weight_hist_count = 0;
  weight_hist_idx = 0;
  flow_g_s = 0;
  if (from_ble) {
    ble.setAppMode(true);
  }
}

void doTimerStart() {
  shot_timer.start();
  buzzer.timerStartChime();
}

void doTimerStop() {
  shot_timer.stop();
  buzzer.timerStopChime();
}

void doTimerReset() {
  shot_timer.reset();
  buzzer.timerResetChime();
  auto_armed = true;
}

void handleBleCommand(const DecentCommand& cmd) {
  switch (cmd.type) {
    case DecentCommand::Type::Tare:
      doTare(true);
      ble.notifyTareAck();
      break;
    case DecentCommand::Type::LedOn:
      ble.setAppMode(true);
      ble.notifyLedAck(battery.decentBatteryByte());
      buzzer.tareChime();
      break;
    case DecentCommand::Type::LedOff:
      ble.notifyLedAck(battery.decentBatteryByte());
      break;
    case DecentCommand::Type::PowerOff:
      ble.notifyLedAck(battery.decentBatteryByte());
      enterDeepSleep();
      break;
    case DecentCommand::Type::TimerStart:
      doTimerStart();
      break;
    case DecentCommand::Type::TimerStop:
      doTimerStop();
      break;
    case DecentCommand::Type::TimerReset:
      doTimerReset();
      break;
    case DecentCommand::Type::Heartbeat:
      break;
    default:
      break;
  }
}

void enterCalMode() {
  cal_mode = CalMode::WaitEmpty;
  scale.startCalEmpty();
  setStatus("Cal: empty OK", 3000);
  buzzer.successChime();
  Serial.println("[cal] empty captured (tared). Place known mass, then:");
  Serial.printf("[cal]   serial: cal %.0f\n", static_cast<double>(cal_mass_g));
  Serial.println("[cal]   or press TARE to finish with default mass");
  cal_mode = CalMode::WaitMass;
}

void finishCal(float mass) {
  if (scale.finishCalKnownMass(mass)) {
    setStatus("Cal OK", 2000);
    buzzer.successChime();
  } else {
    setStatus("Cal FAIL", 2000);
    buzzer.errorChime();
  }
  cal_mode = CalMode::Idle;
}

void handleButton(BtnEvent ev) {
  switch (ev) {
    case BtnEvent::TareShort:
      if (cal_mode == CalMode::WaitMass) {
        finishCal(cal_mass_g);
        return;
      }
      doTare(false);
      ble.notifyButton(1, 1);
      break;
    case BtnEvent::TareLong:
      ble.notifyButton(1, 2);
      enterDeepSleep();
      break;
    case BtnEvent::TimerShort:
      if (shot_timer.running()) {
        doTimerStop();
      } else {
        doTimerStart();
      }
      ble.notifyButton(2, 1);
      break;
    case BtnEvent::TimerLong:
      doTimerReset();
      ble.notifyButton(2, 2);
      break;
    case BtnEvent::BothHeldCal:
      enterCalMode();
      break;
    default:
      break;
  }
}

void handleSerial() {
  while (Serial.available()) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c == '\n') {
      serial_line.trim();
      if (serial_line.length() == 0) {
        serial_line = "";
        continue;
      }
      if (serial_line.equalsIgnoreCase("help")) {
        Serial.println("Commands:");
        Serial.println("  help / tare / weight / factor / cal …");
        Serial.println("  sleep         - deep sleep (touch button to wake)");
        Serial.println("  battery");
        Serial.println("  wifi / wifi set / wifi scan / wifi connect …");
      } else if (serial_line.equalsIgnoreCase("tare")) {
        doTare(false);
      } else if (serial_line.equalsIgnoreCase("sleep")) {
        enterDeepSleep();
      } else if (serial_line.equalsIgnoreCase("battery")) {
        Serial.printf("battery %s (%d%%) usb=%d\n", battery.label(),
                      battery.percent(), battery.isUsb() ? 1 : 0);
      } else if (serial_line.equalsIgnoreCase("cal empty")) {
        scale.startCalEmpty();
        cal_mode = CalMode::WaitMass;
        Serial.println("Place known mass, then: cal 100");
      } else if (serial_line.startsWith("cal ")) {
        float m = serial_line.substring(4).toFloat();
        if (m > 0) finishCal(m);
      } else if (serial_line.equalsIgnoreCase("factor")) {
        Serial.printf("cal_factor=%.6f\n", static_cast<double>(scale.calFactor()));
      } else if (serial_line.startsWith("factor ")) {
        float f = serial_line.substring(7).toFloat();
        scale.setCalFactor(f, true);
      } else if (serial_line.equalsIgnoreCase("weight")) {
        Serial.printf("raw=%.2f display=%.2f\n",
                      static_cast<double>(scale.rawGrams()),
                      static_cast<double>(scale.displayGrams()));
      } else if (serial_line.equalsIgnoreCase("wifi") ||
                 serial_line.equalsIgnoreCase("ip")) {
        wifi_ota.printStatus();
      } else if (serial_line.equalsIgnoreCase("wifi clear")) {
        wifi_ota.clearCredentials();
        delay(200);
        ESP.restart();
      } else if (serial_line.equalsIgnoreCase("wifi ap")) {
        wifi_ota.startSetupAp();
        setStatus("WiFi AP mode", 3000);
      } else if (serial_line.equalsIgnoreCase("wifi scan")) {
        wifi_ota.scanNetworks();
      } else if (serial_line.equalsIgnoreCase("wifi connect")) {
        setStatus("WiFi connecting", 3000);
        if (wifi_ota.tryConnectSaved()) {
          setStatus("WiFi OK", 2000);
        } else {
          setStatus("WiFi fail->AP", 3000);
        }
        wifi_ota.printStatus();
      } else if (serial_line.startsWith("wifi set ")) {
        String rest = serial_line.substring(9);
        rest.trim();
        String ssid, pass;
        if (rest.startsWith("\"")) {
          int endq = rest.indexOf('"', 1);
          if (endq < 0) {
            Serial.println("Usage: wifi set \"ssid with spaces\" <password>");
            serial_line = "";
            continue;
          }
          ssid = rest.substring(1, endq);
          pass = rest.substring(endq + 1);
          pass.trim();
        } else {
          int sp = rest.indexOf(' ');
          if (sp <= 0) {
            Serial.println("Usage: wifi set <ssid> <password>");
            serial_line = "";
            continue;
          }
          ssid = rest.substring(0, sp);
          pass = rest.substring(sp + 1);
          pass.trim();
        }
        if (!ssid.isEmpty()) {
          wifi_ota.saveCredentials(ssid, pass);
          Serial.printf("Saved SSID \"%s\" — rebooting…\n", ssid.c_str());
          delay(300);
          ESP.restart();
        }
      } else {
        Serial.println("Unknown. Type help.");
      }
      serial_line = "";
    } else {
      serial_line += c;
      if (serial_line.length() > 80) serial_line = "";
    }
  }
}

void maybeAutoTimer(float weight_g) {
  if (!kAutoTimerEnabled) return;
  if (shot_timer.running()) return;
  if (auto_armed && weight_g >= kAutoTimerThresholdG) {
    doTimerStart();
    auto_armed = false;
    Serial.println("[timer] auto-start");
  }
  if (!auto_armed && fabsf(weight_g) < kAutoTimerNearZeroG &&
      !shot_timer.running()) {
    auto_armed = true;
  }
}

void logWakeupCause() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT0:
    case ESP_SLEEP_WAKEUP_EXT1:
      Serial.println("[power] woke from deep sleep (button)");
      break;
    case ESP_SLEEP_WAKEUP_TIMER:
      Serial.println("[power] woke from timer");
      break;
    default:
      break;
  }
}

}  // namespace

void setup() {
  Serial.begin(kSerialBaud);
  delay(300);
  // Release any GPIO holds left from previous deep sleep (e.g. RGB pin).
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)PIN_BOARD_RGB);

  Serial.println();
  Serial.printf("=== %s v%s ===\n", kProductName, kFirmwareVersion);
  logWakeupCause();
  Serial.println("Type 'help' for serial commands.");
  Serial.println("Long-press Tare = deep sleep; touch Tare/Timer to wake.");

  buzzer.begin();
  buttons.begin();
  battery.begin();

  if (!display.begin()) {
    Serial.println("[warn] OLED init failed — continuing headless");
  } else {
    display.showSplash();
  }

  if (!scale.begin()) {
    Serial.println("[error] scale begin failed");
  }

  ble.begin(handleBleCommand);

  wifi_ota.begin([]() { return scale.displayGrams(); });
  wifi_ota.printStatus();
  if (wifi_ota.isAccessPoint()) {
    setStatus("WiFi setup AP", 4000);
  } else if (wifi_ota.isStation()) {
    setStatus("WiFi OK", 2000);
  }

  buzzer.bootChime();
  auto_armed = true;  // boot tare already zeroed platform
  last_notify_ms = millis();
  last_display_ms = millis();
}

void loop() {
  scale.update();
  buzzer.update();
  ble.update();
  wifi_ota.update();
  battery.update();
  handleSerial();

  BtnEvent ev = buttons.poll();
  if (ev != BtnEvent::None) handleButton(ev);

  const uint32_t now = millis();

  if (now - last_notify_ms >= kWeightNotifyIntervalMs) {
    last_notify_ms = now;
    const float w = scale.rawGrams();

    if (fabsf(w - last_weight_for_stable) > 0.15f) {
      last_weight_change_ms = now;
      last_weight_for_stable = w;
    }
    const bool stable = (now - last_weight_change_ms) > 400;

    pushWeightHistory(w);
    maybeAutoTimer(w);

    uint8_t mm = 0, ss = 0, ds = 0;
    shot_timer.decentFields(mm, ss, ds);
    ble.notifyWeight(w, stable, mm, ss, ds);
  }

  if (now - last_display_ms >= kDisplayRefreshMs) {
    last_display_ms = now;
    DisplayState st;
    st.weight_g = scale.displayGrams();
    st.flow_g_s = flow_g_s;
    st.timer_ms = shot_timer.elapsedMs();
    st.timer_running = shot_timer.running();
    st.ble_connected = ble.isConnected();
    st.ble_advertising = ble.isAdvertising();
    st.app_mode = ble.appMode() || ble.isConnected();
    st.scale_ok = scale.isReady();
    st.wifi_label = wifi_ota.modeLabel();
    st.ota_active = wifi_ota.otaInProgress();
    st.battery_label = battery.label();
    st.standby = false;
    st.target_yield_g = kDefaultTargetYieldG;
    st.warn_at_g = kDefaultYieldWarnG;
    st.near_target = st.weight_g >= kDefaultYieldWarnG &&
                     st.weight_g < kDefaultTargetYieldG + 2.0f;
    if (status_msg && now < status_until_ms) {
      st.status = status_msg;
    } else {
      status_msg = nullptr;
      if (cal_mode == CalMode::WaitMass) {
        st.status = "Cal: add mass";
      } else if (wifi_ota.otaInProgress()) {
        st.status = "OTA updating";
      }
    }
    display.render(st);
  }
}
