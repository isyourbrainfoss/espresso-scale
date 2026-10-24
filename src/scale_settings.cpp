#include "scale_settings.h"

#include <Preferences.h>

#include "config.h"

namespace {
Preferences prefs;
constexpr const char* kNs = "scale_ui";
constexpr const char* kTarget = "tgt";
constexpr const char* kWarn = "warn";
constexpr const char* kPMin = "pmin";
constexpr const char* kPMax = "pmax";
}  // namespace

void ScaleSettings::begin() {
  load();
  clamp();
  Serial.printf("[scale_ui] target=%.0fg warn=%.0fg P=%.0f-%.0f bar\n",
                static_cast<double>(s_.target_yield_g),
                static_cast<double>(s_.warn_at_g),
                static_cast<double>(s_.pressure_min_bar),
                static_cast<double>(s_.pressure_max_bar));
}

void ScaleSettings::load() {
  if (!prefs.begin(kNs, true)) {
    s_.target_yield_g = kDefaultTargetYieldG;
    s_.warn_at_g = kDefaultYieldWarnG;
    s_.pressure_min_bar = kPressureBarMinBar;
    s_.pressure_max_bar = kPressureBarMaxBar;
    return;
  }
  s_.target_yield_g = prefs.getFloat(kTarget, kDefaultTargetYieldG);
  s_.warn_at_g = prefs.getFloat(kWarn, kDefaultYieldWarnG);
  s_.pressure_min_bar = prefs.getFloat(kPMin, kPressureBarMinBar);
  s_.pressure_max_bar = prefs.getFloat(kPMax, kPressureBarMaxBar);
  prefs.end();
}

void ScaleSettings::save() {
  if (!prefs.begin(kNs, false)) return;
  prefs.putFloat(kTarget, s_.target_yield_g);
  prefs.putFloat(kWarn, s_.warn_at_g);
  prefs.putFloat(kPMin, s_.pressure_min_bar);
  prefs.putFloat(kPMax, s_.pressure_max_bar);
  prefs.end();
  Serial.println("[scale_ui] saved");
}

void ScaleSettings::clamp() {
  if (s_.target_yield_g < 10.0f) s_.target_yield_g = 10.0f;
  if (s_.target_yield_g > 80.0f) s_.target_yield_g = 80.0f;
  if (s_.warn_at_g < 5.0f) s_.warn_at_g = 5.0f;
  if (s_.warn_at_g > s_.target_yield_g) s_.warn_at_g = s_.target_yield_g;
  if (s_.pressure_min_bar < 0.0f) s_.pressure_min_bar = 0.0f;
  if (s_.pressure_max_bar > 15.0f) s_.pressure_max_bar = 15.0f;
  if (s_.pressure_max_bar < s_.pressure_min_bar + 1.0f) {
    s_.pressure_max_bar = s_.pressure_min_bar + 5.0f;
  }
}

bool ScaleSettings::apply(float target_g, float warn_g, float p_min,
                          float p_max) {
  s_.target_yield_g = target_g;
  s_.warn_at_g = warn_g;
  s_.pressure_min_bar = p_min;
  s_.pressure_max_bar = p_max;
  clamp();
  save();
  return true;
}

bool ScaleSettings::applyFromBytes(uint8_t target_g, uint8_t warn_g,
                                   uint8_t p_min, uint8_t p_max) {
  return apply(static_cast<float>(target_g), static_cast<float>(warn_g),
               static_cast<float>(p_min), static_cast<float>(p_max));
}
