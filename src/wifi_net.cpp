/** @file wifi_net.cpp
    @brief WiFi bring-up: own hotspot (AP) or join a network (STA) with hotspot fallback.
*/

#include "wifi_net.h"
#include <WiFi.h>
#include <ESPmDNS.h>

extern volatile uint8_t g_serial_mode;

/** Give up joining after this long and fall back to the hotspot. */
static const unsigned long k_sta_connect_timeout_ms = 20000;
/** While on the fallback hotspot, retry the network this often (if no AP clients). */
static const unsigned long k_sta_retry_interval_ms  = 60000;

enum class NetState : uint8_t {
  AP_ONLY,      ///< Configured for hotspot mode
  CONNECTING,   ///< STA connecting (first attempt — no hotspot running)
  STA_UP,       ///< Joined the network
  FALLBACK_AP,  ///< STA failed; running own hotspot
  RETRYING,     ///< On fallback hotspot, STA retry in progress (AP+STA)
};

static CANConfig     s_cfg;
static NetState      s_state       = NetState::AP_ONLY;
static unsigned long s_state_ms    = 0;   ///< millis() when the current state began
static unsigned long s_sta_lost_ms = 0;   ///< millis() when STA_UP lost its link (0 = link ok)
static bool          s_mdns_up     = false;

/** Text diagnostics only in serial mode 0 — CSV / GVRET streams must stay clean. */
static bool log_ok() { return g_serial_mode == 0; }

static void set_state(NetState st) {
  s_state = st;
  s_state_ms = millis();
}

static void start_mdns() {
  if (s_mdns_up) return;
  if (MDNS.begin(s_cfg.hostname)) {
    MDNS.addService("http", "tcp", 80);
    s_mdns_up = true;
    if (log_ok()) Serial.printf("[WiFi] mDNS: http://%s.local\n", s_cfg.hostname);
  }
}

static void start_ap(wifi_mode_t mode) {
  WiFi.mode(mode);
  WiFi.softAPConfig(IPAddress(192, 168, 1, 1),
                    IPAddress(192, 168, 1, 1),
                    IPAddress(255, 255, 255, 0));
  WiFi.softAP(s_cfg.wifi_ssid, s_cfg.wifi_pass[0] ? s_cfg.wifi_pass : nullptr);
  if (log_ok()) {
    Serial.printf("[WiFi] Hotspot '%s' up — http://192.168.1.1\n", s_cfg.wifi_ssid);
  }
  start_mdns();
}

static void sta_begin() {
  if (s_cfg.sta_static && s_cfg.sta_ip != 0) {
    IPAddress dns = s_cfg.sta_dns ? IPAddress(s_cfg.sta_dns) : IPAddress(s_cfg.sta_gw);
    WiFi.config(IPAddress(s_cfg.sta_ip), IPAddress(s_cfg.sta_gw),
                IPAddress(s_cfg.sta_mask), dns);
  }
  WiFi.begin(s_cfg.sta_ssid, s_cfg.sta_pass[0] ? s_cfg.sta_pass : nullptr);
}

static void on_sta_connected() {
  /* Hotspot no longer needed once we're on the network. */
  if (WiFi.getMode() & WIFI_MODE_AP) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
  }
  set_state(NetState::STA_UP);
  s_sta_lost_ms = 0;
  if (log_ok()) {
    Serial.printf("[WiFi] Joined '%s' — IP %s\n", s_cfg.sta_ssid,
                  WiFi.localIP().toString().c_str());
  }
  start_mdns();
}

void wifi_net_begin(const CANConfig& cfg) {
  s_cfg = cfg;
  WiFi.persistent(false);   // credentials live in our own NVS namespace
  WiFi.setHostname(s_cfg.hostname);

  if (s_cfg.wifi_mode == WIFI_MODE_CFG_STA && s_cfg.sta_ssid[0] != '\0') {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    sta_begin();
    set_state(NetState::CONNECTING);
    if (log_ok()) Serial.printf("[WiFi] Joining '%s'...\n", s_cfg.sta_ssid);
  } else {
    start_ap(WIFI_AP);
    set_state(NetState::AP_ONLY);
  }
}

void wifi_net_loop() {
  const unsigned long now = millis();
  const bool linked = (WiFi.status() == WL_CONNECTED);

  switch (s_state) {
    case NetState::AP_ONLY:
      break;

    case NetState::CONNECTING:
      if (linked) {
        on_sta_connected();
      } else if (now - s_state_ms >= k_sta_connect_timeout_ms) {
        if (log_ok()) Serial.printf("[WiFi] Could not join '%s' — falling back to hotspot\n", s_cfg.sta_ssid);
        WiFi.disconnect(true);
        start_ap(WIFI_AP);
        set_state(NetState::FALLBACK_AP);
      }
      break;

    case NetState::STA_UP:
      if (linked) {
        s_sta_lost_ms = 0;
      } else if (s_sta_lost_ms == 0) {
        s_sta_lost_ms = now;   // auto-reconnect is running; give it a chance
      } else if (now - s_sta_lost_ms >= k_sta_connect_timeout_ms) {
        if (log_ok()) Serial.printf("[WiFi] Lost '%s' — falling back to hotspot\n", s_cfg.sta_ssid);
        WiFi.disconnect(true);
        start_ap(WIFI_AP);
        set_state(NetState::FALLBACK_AP);
      }
      break;

    case NetState::FALLBACK_AP:
      /* Retry only when nobody is on the hotspot: the STA scan can move the
         AP channel and would drop someone who is fixing the config. */
      if (now - s_state_ms >= k_sta_retry_interval_ms) {
        if (WiFi.softAPgetStationNum() == 0) {
          WiFi.mode(WIFI_AP_STA);
          sta_begin();
          set_state(NetState::RETRYING);
        } else {
          s_state_ms = now;   // someone connected — check again later
        }
      }
      break;

    case NetState::RETRYING:
      if (linked) {
        on_sta_connected();
      } else if (now - s_state_ms >= k_sta_connect_timeout_ms) {
        WiFi.disconnect(false);
        WiFi.mode(WIFI_AP);   // drop STA, keep the hotspot
        set_state(NetState::FALLBACK_AP);
      }
      break;
  }
}

String wifi_net_status() {
  switch (s_state) {
    case NetState::AP_ONLY:
      return "Hotspot '" + String(s_cfg.wifi_ssid) + "' — 192.168.1.1";
    case NetState::CONNECTING:
      return "Joining '" + String(s_cfg.sta_ssid) + "'...";
    case NetState::STA_UP:
      return "Joined '" + String(s_cfg.sta_ssid) + "' — " + WiFi.localIP().toString() +
             " (" + String(s_cfg.hostname) + ".local)";
    case NetState::FALLBACK_AP:
    case NetState::RETRYING:
      return "Could not join '" + String(s_cfg.sta_ssid) + "' — fallback hotspot '" +
             String(s_cfg.wifi_ssid) + "' — 192.168.1.1";
  }
  return "";
}
