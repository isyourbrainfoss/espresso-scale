#include "display.h"

#include <U8g2lib.h>
#include <Wire.h>
#include <cmath>

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

  // Standalone brew confirm keeps live weight visible.
  if (s.brew_confirm) {
    char wbuf[16];
    snprintf(wbuf, sizeof(wbuf), "%0.1f g", static_cast<double>(s.weight_g));
    u8g2.setFont(u8g2_font_logisoso18_tr);
    u8g2.drawStr(0, 28, wbuf);
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(8, 44, "Start brew?");
    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.drawStr(2, 58, "Timer = OK");
    u8g2.drawStr(72, 58, "Tare = cancel");
    u8g2.sendBuffer();
    return;
  }

  // Idle kitchen scale: huge weight only (dosing beans, etc.).
  if (!s.recording && !s.timer_running) {
    char wbuf[16];
    snprintf(wbuf, sizeof(wbuf), "%0.1f", static_cast<double>(s.weight_g));
    u8g2.setFont(u8g2_font_logisoso32_tr);
    int ww = u8g2.getStrWidth(wbuf);
    // Center weight; fall back left if too wide.
    int wx = (128 - ww) / 2;
    if (wx < 0) wx = 0;
    u8g2.drawStr(wx, 42, wbuf);
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(wx + ww + 2, 40, "g");
    u8g2.setFont(u8g2_font_5x8_tr);
    if (s.ble_connected || s.app_mode) {
      u8g2.drawStr(2, 58, "APP");
    } else if (s.ble_advertising) {
      u8g2.drawStr(2, 58, "BLE");
    }
    // Right side: flow while pouring (see espresso hit), else live bar.
    if (fabsf(s.flow_g_s) >= 0.05f) {
      char fbuf[16];
      snprintf(fbuf, sizeof(fbuf), "%0.1fg/s", static_cast<double>(s.flow_g_s));
      int fw = u8g2.getStrWidth(fbuf);
      u8g2.drawStr(128 - fw - 2, 58, fbuf);
    } else if (s.has_pressure) {
      char pbuf[16];
      snprintf(pbuf, sizeof(pbuf), "%0.1fb",
               static_cast<double>(s.pressure_bar));
      int pw = u8g2.getStrWidth(pbuf);
      u8g2.drawStr(128 - pw - 2, 58, pbuf);
    }
    if (!s.app_mode) {
      u8g2.drawStr(48, 63, "long Timer=brew");
    }
    u8g2.sendBuffer();
    return;
  }

  // Shot / timer mode: weight + pressure + bars
  char wbuf[16];
  snprintf(wbuf, sizeof(wbuf), "%0.1f", static_cast<double>(s.weight_g));
  u8g2.setFont(u8g2_font_logisoso18_tr);
  u8g2.drawStr(0, 26, wbuf);
  u8g2.setFont(u8g2_font_5x8_tr);
  u8g2.drawStr(50, 18, "g");

  if (s.has_pressure) {
    char pbuf[12];
    snprintf(pbuf, sizeof(pbuf), "%0.1f", static_cast<double>(s.pressure_bar));
    u8g2.setFont(u8g2_font_logisoso18_tr);
    int pw = u8g2.getStrWidth(pbuf);
    u8g2.drawStr(128 - pw - 12, 26, pbuf);
    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.drawStr(120, 18, "b");
  } else {
    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.drawStr(88, 22, s.prs_link ? "PRS…" : "no P");
  }

  // Cup fill bar (0 → target) with warn mark
  const int barX = 2;
  const int barY = 32;
  const int barW = 124;
  const int barH = 7;
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

  // Pressure bar (default 5–10 bar) with 1-bar tick marks
  const int pBarY = 42;
  float pMin = s.pressure_bar_min;
  float pMax = s.pressure_bar_max;
  if (pMax <= pMin + 0.5f) {
    pMin = 5.0f;
    pMax = 10.0f;
  }
  const float pSpan = pMax - pMin;
  u8g2.drawFrame(barX, pBarY, barW, barH);
  // Tick marks at each whole bar between min and max (exclusive of ends).
  for (int bar = static_cast<int>(pMin) + 1; bar < static_cast<int>(pMax); ++bar) {
    float t = (static_cast<float>(bar) - pMin) / pSpan;
    if (t <= 0.0f || t >= 1.0f) continue;
    int tx = barX + static_cast<int>(t * barW);
    u8g2.drawVLine(tx, pBarY - 1, barH + 2);
  }
  if (s.has_pressure) {
    float pp = (s.pressure_bar - pMin) / pSpan;
    if (pp < 0) pp = 0;
    if (pp > 1) pp = 1;
    int pFill = static_cast<int>(pp * (barW - 2));
    if (pFill > 0) {
      u8g2.drawBox(barX + 1, pBarY + 1, pFill, barH - 2);
    }
  }

  u8g2.setFont(u8g2_font_5x8_tr);
  // End labels for pressure window
  {
    char lo[6];
    char hi[6];
    snprintf(lo, sizeof(lo), "%.0f", static_cast<double>(pMin));
    snprintf(hi, sizeof(hi), "%.0f", static_cast<double>(pMax));
    u8g2.drawStr(barX, pBarY + barH + 7, lo);
    int hw = u8g2.getStrWidth(hi);
    u8g2.drawStr(barX + barW - hw, pBarY + barH + 7, hi);
  }

  char fbuf[20];
  snprintf(fbuf, sizeof(fbuf), "%0.1fg/s", static_cast<double>(s.flow_g_s));
  u8g2.drawStr(2, 63, fbuf);

  if (s.recording) {
    u8g2.drawStr(48, 63, "REC");
    // Natural stop: short-press Timer
    u8g2.drawStr(72, 63, "Tmr=stop");
  } else if (s.near_target) {
    u8g2.drawStr(48, 63, "WIND");
    if (s.prs_link) {
      u8g2.drawStr(78, 63, "PRS");
    } else if (s.app_mode || s.ble_connected) {
      u8g2.drawStr(78, 63, "APP");
    }
  } else if (s.prs_link) {
    u8g2.drawStr(78, 63, "PRS");
  } else if (s.app_mode || s.ble_connected) {
    u8g2.drawStr(78, 63, "APP");
  } else if (s.ble_advertising) {
    u8g2.drawStr(78, 63, "BLE");
  }

  if (!s.recording) {
    char tlab[12];
    snprintf(tlab, sizeof(tlab), "/%0.0fg", static_cast<double>(s.target_yield_g));
    u8g2.drawStr(100, 63, tlab);
  }

  u8g2.sendBuffer();
}
