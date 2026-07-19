#include <Arduino.h>
#include <cmath>
#include <cstring>

#include "battery.h"
#include "ble_decent.h"
#include "buttons.h"
#include "buzzer.h"
#include "config.h"
#include "display.h"
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

bool standby = false;

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

void enterStandby() {
  if (standby) return;
  standby = true;
  shot_timer.stop();
  display.setPowerSave(true);
  buzzer.sleepChime();
  setStatus(nullptr, 0);
  Serial.println("[power] standby (hold Tare or any press to wake)");
}

void leaveStandby() {
  if (!standby) return;
  standby = false;
  display.setPowerSave(false);
  buzzer.wakeChime();
  setStatus("Wake", 1200);
  Serial.println("[power] wake");
}

void doTare(bool from_ble) {
  if (standby) leaveStandby();
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
  if (standby) leaveStandby();
  shot_timer.start();
  buzzer.timerStartChime();
}

void doTimerStop() {
  if (standby) leaveStandby();
  shot_timer.stop();
  buzzer.timerStopChime();
}

void doTimerReset() {
  if (standby) leaveStandby();
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
      leaveStandby();
      ble.setAppMode(true);
      ble.notifyLedAck(battery.decentBatteryByte());
      buzzer.tareChime();
      break;
    case DecentCommand::Type::LedOff:
      ble.notifyLedAck(battery.decentBatteryByte());
      break;
    case DecentCommand::Type::PowerOff:
      // Soft standby instead of hard power-off (USB breadboard)
      enterStandby();
      ble.notifyLedAck(battery.decentBatteryByte());
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
  if (standby) leaveStandby();
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
  // Any interaction wakes from standby (except we handle TareLong as toggle)
  if (standby) {
    if (ev == BtnEvent::TareLong) {
      leaveStandby();
      return;
    }
    if (ev != BtnEvent::None) {
      leaveStandby();
      // Fall through so short press still does its action after wake
    }
  }

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
      if (standby) {
        leaveStandby();
      } else {
        enterStandby();
      }
      ble.notifyButton(1, 2);
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
        Serial.println("  sleep / wake / battery");
        Serial.println("  wifi / wifi set / wifi scan / wifi connect …");
      } else if (serial_line.equalsIgnoreCase("tare")) {
        doTare(false);
      } else if (serial_line.equalsIgnoreCase("sleep")) {
        enterStandby();
      } else if (serial_line.equalsIgnoreCase("wake")) {
        leaveStandby();
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
  if (standby) return;
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

}  // namespace

void setup() {
  Serial.begin(kSerialBaud);
  delay(300);
  Serial.println();
  Serial.printf("=== %s v%s ===\n", kProductName, kFirmwareVersion);
  Serial.println("Type 'help' for serial commands.");

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

    if (!standby) {
      pushWeightHistory(w);
      maybeAutoTimer(w);
    }

    uint8_t mm = 0, ss = 0, ds = 0;
    shot_timer.decentFields(mm, ss, ds);
    // Keep BLE streaming in standby so apps still get weight if connected
    ble.notifyWeight(w, stable, mm, ss, ds);
  }

  if (!standby && now - last_display_ms >= kDisplayRefreshMs) {
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
