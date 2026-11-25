/** @file wifi_net.h
    @brief WiFi bring-up: own hotspot (AP) or join a network (STA) with hotspot fallback.
    @details In STA mode the unit tries to join the configured network.  If it
             cannot connect within k_sta_connect_timeout_ms, or loses the network
             for that long, it starts its own hotspot (wifi_ssid / 192.168.1.1)
             so the /config page is always reachable.  While on the fallback
             hotspot it retries the network periodically, but only when nobody
             is connected to the hotspot.
*/

#ifndef WIFI_NET_H
#define WIFI_NET_H

#include <Arduino.h>
#include "can_config.h"

/** @brief Start WiFi per @p cfg.  Non-blocking — call wifi_net_loop() every loop(). */
void wifi_net_begin(const CANConfig& cfg);

/** @brief Drive connect / fallback / retry state machine. */
void wifi_net_loop();

/** @brief One-line human-readable status, e.g. "Joined 'Shed' — 192.168.0.42". */
String wifi_net_status();

#endif // WIFI_NET_H
