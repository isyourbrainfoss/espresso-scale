#include <Arduino.h>
#include <cmath>
#include <cstring>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include "battery.h"
#include "ble_decent.h"
#include "buttons.h"
#include "buzzer.h"
#include "config.h"
#include "device_auth.h"
#include "display.h"
#include "pins.h"
#include "pressensor_client.h"
#include "scale.h"
#include "scale_settings.h"
#include "shot_recorder.h"
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
PressensorClient prs;
ShotRecorder shot_rec;
ScaleSettings scale_ui;

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

// Phone-forwarded pressure (app brew keeps PRS; scale only displays P).
bool phone_brew_active = false;
bool has_phone_pressure = false;
float phone_pressure_bar = 0;
uint32_t phone_pressure_ms = 0;

// Standalone brew owns PRS link + local shot recorder.
bool standalone_brew_active = false;
// Long-press Timer arms a confirm prompt (short Timer = OK, Tare = cancel).
bool brew_confirm_pending = false;
uint32_t brew_confirm_until_ms = 0;
// Soft yield-warn chime once per brew when cup crosses warn_at_g.
bool yield_warn_fired = false;

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

void maybePushNextcloud() {
  if (!wifi_ota.hasNextcloud()) return;
  String json = shot_rec.readFlashJson();
  if (json.isEmpty()) json = shot_rec.toJson();
  if (json.isEmpty()) return;
  if (wifi_ota.pushShotToNextcloud(json)) {
    setStatus("NC pushed", 2000);
  } else {
    setStatus("NC push fail", 2000);
  }
}

