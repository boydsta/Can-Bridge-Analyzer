/** @file can_config.h
    @brief Runtime CAN configuration stored in NVS.
    @details All settings are edited via the /config web page and saved to NVS.
             Changes take effect after reboot.
*/

#ifndef CAN_CONFIG_H
#define CAN_CONFIG_H

#ifndef MCP2515
#define MCP2515 1
#endif
#ifndef MCP251863
#define MCP251863 2
#endif
#ifndef CAN_CHIP
#define CAN_CHIP MCP251863
#endif
#if CAN_CHIP != MCP2515 && CAN_CHIP != MCP251863
#error "CAN_CHIP must be MCP2515 or MCP251863"
#endif

/** @brief Runtime serial logging mode flag. */
extern volatile bool g_serial_logging;
/** @brief Serial output mode: 0=Diagnostics, 1=CSV, 2=Binary GVRET. */
extern volatile uint8_t g_serial_mode;

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <Preferences.h>
#include <esp_system.h>

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

#if CAN_CHIP == MCP2515
static const BusPreset k_bus_presets[] = {
  { "125 kbps  —  CAN 2.0",    125000,  1, false },
  { "250 kbps  —  CAN 2.0",    250000,  1, false },
  { "500 kbps  —  CAN 2.0",    500000,  1, false },
  { "1 Mbps    —  CAN 2.0",   1000000,  1, false },
};
#else
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
#endif
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
  char     wifi_ssid[33];   ///< WiFi AP SSID (max 32 chars) — own hotspot, also the fallback
  char     wifi_pass[64];   ///< WiFi AP password; empty = open network
  char     active_dbcs[128]; ///< Comma-separated list of active DBC files (e.g. "leaf.dbc,tesla.dbc"); empty = none

  uint8_t  wifi_mode;       ///< WIFI_MODE_CFG_AP or WIFI_MODE_CFG_STA
  char     sta_ssid[33];    ///< Network to join (station mode)
  char     sta_pass[64];    ///< Password of the network to join; empty = open
  bool     sta_static;      ///< true = static IP below, false = DHCP
  uint32_t sta_ip;          ///< Static IP (IPAddress as uint32_t)
  uint32_t sta_mask;        ///< Static subnet mask
  uint32_t sta_gw;          ///< Static gateway
  uint32_t sta_dns;         ///< Static DNS server; 0 = use gateway
  char     hostname[33];    ///< DHCP hostname and mDNS name (<hostname>.local)
};

static const uint8_t WIFI_MODE_CFG_AP  = 0;  ///< Run own hotspot only (default)
static const uint8_t WIFI_MODE_CFG_STA = 1;  ///< Join a network; fall back to own hotspot on failure

/** @brief Default hostname "canbridge-xxxxxx" from the last 3 bytes of the factory MAC. */
inline void default_hostname(char* out, size_t len) {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(out, len, "canbridge-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

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

  cfg.wifi_mode = (uint8_t)prefs.getUChar("wifi_mode", WIFI_MODE_CFG_AP);
  if (cfg.wifi_mode > WIFI_MODE_CFG_STA) cfg.wifi_mode = WIFI_MODE_CFG_AP;
  String sts = prefs.getString("sta_ssid", "");
  strncpy(cfg.sta_ssid, sts.c_str(), sizeof(cfg.sta_ssid) - 1);
  cfg.sta_ssid[sizeof(cfg.sta_ssid) - 1] = '\0';
  String stp = prefs.getString("sta_pass", "");
  strncpy(cfg.sta_pass, stp.c_str(), sizeof(cfg.sta_pass) - 1);
  cfg.sta_pass[sizeof(cfg.sta_pass) - 1] = '\0';
  cfg.sta_static = prefs.getBool("sta_static", false);
  cfg.sta_ip     = prefs.getUInt("sta_ip",   0);
  cfg.sta_mask   = prefs.getUInt("sta_mask", 0);
  cfg.sta_gw     = prefs.getUInt("sta_gw",   0);
  cfg.sta_dns    = prefs.getUInt("sta_dns",  0);
  String hn = prefs.getString("hostname", "");
  strncpy(cfg.hostname, hn.c_str(), sizeof(cfg.hostname) - 1);
  cfg.hostname[sizeof(cfg.hostname) - 1] = '\0';
  if (cfg.hostname[0] == '\0') default_hostname(cfg.hostname, sizeof(cfg.hostname));
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
  prefs.putUChar("wifi_mode",  cfg.wifi_mode);
  prefs.putString("sta_ssid",  cfg.sta_ssid);
  prefs.putString("sta_pass",  cfg.sta_pass);
  prefs.putBool("sta_static",  cfg.sta_static);
  prefs.putUInt("sta_ip",      cfg.sta_ip);
  prefs.putUInt("sta_mask",    cfg.sta_mask);
  prefs.putUInt("sta_gw",      cfg.sta_gw);
  prefs.putUInt("sta_dns",     cfg.sta_dns);
  prefs.putString("hostname",  cfg.hostname);
  prefs.end();
}

#endif // CAN_CONFIG_H
