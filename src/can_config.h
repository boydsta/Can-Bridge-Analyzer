/** @file can_config.h
    @brief Runtime CAN configuration stored in NVS.
    @details All settings are edited via the /config web page and saved to NVS.
             Changes take effect after reboot.
*/

#ifndef CAN_CONFIG_H
#define CAN_CONFIG_H

/** @brief Runtime serial logging mode flag. */
extern volatile bool g_serial_logging;
/** @brief Serial output mode: 0=Diagnostics, 1=CSV, 2=Binary GVRET. */
extern volatile uint8_t g_serial_mode;

#include <stdint.h>
#include <Preferences.h>

// ---------------------------------------------------------------------------
// Bus preset table — combines arbitration speed, CAN FD data multiplier, and
// frame type into a single user-visible choice.  Both CAN0 and CAN1 always
// share the same preset (mismatching frame types on the same network causes
// bus errors).
// ---------------------------------------------------------------------------
struct BusPreset {
  const char* label;      ///< Human-readable string shown in the config page
  uint32_t    arb_bps;    ///< Arbitration bit rate in bit/s
  uint8_t     data_mult;  ///< Data-phase multiplier: 1 = CAN 2.0 / same as arb; 2/4/8 for CAN FD
  bool        is_fd;      ///< true = CAN FD mode, false = Classic CAN 2.0B
};

static const BusPreset k_bus_presets[] = {
  { "125 kbps  —  CAN 2.0",    125000,  1, false },
  { "250 kbps  —  CAN 2.0",    250000,  1, false },
  { "500 kbps  —  CAN 2.0",    500000,  1, false },  // index 2 = default
  { "1 Mbps    —  CAN 2.0",   1000000,  1, false },
  { "500k / 2M  —  CAN FD",    500000,  4, true  },
  { "500k / 4M  —  CAN FD",    500000,  8, true  },
  { "1M / 4M    —  CAN FD",   1000000,  4, true  },
  { "1M / 8M    —  CAN FD",   1000000,  8, true  },
};
static const uint8_t k_num_presets = (uint8_t)(sizeof(k_bus_presets) / sizeof(k_bus_presets[0]));
static const uint8_t k_default_preset = 2;  // 500 kbps CAN 2.0

// ---------------------------------------------------------------------------
// CANConfig — all configurable settings persisted in NVS namespace "can_cfg"
// ---------------------------------------------------------------------------
struct CANConfig {
  bool     can0_enable;     ///< Enable CAN0 controller
  bool     can1_enable;     ///< Enable CAN1 controller
  bool     can0_print;      ///< Print CAN0 frames to USB serial
  bool     can1_print;      ///< Print CAN1 frames to USB serial
  uint8_t  bus_preset;      ///< Index into k_bus_presets[] — encodes speed + frame type
  uint8_t  serial_mode;     ///< 0=Diagnostics  1=SavvyCAN CSV  2=Binary GVRET
  char     wifi_ssid[33];   ///< WiFi AP SSID (max 32 chars)
  char     wifi_pass[64];   ///< WiFi AP password; empty = open network
  char     active_dbcs[128]; ///< Comma-separated list of active DBC files (e.g. "leaf.dbc,tesla.dbc"); empty = none
};

/** @brief Load CANConfig from NVS, applying sensible defaults. */
inline CANConfig load_can_config() {
  CANConfig cfg;
  Preferences prefs;
  prefs.begin("can_cfg", /* readOnly= */ true);
  cfg.can0_enable  = prefs.getBool("c0_en", true);
  cfg.can1_enable  = prefs.getBool("c1_en", false);
  cfg.can0_print   = prefs.getBool("c0_pr", true);
  cfg.can1_print   = prefs.getBool("c1_pr", false);
  cfg.bus_preset   = (uint8_t)prefs.getUChar("bus_preset", k_default_preset);
  if (cfg.bus_preset >= k_num_presets) cfg.bus_preset = k_default_preset;
  cfg.serial_mode  = (uint8_t)prefs.getUChar("ser_mode", 1);
  String ss = prefs.getString("wifi_ssid", "CanBridgeAnalyzer");
  strncpy(cfg.wifi_ssid, ss.c_str(), sizeof(cfg.wifi_ssid) - 1);
  cfg.wifi_ssid[sizeof(cfg.wifi_ssid) - 1] = '\0';
  String wp = prefs.getString("wifi_pass", "canbus123");
  strncpy(cfg.wifi_pass, wp.c_str(), sizeof(cfg.wifi_pass) - 1);
  cfg.wifi_pass[sizeof(cfg.wifi_pass) - 1] = '\0';
  String dbc = prefs.getString("active_dbcs", "");
  strncpy(cfg.active_dbcs, dbc.c_str(), sizeof(cfg.active_dbcs) - 1);
  cfg.active_dbcs[sizeof(cfg.active_dbcs) - 1] = '\0';
  prefs.end();
  return cfg;
}

/** @brief Save CANConfig to NVS. */
inline void save_can_config(const CANConfig& cfg) {
  Preferences prefs;
  prefs.begin("can_cfg", /* readOnly= */ false);
  prefs.putBool("c0_en",    cfg.can0_enable);
  prefs.putBool("c1_en",    cfg.can1_enable);
  prefs.putBool("c0_pr",    cfg.can0_print);
  prefs.putBool("c1_pr",    cfg.can1_print);
  prefs.putUChar("bus_preset", cfg.bus_preset);
  prefs.putUChar("ser_mode", cfg.serial_mode);
  prefs.putString("wifi_ssid", cfg.wifi_ssid);
  prefs.putString("wifi_pass", cfg.wifi_pass);
  prefs.putString("active_dbcs", cfg.active_dbcs);
  prefs.end();
}

#endif // CAN_CONFIG_H