// Full deep sleep: radios off, OLED off, HX711 PD. Wake on Tare or Timer HIGH.
void enterDeepSleep() {
  Serial.println("[power] deep sleep — touch Tare or Timer to wake");
  Serial.println("[power] note: red/blue on a BMS module often stay on with battery");
  if (shot_rec.isRecording()) {
    shot_rec.stop();
  }
  prs.end();
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

void clearBrewConfirm() {
  brew_confirm_pending = false;
  brew_confirm_until_ms = 0;
}

void stopPhoneBrew();  // defined below; used by standalone start

void armBrewConfirm() {
  if (phone_brew_active || standalone_brew_active || shot_rec.isRecording()) {
    return;
  }
  brew_confirm_pending = true;
  brew_confirm_until_ms = millis() + kBrewConfirmTimeoutMs;
  buzzer.tareChime();  // soft acknowledge arm
  Serial.println("[brew] confirm armed — Timer=OK Tare=cancel");
}

void cancelBrewConfirm() {
  if (!brew_confirm_pending) return;
  clearBrewConfirm();
  setStatus("Brew cancel", 1200);
  Serial.println("[brew] confirm cancelled");
}

void startStandaloneBrew() {
  if (standalone_brew_active) return;

  // Phone may have left mirror mode; clear it so local brew owns the scale.
  if (phone_brew_active) {
    stopPhoneBrew();
  }

  clearBrewConfirm();
  standalone_brew_active = true;
  yield_warn_fired = false;
  // Tare only when a brew is explicitly confirmed (not on every Timer tap).
  doTare(false);
  shot_timer.start();
  shot_rec.start();
  // Free if already linked; request fresh PRS connection for pressure.
  if (!prs.isConnected()) {
    prs.requestConnect();
    setStatus("Scan PRS…", 2500);
  } else {
    setStatus("Brew (PRS)", 1500);
  }
  buzzer.timerStartChime();
  Serial.println("[brew] standalone start");
}

void stopStandaloneBrew() {
  clearBrewConfirm();
  const bool had_local_shot =
      standalone_brew_active || shot_rec.isRecording();
  if (!had_local_shot) {
    if (shot_timer.running()) {
      shot_timer.stop();
      buzzer.timerStopChime();
    }
    return;
  }
  standalone_brew_active = false;
  yield_warn_fired = false;
  shot_timer.stop();
  const size_t n = shot_rec.sampleCount();
  buzzer.timerStopChime();
  // Only save real pulls into the 3-shot ring — abort short/empty recordings.
  if (shot_rec.isRecording()) {
    if (n > 5) {
      shot_rec.stop();
      setStatus("Shot saved", 2000);
      Serial.printf("[brew] standalone stop — stored=%u\n",
                    static_cast<unsigned>(shot_rec.storedCount()));
      maybePushNextcloud();
    } else {
      shot_rec.abort();
      setStatus("Brew cancel", 1200);
      Serial.println("[brew] standalone stop — discarded short shot");
    }
  }
}

void startPhoneBrew() {
  // App owns the shot + Pressensor. Scale only mirrors pressure on OLED.
  // Do NOT tare, do NOT SPIFFS-record, do NOT chime — app already tared and
  // a noisy "brew start" here made Timer short later look like "Shot saved".
  clearBrewConfirm();
  if (standalone_brew_active || shot_rec.isRecording()) {
    // Abort any leftover local recording without "Shot saved" UX confusion.
    standalone_brew_active = false;
    if (shot_rec.isRecording()) {
      shot_rec.stop();
    }
  }
  prs.disconnect();
  phone_brew_active = true;
  yield_warn_fired = false;
  // Keep live grams on the OLED. Tare already chimed; a full-screen
  // "App brew" overlay hid the pour. Idle layout stays huge weight.
  Serial.println("[brew] phone brew start (mirror only, silent)");
}

void stopPhoneBrew() {
  if (!phone_brew_active && !has_phone_pressure) return;
  phone_brew_active = false;
  has_phone_pressure = false;
  // Never claim "Shot saved" for app-driven sessions.
  Serial.println("[brew] phone brew end");
}

// Pure shot timer (no tare, no PRS, no SPIFFS record) — kitchen / dose timing.
void doPureTimerToggle() {
  if (shot_timer.running()) {
    shot_timer.stop();
    buzzer.timerStopChime();
  } else {
    shot_timer.start();
    buzzer.timerStartChime();
  }
}

void doTimerStart() {
  // BLE timer command: pure timer only (never auto-start a standalone brew).
  if (standalone_brew_active || shot_rec.isRecording()) {
    return;
  }
  clearBrewConfirm();
  shot_timer.start();
  buzzer.timerStartChime();
}

void doTimerStop() {
  if (phone_brew_active) {
    shot_timer.stop();
    buzzer.timerStopChime();
    return;
  }
  if (standalone_brew_active || shot_rec.isRecording()) {
    stopStandaloneBrew();
    return;
  }
  clearBrewConfirm();
  shot_timer.stop();
  buzzer.timerStopChime();
}

void doTimerReset() {
  clearBrewConfirm();
  if (standalone_brew_active || shot_rec.isRecording()) {
    stopStandaloneBrew();
  }
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
      // Phone app connected for weight — free PRS for the phone.
      if (!phone_brew_active && !standalone_brew_active) {
        prs.disconnect();
      }
      ble.notifyLedAck(battery.decentBatteryByte());
      // Silent — app start already sends tare; avoid beep spam.
      break;
    case DecentCommand::Type::LedOff:
      ble.notifyLedAck(battery.decentBatteryByte());
      break;
    case DecentCommand::Type::PowerOff:
      ble.notifyLedAck(battery.decentBatteryByte());
      // enterDeepSleep() -> ble.end() waits for the ack to be sent and
      // disconnects the client cleanly before deinitialising NimBLE.
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
    case DecentCommand::Type::Heartbeat:  // handled in BLE callback, not queued
      break;
    case DecentCommand::Type::PhonePressure:
      phone_pressure_bar = cmd.pressure_mbar / 1000.0f;
      has_phone_pressure = true;
      phone_pressure_ms = millis();
      break;
    case DecentCommand::Type::PhoneBrewStart:
      startPhoneBrew();
      break;
    case DecentCommand::Type::PhoneBrewEnd:
      stopPhoneBrew();
      break;
    case DecentCommand::Type::ScaleDisplayConfig:
      scale_ui.applyFromBytes(cmd.cfg_target_g, cmd.cfg_warn_g, cmd.cfg_p_min,
                              cmd.cfg_p_max);
      // Silent apply (called on every app brew start).
      break;
    case DecentCommand::Type::ShotExport: {
      // opcode 0=list sizes, 1=transfer age, 2=status/IP
      if (cmd.shot_opcode == 0) {
        uint16_t sizes[3] = {0, 0, 0};
        const uint8_t n = static_cast<uint8_t>(shot_rec.storedCount());
        for (uint8_t i = 0; i < n && i < 3; i++) {
          sizes[i] = static_cast<uint16_t>(
              shot_rec.slotMeta(i).bytes > 65535 ? 65535
                                                 : shot_rec.slotMeta(i).bytes);
        }
        ble.notifyShotList(sizes, n);
      } else if (cmd.shot_opcode == 1) {
        if (ble.isTransferBusy()) {
          ble.cancelShotTransfer();
        }
        String json = shot_rec.readSlotJson(cmd.shot_slot);
        if (json.isEmpty()) {
          // Empty list response for missing slot
          uint16_t z[3] = {0, 0, 0};
          ble.notifyShotList(z, 0);
        } else {
          ble.beginShotTransfer(cmd.shot_slot, json);
        }
      } else if (cmd.shot_opcode == 2) {
        String ip = wifi_ota.ipString();
        if (ip.isEmpty()) ip = "no-wifi";
        ble.notifyShotStatus(ip.c_str());
      }
      break;
    }
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
      // Confirm prompt: Tare cancels without zeroing (bean weighing safe).
      if (brew_confirm_pending) {
        cancelBrewConfirm();
        ble.notifyButton(1, 1);
        return;
      }
      doTare(false);
      ble.notifyButton(1, 1);
      break;
    case BtnEvent::TareLong:
      clearBrewConfirm();
      ble.notifyButton(1, 2);
      enterDeepSleep();
      break;
    case BtnEvent::TimerShort:
      if (phone_brew_active) {
        // App session left mirror mode — clear it so Timer can start a local brew.
        stopPhoneBrew();
        setStatus("App off", 1000);
        ble.notifyButton(2, 1);
        return;
      }
      if (brew_confirm_pending) {
        // Second press confirms phone-free brew (tare + PRS + record).
        startStandaloneBrew();
        ble.notifyButton(2, 1);
        return;
      }
      if (standalone_brew_active || shot_rec.isRecording()) {
        // End standalone brew / save shot.
        stopStandaloneBrew();
        ble.notifyButton(2, 1);
        return;
      }
      // Idle short press: pure timer only — never starts a recorded brew.
      doPureTimerToggle();
      ble.notifyButton(2, 1);
      break;
    case BtnEvent::TimerLong:
      if (phone_brew_active) {
        stopPhoneBrew();
      }
      if (standalone_brew_active || shot_rec.isRecording()) {
        // Long-press during brew: stop + reset timer.
        doTimerReset();
      } else if (brew_confirm_pending) {
        // Second long-press dismisses confirm.
        cancelBrewConfirm();
      } else {
        // Long-press Timer arms "Start brew?" (not reset).
        armBrewConfirm();
      }
      ble.notifyButton(2, 2);
      break;
    case BtnEvent::BothHeldCal:
      clearBrewConfirm();
      enterCalMode();
      break;
    case BtnEvent::TimerSetupHold:
      // Physical-access gesture: show the device password; if not on a
      // LAN (or the password screen is already up), open the setup AP.
      // The 1 s long-press already armed "Start brew?"; this is not a brew.
      clearBrewConfirm();
      buzzer.successChime();
      if (wifi_ota.isAccessPoint()) {
        wifi_ota.requestSetupScreen();
      } else if (wifi_ota.isStation() && !wifi_ota.setupScreenActive()) {
        wifi_ota.requestSetupScreen();
      } else {
        wifi_ota.startSetupAp();
      }
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
        Serial.println("  brew start|stop  - phone-free brew + PRS");
        Serial.println("  Buttons: Tare short=zero, long=sleep");
        Serial.println("           Timer short=timer / confirm OK / stop brew");
        Serial.println("           Timer long=Start brew? prompt");
        Serial.println("           Hold Timer ~1 s at power-on = WiFi setup AP");
        Serial.println("  prs|prs scan|prs off");
        Serial.println("  shot          - print last shot JSON length");
        Serial.println("  wifi / wifi set / wifi scan / wifi connect / wifi ap / wifi clear");
        Serial.println("  nc push|clear - Nextcloud WebDAV for last shot");
        Serial.println("  zero clear    - forget stored empty-platform zero");
        Serial.println("  auth show     - show device password on the OLED");
        Serial.println("  auth reset    - new random device password (reboots)");
      } else if (serial_line.equalsIgnoreCase("zero clear")) {
        scale.clearEmptyZero();
      } else if (serial_line.equalsIgnoreCase("tare")) {
        doTare(false);
      } else if (serial_line.equalsIgnoreCase("sleep")) {
        enterDeepSleep();
      } else if (serial_line.equalsIgnoreCase("battery")) {
        Serial.printf("battery %s (%d%%) usb=%d\n", battery.label(),
                      battery.percent(), battery.isUsb() ? 1 : 0);
      } else if (serial_line.equalsIgnoreCase("brew start")) {
        startStandaloneBrew();
      } else if (serial_line.equalsIgnoreCase("brew stop")) {
        stopStandaloneBrew();
      } else if (serial_line.equalsIgnoreCase("prs") ||
                 serial_line.equalsIgnoreCase("prs scan")) {
        prs.requestConnect();
        setStatus("Scan PRS…", 3000);
      } else if (serial_line.equalsIgnoreCase("prs off")) {
        prs.disconnect();
        Serial.println("[prs] disconnected");
      } else if (serial_line.equalsIgnoreCase("shot")) {
        String j = shot_rec.readFlashJson();
        if (j.isEmpty()) j = shot_rec.toJson();
        Serial.printf("shot bytes=%u has=%d recording=%d\n",
                      static_cast<unsigned>(j.length()),
                      shot_rec.hasShot() ? 1 : 0,
                      shot_rec.isRecording() ? 1 : 0);
        if (j.length() > 0 && j.length() < 400) {
          Serial.println(j);
        } else if (j.length() >= 400) {
          Serial.println(j.substring(0, 200) + "…");
        }
      } else if (serial_line.equalsIgnoreCase("nc push")) {
        maybePushNextcloud();
      } else if (serial_line.equalsIgnoreCase("nc clear")) {
        wifi_ota.clearNextcloud();
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
        Serial.println(
            "[wifi] credentials cleared. Radio stays off after reboot. "
            "Hold Timer at boot, or send 'wifi ap', for setup.");
        delay(200);
        ESP.restart();
      } else if (serial_line.equalsIgnoreCase("auth show")) {
        // Shown on the OLED only (not echoed to the serial log).
        wifi_ota.requestSetupScreen();
        Serial.println("[auth] device password shown on the OLED for 60 s");
      } else if (serial_line.equalsIgnoreCase("auth reset")) {
        device_auth::reset();
        Serial.println("[auth] password erased — new one generated on reboot");
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
          setStatus("WiFi off", 3000);
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
        String err;
        if (!WifiOta::validateCredentials(ssid, pass, &err)) {
          Serial.printf("Not saved: %s\n", err.c_str());
        } else if (wifi_ota.saveCredentials(ssid, pass)) {
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
      if (serial_line.length() > 128) serial_line = "";
    }
  }
}

