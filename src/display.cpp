#include "display.h"

#include <U8g2lib.h>
#include <Wire.h>

#include "config.h"
#include "pins.h"

namespace {
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE);
}  // namespace

bool Display::begin() {
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  u8g2.setI2CAddress(0x3C << 1);
  if (!u8g2.begin()) {
    Serial.println("[display] u8g2 begin failed");
    ready_ = false;
    return false;
  }
  ready_ = true;
  power_save_ = false;
  Serial.println("[display] ready");
  return true;
}

void Display::setPowerSave(bool on) {
  if (!ready_) return;
  power_save_ = on;
  u8g2.setPowerSave(on ? 1 : 0);
}

void Display::showSplash() {
  if (!ready_ || power_save_) return;
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(8, 20, "Half Decent");
  u8g2.drawStr(20, 36, "Espresso Scale");
  u8g2.setFont(u8g2_font_5x8_tr);
  u8g2.drawStr(36, 54, kFirmwareVersion);
  u8g2.sendBuffer();
}

void Display::render(const DisplayState& s) {
  if (!ready_) return;
  if (power_save_ || s.standby) return;

  u8g2.clearBuffer();

  if (s.status != nullptr) {
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(0, 28, s.status);
    u8g2.sendBuffer();
    return;
  }

  if (!s.scale_ok) {
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(8, 32, "HX711 not ready");
    u8g2.sendBuffer();
    return;
  }

  // Top row: battery (left) · wifi / OTA (right)
  u8g2.setFont(u8g2_font_5x8_tr);
  if (s.battery_label && s.battery_label[0]) {
    u8g2.drawStr(2, 8, s.battery_label);
  }
  if (s.ota_active) {
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(80, 10, "OTA");
  } else if (s.wifi_label && s.wifi_label[0]) {
    int wl = u8g2.getStrWidth(s.wifi_label);
    u8g2.drawStr(128 - wl - 2, 8, s.wifi_label);
  }

  // Large weight
  char wbuf[16];
  snprintf(wbuf, sizeof(wbuf), "%0.1f", static_cast<double>(s.weight_g));
  u8g2.setFont(u8g2_font_logisoso24_tr);
  int ww = u8g2.getStrWidth(wbuf);
  u8g2.drawStr(2, 34, wbuf);
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(2 + ww + 2, 32, "g");

  // Flow rate
  char fbuf[16];
  snprintf(fbuf, sizeof(fbuf), "%0.2f g/s", static_cast<double>(s.flow_g_s));
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(2, 48, fbuf);

  // Timer
  uint32_t total_s = s.timer_ms / 1000;
  uint32_t mm = total_s / 60;
  uint32_t ss = total_s % 60;
  char tbuf[12];
  snprintf(tbuf, sizeof(tbuf), "%lu:%02lu", static_cast<unsigned long>(mm),
           static_cast<unsigned long>(ss));
  if (s.timer_running) {
    u8g2.drawStr(2, 62, tbuf);
    u8g2.drawStr(40, 62, ">");
  } else {
    u8g2.drawStr(2, 62, tbuf);
  }

  if (s.app_mode || s.ble_connected) {
    u8g2.drawStr(90, 62, "APP");
  } else if (s.ble_advertising) {
    u8g2.drawStr(90, 62, "BLE");
  }

  u8g2.sendBuffer();
}
