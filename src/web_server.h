/** @file web_server.h
    @brief WiFi web server providing the CAN analysis web interface.
*/

#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <CustomOTA.h>
#include "can_bridge.h"
#include "can_config.h"
#include "dbc_parser.h"
#include "tx_scheduler.h"
#include "marker_plan.h"

/** @brief Firmware version shown on the OTA page (/update). Keep in step with the README. */
#define FW_VERSION "0.3.0"

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

    /** @brief Start the web server.  WiFi is brought up separately by wifi_net_begin(). */
    void begin();

    /** @brief Push updated CAN data to connected clients — call from loop(). */
    void update();

    /** @return Number of currently connected WebSocket clients. */
    uint32_t getClientCount() const { return client_count; }

    /** @brief Service OTA (deferred restart, upload timeout) — call every loop(). */
    void handleOTA() { ota.handle(); }

    /** @brief OTA update gate — call every loop(). @p reason must be a static string. */
    void setUpdateAllowed(bool allowed, const char* reason) { ota.setUpdateAllowed(allowed, reason); }

private:
    CANBridge* can_bridge;
    AsyncWebServer server;
    AsyncWebSocket ws;
    CustomOTA ota;   //!< Firmware / SPIFFS updates over WiFi at /update
    DBCParser dbc_parser;

    uint32_t client_count;
    unsigned long last_update;
    unsigned long last_stats_print;
    bool spiffs_mounted = false;
    bool reload_dbc_pending = false;

    void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                         AwsEventType type, void *arg, uint8_t *data, size_t len);
    void processCommand(AsyncWebSocketClient *client, const String& command);
    void sendCANDataToClients();
    void monitorClientQueues();

    const char* generateMainPage();
    const char* generateCSS();
    const char* generateJavaScript();
    String generateConfigPage();
    String generateDBCPage();
    String generateTxPage();
    String generateMarkersPage();
    String generateLicensesPage();

    String get_can_ids_summary();
    String get_selected_payload_detail(uint32_t can_id);
    String get_dbc_file_list_json();
    static String sanitize_dbc_filename(const String& raw);

    void reload_dbc_parser();
    static bool   is_in_dbc_list(const char* list, const char* fname);
    static void   add_to_dbc_list(char* list, size_t list_size, const char* fname);
    static void   remove_from_dbc_list(char* list, const char* fname);
};

#endif // WEB_SERVER_H