void maybeAutoTimer(float weight_g) {
  if (!kAutoTimerEnabled) return;
  if (shot_timer.running() || standalone_brew_active || phone_brew_active) return;
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

void resolvePressure(float& out_bar, bool& out_has, bool& out_prs_link) {
  out_prs_link = prs.isConnected();
  // Stale phone pressure
  if (has_phone_pressure &&
      (millis() - phone_pressure_ms) > kPhonePressureStaleMs) {
    has_phone_pressure = false;
  }
  if (phone_brew_active || has_phone_pressure) {
    out_has = has_phone_pressure;
    out_bar = phone_pressure_bar;
    return;
  }
  if (prs.isConnected() && prs.hasReading()) {
    out_has = true;
    out_bar = prs.pressureBar();
    return;
  }
  out_has = false;
  out_bar = 0;
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

String shotJsonForHttp() {
  String flash = shot_rec.readFlashJson();
  if (!flash.isEmpty()) return flash;
  return shot_rec.toJson();
}

bool hasShotForHttp() {
  return shot_rec.storedCount() > 0;
}

String shotsListForHttp() { return shot_rec.listJson(); }

String shotAtForHttp(size_t age) { return shot_rec.readSlotJson(age); }

bool wokeFromSleep() {
  const auto cause = esp_sleep_get_wakeup_cause();
  return cause == ESP_SLEEP_WAKEUP_EXT0 || cause == ESP_SLEEP_WAKEUP_EXT1 ||
         cause == ESP_SLEEP_WAKEUP_TIMER;
}

// --- Boot sequencing -------------------------------------------------------
// setup() only brings up what the weight readout needs (HX711, OLED, NVS).
// loop() then shows live grams while this sequencer starts the radios in the
// background: Wi-Fi after the boot zero (or kRadioStartMaxDelayMs), BLE once
// the Wi-Fi scan is over — the same "decide Wi-Fi before NimBLE" order as
// v1.6.3, without blocking the weighing loop.
struct BootTimes {
  uint32_t first_frame = 0;   // weight screen first drawn
  uint32_t first_live = 0;    // first frame with zeroed, live grams
  uint32_t wifi_start = 0;
  uint32_t ble_start = 0;
  uint32_t wifi_done = 0;
};
BootTimes boot_t;
bool woke_from_sleep = false;
bool radios_started = false;
bool ble_started = false;
// Cold-boot setup-hotspot gesture: Timer held for most of kWifiSetupHoldMs.
int hold_samples = 0;
int hold_high = 0;
uint32_t hold_last_ms = 0;
bool setup_ap_requested = false;
// Ignore buttons until released: the finger that woke the scale (or the
// one holding Timer for the hotspot) must not also tare / start a timer.
bool buttons_locked = true;

void sampleSetupHold(uint32_t now) {
  if (woke_from_sleep || radios_started || now >= kWifiSetupHoldMs) return;
  if (now - hold_last_ms < 40) return;
  hold_last_ms = now;
  hold_samples++;
  if (digitalRead(PIN_BTN_TIMER) == HIGH) hold_high++;
}

void bootSequencer(uint32_t now) {
  sampleSetupHold(now);

  if (!radios_started) {
    if (!woke_from_sleep && now < kWifiSetupHoldMs) return;
    if (!scale.bootZeroDone() && now < kRadioStartMaxDelayMs) return;
    radios_started = true;
    // Most of the hold window, not a tap.
    setup_ap_requested = !woke_from_sleep && hold_samples > 0 &&
                         hold_high * 5 >= hold_samples * 4;
    boot_t.wifi_start = now;
    if (wifi_ota.mode() == WifiMode::Off) {  // serial `wifi ap` may have won
      // The setup AP draws its own SSID/password screen; no status overlay.
      wifi_ota.setShotFileFn([](size_t age, char* path, size_t len) {
        return shot_rec.slotFile(age, path, len);
      });
      wifi_ota.begin([]() { return scale.displayGrams(); }, shotJsonForHttp,
                     hasShotForHttp, shotsListForHttp, shotAtForHttp,
                     setup_ap_requested);
    }
    return;  // let loop() run once before NimBLE init
  }

  if (!ble_started && wifi_ota.bootScanDone()) {
    ble_started = true;
    boot_t.ble_start = millis();
    ble.begin(handleBleCommand);
    prs.begin();
  }

  if (!boot_t.wifi_done && wifi_ota.bootSettled()) {
    boot_t.wifi_done = millis();
    Serial.printf("[boot] live weight %lu ms, BLE %lu ms, Wi-Fi %s %lu ms\n",
                  static_cast<unsigned long>(boot_t.first_live),
                  static_cast<unsigned long>(boot_t.ble_start),
                  wifi_ota.modeLabel(),
                  static_cast<unsigned long>(boot_t.wifi_done));
    wifi_ota.printStatus();
  }
}

String bootInfoForHttp() {
  auto sec = [](uint32_t ms) { return String(ms / 1000.0f, 2) + " s"; };
  String s = F("Boot: live weight ");
  s += boot_t.first_live ? sec(boot_t.first_live) : String("-");
  s += F(" (zero ");
  s += scale.bootZeroDone() ? sec(scale.bootZeroMs()) : String("-");
  s += F(") · radios start ");
  s += boot_t.wifi_start ? sec(boot_t.wifi_start) : String("-");
  s += F(" · BLE ");
  s += boot_t.ble_start ? sec(boot_t.ble_start) : String("-");
  s += F(" · Wi-Fi ");
  s += boot_t.wifi_done ? sec(boot_t.wifi_done) : String("-");
  s += F(" · ");
  switch (scale.bootZero()) {
    case Scale::BootZero::First:
      s += F("tared (new empty ref)");
      break;
    case Scale::BootZero::Tared:
      s += F("tared, ");
      s += String(scale.bootLoadG(), 1);
      s += F(" g vs empty ref");
      break;
    case Scale::BootZero::KeptEmptyZero:
      s += F("kept empty ref (load ");
      s += String(scale.bootLoadG(), 1);
      s += F(" g at boot)");
      break;
    default:
      s += F("pending");
  }
  s += F(" · HX711 ");
  s += String(scale.sps(), 1);
  s += F(" SPS · last reset ");
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: s += F("power-on"); break;
    case ESP_RST_SW: s += F("software"); break;
    case ESP_RST_PANIC: s += F("PANIC"); break;
    case ESP_RST_INT_WDT: s += F("INT_WDT"); break;
    case ESP_RST_TASK_WDT: s += F("TASK_WDT"); break;
    case ESP_RST_WDT: s += F("WDT"); break;
    case ESP_RST_DEEPSLEEP: s += F("deep-sleep wake"); break;
    case ESP_RST_BROWNOUT: s += F("BROWNOUT"); break;
    default: s += String(static_cast<int>(esp_reset_reason()));
  }
  // Heap: free now, lowest since boot, largest single block (fragmentation).
  s += F(" · heap ");
  s += String(ESP.getFreeHeap() / 1024);
  s += F(" KB free, min ");
  s += String(ESP.getMinFreeHeap() / 1024);
  s += F(" KB, largest block ");
  s += String(ESP.getMaxAllocHeap() / 1024);
  s += F(" KB");
  return s;
}

}  // namespace

