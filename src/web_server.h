/** @file web_server.h
    @brief WiFi web server providing the CAN analysis web interface.
*/

#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include "can_bridge.h"
#include "can_config.h"

/**
 * @brief Web server for the CAN Bridge Analyzer interface.
 * @details Hosts a WiFi access point and serves a real-time web UI over WebSocket.
 */
class CANWebServer {
public:
    /**
     * @brief Construct the web server.
     * @param bridge Pointer to the CAN bridge for data access (may be nullptr).
     */
    CANWebServer(CANBridge* bridge);

    /**
     * @brief Start the WiFi AP and web server.
     * @param ssid     WiFi network name.
     * @param password WiFi password (nullptr or empty = open network).
     */
    void begin(const char* ssid = "CanBridgeAnalyzer", const char* password = "canbus123");

    /** @brief Push updated CAN data to connected clients — call from loop(). */
    void update();

    /** @return Number of currently connected WebSocket clients. */
    uint32_t getClientCount() const { return client_count; }

private:
    CANBridge* can_bridge;
    AsyncWebServer server;
    AsyncWebSocket ws;

    uint32_t client_count;
    unsigned long last_update;
    unsigned long last_stats_print;

    void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                         AwsEventType type, void *arg, uint8_t *data, size_t len);
    void processCommand(AsyncWebSocketClient *client, const String& command);
    void sendCANDataToClients();
    void monitorClientQueues();

    const char* generateMainPage();
    const char* generateCSS();
    const char* generateJavaScript();
    String generateConfigPage();

    String get_can_ids_summary();
    String get_selected_payload_detail(uint32_t can_id);
};

#endif // WEB_SERVER_H
