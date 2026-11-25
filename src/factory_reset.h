/** @file factory_reset.h
    @brief Hardware factory reset: hold AI2 above 7 V for 200 ms.
    @details AI2 (GPIO35, connector pin 24) is a 0–20 V analog input that
             normally reads 0 V.  Bridging it to +12 V (pin 1) for at least
             k_reset_hold_ms erases every setting this firmware stores in NVS
             (WiFi, CAN, TX schedule, marker plan) and reboots into the default
             hotspot 'CanBridgeAnalyzer' / 'canbus123' at 192.168.1.1.
             DBC files in SPIFFS and the OTA rollback slot are kept.

             The input must first be seen LOW after boot before a reset can
             trigger, so a jumper left in place does not reset on every boot.
*/

#ifndef FACTORY_RESET_H
#define FACTORY_RESET_H

/** @brief Configure the AI2 ADC.  Call once early in setup(). */
void factory_reset_begin();

/** @brief Sample AI2 and perform the reset when held high.  Call often (setup wait + loop). */
void factory_reset_poll();

#endif // FACTORY_RESET_H