void setup() {
  // No settle delay for USB CDC: early lines may be missed by a host that
  // attaches late, the [boot] summary is printed again once Wi-Fi settles.
  Serial.begin(kSerialBaud);
  // Release any GPIO holds left from previous deep sleep (e.g. RGB pin).
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)PIN_BOARD_RGB);
  woke_from_sleep = wokeFromSleep();

  // HX711 first: it needs ~400 ms to settle, which overlaps everything below.
  if (!scale.begin()) {
    Serial.println("[error] scale begin failed");
  }
  buzzer.begin();
  buttons.begin();
  battery.begin();

  if (!display.begin()) {
    Serial.println("[warn] OLED init failed — continuing headless");
  } else {
    // Straight to the weight screen (held at 0.0 until the boot zero lands).
    DisplayState st{};
    st.scale_ok = true;
    st.target_yield_g = kDefaultTargetYieldG;
    st.warn_at_g = kDefaultYieldWarnG;
    st.pressure_bar_min = kPressureBarMinBar;
    st.pressure_bar_max = kPressureBarMaxBar;
    display.render(st);
    boot_t.first_frame = millis();
  }

  shot_rec.begin();
  scale_ui.begin();
  wifi_ota.setBootInfo(bootInfoForHttp);
  wifi_ota.setTareHandler([]() { doTare(false); });

  Serial.println();
  Serial.printf("=== %s v%s ===\n", kProductName, kFirmwareVersion);
  logWakeupCause();
  Serial.println("Type 'help' for serial commands.");
  Serial.println("Tare short = zero (beans safe). Tare long = sleep.");
  Serial.println("Timer short = kitchen timer. Timer long = Start brew?");
  Serial.println("  then Timer again = start (tare+PRS+REC), Tare = cancel.");
  Serial.println("Hold Timer for ~1 s right after power-on to open the WiFi "
                 "setup hotspot.");

  buzzer.bootChime();
  auto_armed = true;  // boot zero lands within ~1 s
  last_notify_ms = millis();
  last_display_ms = millis();
}

