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
  u8g2.drawStr(20, 20, "Flowlog");
  u8g2.drawStr(28, 36, "Scale");
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

  // Large weight (primary Flowlog readout)
  char wbuf[16];
  snprintf(wbuf, sizeof(wbuf), "%0.1f", static_cast<double>(s.weight_g));
  u8g2.setFont(u8g2_font_logisoso24_tr);
  int ww = u8g2.getStrWidth(wbuf);
  u8g2.drawStr(2, 30, wbuf);
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(2 + ww + 2, 28, "g");

  // Target label e.g. /36
  char tlabel[20];
  snprintf(tlabel, sizeof(tlabel), "/%0.0f", static_cast<double>(s.target_yield_g));
  u8g2.drawStr(2 + ww + 14, 28, tlabel);

  // Cup fill bar (0 → target) with warn mark — mirrors Flowlog Live bar
  const int barX = 2;
  const int barY = 36;
  const int barW = 124;
  const int barH = 8;
  u8g2.drawFrame(barX, barY, barW, barH);
  float target = s.target_yield_g > 1.0f ? s.target_yield_g : 36.0f;
  float progress = s.weight_g / target;
  if (progress < 0) progress = 0;
  if (progress > 1) progress = 1;
  int fillW = static_cast<int>(progress * (barW - 2));
  if (fillW > 0) {
    u8g2.drawBox(barX + 1, barY + 1, fillW, barH - 2);
  }
  float warn = s.warn_at_g;
  if (warn > 0 && warn < target) {
    int wx = barX + static_cast<int>((warn / target) * barW);
    u8g2.drawVLine(wx, barY - 1, barH + 2);
  }

  // Flow + link status (timer is secondary for Flowlog use)
  char fbuf[16];
  snprintf(fbuf, sizeof(fbuf), "%0.1f g/s", static_cast<double>(s.flow_g_s));
  u8g2.setFont(u8g2_font_5x8_tr);
  u8g2.drawStr(2, 54, fbuf);

  if (s.near_target) {
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(50, 54, "WIND BACK");
  } else if (s.app_mode || s.ble_connected) {
    u8g2.drawStr(90, 54, "FL");
  } else if (s.ble_advertising) {
    u8g2.drawStr(90, 54, "BLE");
  }

  u8g2.setFont(u8g2_font_5x8_tr);
  u8g2.drawStr(2, 63, "Flowlog scale");

  u8g2.sendBuffer();
}
