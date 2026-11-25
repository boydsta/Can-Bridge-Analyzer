/** @file factory_reset.cpp
    @brief Hardware factory reset: hold AI2 above 7 V for 200 ms.
*/

#include "factory_reset.h"
#include <Arduino.h>
#include <Preferences.h>
#include "spud.h"
#include "tx_scheduler.h"
#include "marker_plan.h"

extern volatile uint8_t g_serial_mode;

/** AI2 divider: 0–20 V at the connector → 0–3.3 V at the ESP32 pin. */
static const float    k_ai_full_scale_v   = 20.0f;
static const float    k_adc_full_scale_mv = 3300.0f;
/** Trigger level at the connector (V). */
static const float    k_reset_threshold_v = 7.0f;
/** Re-arm level: input must drop below this after boot before a reset can fire. */
static const float    k_rearm_threshold_v = 3.0f;
/** How long the input must stay above the threshold. */
static const uint32_t k_reset_hold_ms     = 200;
/** Sample interval. */
static const uint32_t k_sample_ms         = 10;

static const uint32_t k_trip_mv  = (uint32_t)(k_reset_threshold_v * k_adc_full_scale_mv / k_ai_full_scale_v);
static const uint32_t k_rearm_mv = (uint32_t)(k_rearm_threshold_v * k_adc_full_scale_mv / k_ai_full_scale_v);

static bool     s_armed     = false;
static uint32_t s_high_since = 0;   ///< millis() when the input went high (0 = not high)
static uint32_t s_last_sample = 0;

static void erase_namespace(const char* ns) {
  Preferences prefs;
  if (prefs.begin(ns, false)) {
    prefs.clear();
    prefs.end();
  }
}

static void do_factory_reset() {
  if (g_serial_mode != 2) {
    Serial.println();
    Serial.println("[RESET] AI2 held high — FACTORY RESET: erasing all settings");
  }
  erase_namespace("can_cfg");
  erase_namespace(TX_SCHED_NVS_NS);
  erase_namespace(MARKER_NVS_NS);
  if (g_serial_mode != 2) {
    Serial.println("[RESET] Done. Rebooting to hotspot 'CanBridgeAnalyzer' / 'canbus123' at 192.168.1.1");
    Serial.flush();
  }
  delay(200);
  ESP.restart();
}

void factory_reset_begin() {
  analogSetPinAttenuation(AI2, ADC_11db);   // full 0–3.3 V range
}

void factory_reset_poll() {
  const uint32_t now = millis();
  if (now - s_last_sample < k_sample_ms) return;
  s_last_sample = now;

  const uint32_t mv = analogReadMilliVolts(AI2);

  if (!s_armed) {
    if (mv < k_rearm_mv) s_armed = true;
    return;
  }

  if (mv >= k_trip_mv) {
    if (s_high_since == 0) s_high_since = now ? now : 1;
    else if (now - s_high_since >= k_reset_hold_ms) do_factory_reset();
  } else {
    s_high_since = 0;
  }
}