void loop() {
  scale.update();
  buzzer.update();
  bootSequencer(millis());
  if (ble_started) {
    ble.update();
    ble.pumpShotTransfer();
    prs.update();
  }
  wifi_ota.update();
  battery.update();
  handleSerial();

  // If the phone just connected for weight, free PRS (one central at a time).
  static bool was_ble = false;
  if (ble.isConnected() && !was_ble) {
    if (!phone_brew_active && !standalone_brew_active) {
      prs.disconnect();
    }
  }
  was_ble = ble.isConnected();

  BtnEvent ev = buttons.poll();
  if (buttons_locked) {
    if (digitalRead(PIN_BTN_TARE) == LOW && digitalRead(PIN_BTN_TIMER) == LOW) {
      buttons_locked = false;
    }
    ev = BtnEvent::None;
  }
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

    // Brew confirm prompt times out so it cannot stick around while dosing.
    if (brew_confirm_pending && millis() > brew_confirm_until_ms) {
      cancelBrewConfirm();
    }

    float p_bar = 0;
    bool has_p = false;
    bool prs_link = false;
    resolvePressure(p_bar, has_p, prs_link);

    if (shot_rec.isRecording()) {
      shot_rec.add(p_bar, has_p, w, true);
    }
    // Soft wind-back cue near target (once per brew) — standalone SPIFFS
    // recording OR phone app brew (mirror mode does not SPIFFS-record).
    const bool yield_warn_armed =
        shot_rec.isRecording() || standalone_brew_active || phone_brew_active;
    if (yield_warn_armed && !yield_warn_fired &&
        w >= scale_ui.get().warn_at_g) {
      yield_warn_fired = true;
      buzzer.yieldWarnChime();
      Serial.printf("[brew] yield warn @ %.1fg (thresh %.0f) app=%d rec=%d\n",
                    static_cast<double>(w),
                    static_cast<double>(scale_ui.get().warn_at_g),
                    phone_brew_active ? 1 : 0,
                    shot_rec.isRecording() ? 1 : 0);
    }

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
    st.app_mode = ble.appMode() || ble.isConnected() || phone_brew_active;
    st.scale_ok = scale.isReady();
    st.wifi_label = wifi_ota.modeLabel();
    st.ota_active = wifi_ota.otaInProgress();
    st.battery_label = battery.label();
    st.standby = false;
    st.target_yield_g = scale_ui.get().target_yield_g;
    st.warn_at_g = scale_ui.get().warn_at_g;
    st.near_target = st.weight_g >= scale_ui.get().warn_at_g &&
                     st.weight_g < scale_ui.get().target_yield_g + 2.0f;
    st.pressure_bar_min = scale_ui.get().pressure_min_bar;
    st.pressure_bar_max = scale_ui.get().pressure_max_bar;
    st.recording = shot_rec.isRecording();
    st.brew_confirm = brew_confirm_pending;
    resolvePressure(st.pressure_bar, st.has_pressure, st.prs_link);
    String setup_ip;
    if (wifi_ota.setupScreenActive()) {
      st.setup_screen = true;
      st.setup_ap = wifi_ota.isAccessPoint();
      st.setup_ssid = st.setup_ap ? device_auth::apSsid().c_str() : kHostname;
      st.setup_pass = device_auth::password().c_str();
      setup_ip = wifi_ota.ipString();
      st.setup_ip = setup_ip.c_str();
      st.setup_secs_left = wifi_ota.apSecondsLeft();
    }
    // Confirm UI is its own layout — only flash status when not confirming.
    if (!brew_confirm_pending && status_msg && now < status_until_ms) {
      st.status = status_msg;
    } else if (!brew_confirm_pending) {
      status_msg = nullptr;
      if (cal_mode == CalMode::WaitMass) {
        st.status = "Cal: add mass";
      } else if (wifi_ota.otaInProgress()) {
        st.status = "OTA updating";
      } else if (prs.isScanning()) {
        st.status = "PRS scan…";
      }
    }
    display.render(st);
    if (!boot_t.first_live && scale.bootZeroDone()) boot_t.first_live = millis();
  }
}
