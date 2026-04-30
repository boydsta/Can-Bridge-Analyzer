#include "web_server.h"
#include "can_analysis.h"
#include <Preferences.h>
#include "can_config.h"

/* [NEW] Full runtime config — set by setup() from NVS, read here for /api/get_config. */
extern CANConfig g_cfg;

/* [NEW] Current CAN speed — read-only here, set by setup() from NVS. */
extern uint32_t g_can_speed;

/* Runtime serial logging flag — defined in main_reverse_engineering.cpp. */
extern volatile bool g_serial_logging;
/* Runtime serial log format flag — true = GVRET/SavvyCAN, false = Kvaser .asc. */
extern volatile bool g_serial_log_gvret;
/* Runtime serial output mode: 0=Diagnostics  1=SavvyCAN CSV  2=Binary GVRET */
extern volatile uint8_t g_serial_mode;

/* [OLD] Runtime-reinit volatile flags removed — speed changes now save to
   NVS and reboot rather than hotswapping the CAN controllers mid-flight.
extern volatile bool     g_reinit_pending;
extern volatile uint32_t g_pending_speed; */

CANWebServer::CANWebServer(CANBridge* bridge)
    : server(80), ws("/ws"), can_bridge(bridge), client_count(0), last_update(0), last_stats_print(0) {

    // Setup WebSocket
    ws.onEvent([this](AsyncWebSocket *server, AsyncWebSocketClient *client,
                     AwsEventType type, void *arg, uint8_t *data, size_t len) {
        this->onWebSocketEvent(server, client, type, arg, data, len);
    });
    server.addHandler(&ws);
}

void CANWebServer::begin(const char* ssid, const char* password) {
    if (g_serial_logging && g_serial_mode == 0) { Serial.println("Starting web server..."); }

    // Configure WiFi AP
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, password);
    WiFi.softAPConfig(IPAddress(192, 168, 1, 1),
                      IPAddress(192, 168, 1, 1),
                      IPAddress(255, 255, 255, 0));

    if (g_serial_logging && g_serial_mode == 0) {
      Serial.printf("WiFi AP: %s\n", ssid);
      Serial.printf("IP Address: %s\n", WiFi.softAPIP().toString().c_str());
    }

    // Serve main page
    server.on("/", HTTP_GET, [this](AsyncWebServerRequest *request) {
        request->send(200, "text/html", generateMainPage());
    });

    // Serve CSS
    server.on("/style.css", HTTP_GET, [this](AsyncWebServerRequest *request) {
        request->send(200, "text/css", generateCSS());
    });

    // Serve JavaScript
    server.on("/script.js", HTTP_GET, [this](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *resp = request->beginResponse(200, "application/javascript", generateJavaScript());
        resp->addHeader("Cache-Control", "no-store");
        request->send(resp);
    });

    // API endpoint to block CAN ID
    server.on("/api/block", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (request->hasParam("id", true)) {
            String id_str = request->getParam("id", true)->value();
            uint32_t can_id = strtoul(id_str.c_str(), nullptr, 16);
            
            if (can_bridge != nullptr) {
                can_bridge->block_can_id(can_id);
                request->send(200, "text/plain", "CAN ID blocked successfully");
            } else {
                request->send(400, "text/plain", "No bridge available");
            }
        } else {
            request->send(400, "text/plain", "Missing ID parameter");
        }
    });

    // API endpoint to unblock CAN ID
    server.on("/api/unblock", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (request->hasParam("id", true)) {
            String id_str = request->getParam("id", true)->value();
            uint32_t can_id = strtoul(id_str.c_str(), nullptr, 16);
            
            if (can_bridge != nullptr) {
                can_bridge->unblock_can_id(can_id);
                request->send(200, "text/plain", "CAN ID unblocked successfully");
            } else {
                request->send(400, "text/plain", "No bridge available");
            }
        } else {
            request->send(400, "text/plain", "Missing ID parameter");
        }
    });

    // API endpoint to get blocked CAN IDs list
    server.on("/api/blocked_list", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (can_bridge != nullptr) {
            String response = "{\"blocked_ids\":[";
            auto blocked_ids = can_bridge->get_blocked_ids();
            for (size_t i = 0; i < blocked_ids.size(); i++) {
                if (i > 0) response += ",";
                response += "\"0x" + String(blocked_ids[i], HEX) + "\"";
            }
            response += "]}";
            request->send(200, "application/json", response);
        } else {
            request->send(400, "text/plain", "No bridge available");
        }
    });

    // API endpoint to get detailed payload data for selected CAN ID
    server.on("/api/payload_detail", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (request->hasParam("id")) {
            String id_str = request->getParam("id")->value();
            uint32_t can_id = strtoul(id_str.c_str(), nullptr, 16);
            
            //Serial.printf("API request for payload detail: ID=%s (0x%X)\n", id_str.c_str(), can_id);
            
            String payload_detail = get_selected_payload_detail(can_id);
            //Serial.printf("Payload detail response: %s\n", payload_detail.c_str());
            
            request->send(200, "application/json", payload_detail);
        } else {
            request->send(400, "text/plain", "Missing ID parameter");
        }
    });

    // Simple favicon handler to prevent 500 errors
    server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(204); // No content
    });

    /* /config — device configuration page */
    server.on("/config", HTTP_GET, [this](AsyncWebServerRequest *request) {
        request->send(200, "text/html", generateConfigPage());
    });

    /* /api/get_config — returns all current runtime settings as JSON */
    server.on("/api/get_config", HTTP_GET, [](AsyncWebServerRequest *request) {
        String resp = "{";
        resp += "\"can0_enable\":"  + String(g_cfg.can0_enable ? "true" : "false") + ",";
        resp += "\"can1_enable\":"  + String(g_cfg.can1_enable ? "true" : "false") + ",";
        resp += "\"bus_preset\":"   + String(g_cfg.bus_preset);
        resp += "}";
        request->send(200, "application/json", resp);
    });

    /* /api/set_config — save all settings to NVS then reboot.
       POST body (form-encoded): can0_enable=1&can1_enable=0&bus_preset=2&name=MY-LOGGER */
    server.on("/api/set_config", HTTP_POST, [](AsyncWebServerRequest *request) {
        CANConfig cfg;

        /* Checkboxes absent = false (enable flags). */
        cfg.can0_enable = request->hasParam("can0_enable", true);
        cfg.can1_enable = request->hasParam("can1_enable", true);
        cfg.can0_print  = request->hasParam("can0_print",  true);
        cfg.can1_print  = request->hasParam("can1_print",  true);

        /* Bus preset — validated against k_bus_presets table */
        uint8_t preset = k_default_preset;
        if (request->hasParam("bus_preset", true)) {
            preset = (uint8_t)request->getParam("bus_preset", true)->value().toInt();
        }
        if (preset >= k_num_presets) {
            request->send(400, "application/json", "{\"error\":\"Invalid bus_preset index\"}");
            return;
        }
        cfg.bus_preset = preset;

        // serial_mode is managed separately via /api/set_serial_mode; preserve current value
        cfg.serial_mode = g_serial_mode;

        // WiFi SSID
        String ss = "CanBridgeAnalyzer";
        if (request->hasParam("wifi_ssid", true)) {
            ss = request->getParam("wifi_ssid", true)->value();
            ss.trim();
            if (ss.length() == 0) ss = "CanBridgeAnalyzer";
            if (ss.length() > 32) ss = ss.substring(0, 32);
        }
        strncpy(cfg.wifi_ssid, ss.c_str(), sizeof(cfg.wifi_ssid) - 1);
        cfg.wifi_ssid[sizeof(cfg.wifi_ssid) - 1] = '\0';

        // WiFi password — empty string = open network (no password)
        String wp = "";
        if (request->hasParam("wifi_pass", true)) {
            wp = request->getParam("wifi_pass", true)->value();
            // Enforce WPA2 minimum 8 chars if a password is provided
            if (wp.length() > 0 && wp.length() < 8) {
                request->send(400, "application/json",
                    "{\"error\":\"WiFi password must be at least 8 characters, or leave blank for open network\"}");
                return;
            }
            if (wp.length() > 63) wp = wp.substring(0, 63);
        }
        strncpy(cfg.wifi_pass, wp.c_str(), sizeof(cfg.wifi_pass) - 1);
        cfg.wifi_pass[sizeof(cfg.wifi_pass) - 1] = '\0';

        save_can_config(cfg);
        if (g_serial_logging && g_serial_mode == 0) {
          Serial.printf("[CFG] Saved: CAN0=%s | CAN1=%s | preset=%u (%s). Rebooting...\n",
                        cfg.can0_enable ? "ON" : "OFF",
                        cfg.can1_enable ? "ON" : "OFF",
                        cfg.bus_preset, k_bus_presets[cfg.bus_preset].label);
        }
        request->send(200, "text/html",
            "<html><body style='font-family:monospace;background:#1a1a1a;color:#2ecc71;padding:40px'>"
            "<h2>Config saved.</h2><p>Rebooting in 1 second...</p>"
            "<script>setTimeout(()=>location.href='/',2500)</script></body></html>");
        delay(800);
        ESP.restart();
    });

    /* /api/set_speed and /api/get_speed — deprecated, use /api/set_config with bus_preset. */
    server.on("/api/set_speed", HTTP_POST, [](AsyncWebServerRequest *request) {
        request->send(410, "application/json",
            "{\"error\":\"set_speed is deprecated. Use /api/set_config with bus_preset.\"}");
    });

    server.on("/api/get_speed", HTTP_GET, [](AsyncWebServerRequest *request) {
        uint8_t p = g_cfg.bus_preset;
        String resp = "{\"preset\":" + String(p) +
                      ",\"label\":\"" + String(k_bus_presets[p].label) + "\"}";
        request->send(200, "application/json", resp);
    });

    // Start server
    server.begin();
    if (g_serial_logging && g_serial_mode == 0) { Serial.println("Web server started successfully"); }

    // API endpoint to toggle serial logging in real time
    server.on("/api/serial_logging", HTTP_GET, [](AsyncWebServerRequest *request) {
        String resp = "{\"serial_logging\":" + String(g_serial_logging ? "true" : "false") + "}";
        request->send(200, "application/json", resp);
    });
    server.on("/api/serial_logging", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (request->hasParam("enabled", true)) {
            g_serial_logging = (request->getParam("enabled", true)->value() == "1");
        } else {
            // No body param — toggle
            g_serial_logging = !g_serial_logging;
        }
        if (g_serial_logging) {
            if (g_serial_mode == 1) {
                // CSV mode — print header so the saved file is directly importable
                Serial.println("Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8");
            } else if (g_serial_mode == 0) {
                // Diagnostics mode — announce so the user knows output is live
                Serial.println("# CAN Diagnostics logging started");
            }
            // Binary GVRET (mode 2): no text header — binary protocol only
        }
        String resp = "{\"serial_logging\":" + String(g_serial_logging ? "true" : "false") + "}";
        request->send(200, "application/json", resp);
    });

    // API endpoint to get/toggle serial log format
    server.on("/api/serial_log_format", HTTP_GET, [](AsyncWebServerRequest *request) {
        String resp = "{\"gvret\":" + String(g_serial_log_gvret ? "true" : "false") + "}";
        request->send(200, "application/json", resp);
    });
    server.on("/api/serial_log_format", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (request->hasParam("gvret", true)) {
            g_serial_log_gvret = (request->getParam("gvret", true)->value() == "1");
        } else {
            g_serial_log_gvret = !g_serial_log_gvret;
        }
        String resp = "{\"gvret\":" + String(g_serial_log_gvret ? "true" : "false") + "}";
        request->send(200, "application/json", resp);
    });

    /* /api/set_serial_mode — change serial output mode immediately, no reboot.
       POST: serial_mode=0|1|2  (0=Diag, 1=CSV, 2=Binary GVRET)
       Saves to NVS so the mode survives a reboot. */
    server.on("/api/set_serial_mode", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!request->hasParam("serial_mode", true)) {
            request->send(400, "application/json", "{\"error\":\"Missing serial_mode\"}");
            return;
        }
        uint8_t m = (uint8_t)request->getParam("serial_mode", true)->value().toInt();
        if (m > 2) m = 1;
        g_serial_mode = m;
        // Persist so it survives reboot
        Preferences prefs;
        prefs.begin("can_cfg", false);
        prefs.putUChar("ser_mode", m);
        prefs.end();
        request->send(200, "application/json",
            "{\"serial_mode\":" + String(m) + "}");
    });
}

void CANWebServer::update() {
    // Send data to clients every 100ms
    unsigned long now = millis();
    if (now - last_update >= 100) {
        sendCANDataToClients();
        last_update = now;
    }

    // Monitor client queue status periodically
    monitorClientQueues();

    // Print statistics every 10 seconds
    if (now - last_stats_print >= 10000) {
        if (g_serial_logging && g_serial_mode == 0) { Serial.printf("Web Server - Clients: %u\n", client_count); }
        last_stats_print = now;
    }
}

void CANWebServer::onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                                   AwsEventType type, void *arg, uint8_t *data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT:
            client_count++;
            if (g_serial_logging && g_serial_mode == 0) {
              Serial.printf("WebSocket client #%u connected from %s\n",
                           client->id(), client->remoteIP().toString().c_str());
            }

            // Send initial lightweight data to new client
            if (can_bridge && can_bridge->getAnalysis()) {
                String json_data = get_can_ids_summary();
                client->text(json_data);
            }
            break;

        case WS_EVT_DISCONNECT:
            if (client_count > 0) client_count--;
            if (g_serial_logging && g_serial_mode == 0) { Serial.printf("WebSocket client #%u disconnected\n", client->id()); }
            break;

        case WS_EVT_DATA: {
            AwsFrameInfo *info = (AwsFrameInfo*)arg;
            if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
                data[len] = 0; // Null terminate
                String command = String((char*)data);
                processCommand(client, command);
            }
            break;
        }

        case WS_EVT_PONG:
        case WS_EVT_ERROR:
            break;
    }
}

void CANWebServer::processCommand(AsyncWebSocketClient *client, const String& command) {
    if (g_serial_logging && g_serial_mode == 0) { Serial.printf("WebSocket command: %s\n", command.c_str()); }

    if (command.startsWith("block:")) {
        // Block a CAN ID: "block:0x123"
        String can_id_str = command.substring(6);
        uint32_t can_id = strtoul(can_id_str.c_str(), nullptr, 16);

        if (can_bridge) {
            // Block at bridge level — this is what is_can_id_blocked() checks
            // and what the JSON summary "x" field reports back to the UI.
            can_bridge->block_can_id(can_id);

            if (can_bridge->getAnalysis()) {
                FilterRule rule;
                rule.can_id = can_id;
                rule.block = true;
                rule.active = true;
                can_bridge->getAnalysis()->addFilterRule(rule);
            }
            client->text("{\"status\":\"blocked\",\"id\":\"0x" + String(can_id, HEX) + "\"}");
        }
    }
    else if (command.startsWith("unblock:")) {
        // Unblock a CAN ID: "unblock:0x123"
        String can_id_str = command.substring(8);
        uint32_t can_id = strtoul(can_id_str.c_str(), nullptr, 16);

        if (can_bridge) {
            can_bridge->unblock_can_id(can_id);

            if (can_bridge->getAnalysis()) {
                can_bridge->getAnalysis()->removeFilterRule(can_id);
            }
            client->text("{\"status\":\"unblocked\",\"id\":\"0x" + String(can_id, HEX) + "\"}");
        }
    }
    else if (command.startsWith("send:")) {
        // Send custom message: "send:0x123:DEADBEEF"
        int first_colon = command.indexOf(':', 5);
        if (first_colon > 0) {
            String can_id_str = command.substring(5, first_colon);
            String payload_str = command.substring(first_colon + 1);

            uint32_t can_id = strtoul(can_id_str.c_str(), nullptr, 16);

            // Convert hex string to bytes
            uint8_t payload[8] = {0};
            int payload_len = (int)min((unsigned int)(payload_str.length() / 2), (unsigned int)8);

            for (int i = 0; i < payload_len; i++) {
                String byte_str = payload_str.substring(i * 2, i * 2 + 2);
                payload[i] = strtoul(byte_str.c_str(), nullptr, 16);
            }

            // Create CAN frame and forward it
            if (can_bridge) {
                CANFDMessage frame;
                frame.id = can_id;
                frame.len = payload_len;
                for (int i = 0; i < payload_len; i++) {
                    frame.data[i] = payload[i];
                }
                can_bridge->forwardMessage(frame);
                client->printf("Sent message: 0x%03X [%s]", can_id, payload_str.c_str());
            }
        }
    }
    else if (command == "clear") {
        if (can_bridge && can_bridge->getAnalysis()) {
            if (g_serial_logging && g_serial_mode == 0) { Serial.println("Clear command received - would reset analysis data"); }
            client->text("Data cleared");
        }
    }
    else if (command == "snapshot") {
        if (can_bridge && can_bridge->getAnalysis()) {
            // Send lightweight summary for snapshot
            String snapshot = get_can_ids_summary();
            client->text(snapshot);
        }
    }
    else if (command.startsWith("detail:")) {
        // Get detailed payload for specific CAN ID: "detail:0x123"
        String can_id_str = command.substring(7);
        uint32_t can_id = strtoul(can_id_str.c_str(), nullptr, 16);
        
        String detail = get_selected_payload_detail(can_id);
        client->text(detail);
    }
}

void CANWebServer::sendCANDataToClients() {
    if (can_bridge && can_bridge->getAnalysis() && client_count > 0) {
        // Use lightweight ID summary instead of full data for WebSocket
        String json_data = get_can_ids_summary();
        
        // Add debug logging
        // Serial.printf("Sending WebSocket data: %s\n", json_data.c_str());
        
        // Check each client individually and drop data if their queue is full
        ws.cleanupClients();
        
        // Use manual iteration to avoid copy constructor issues
        for (uint32_t i = 0; i < ws.count(); i++) {
            AsyncWebSocketClient* client = ws.client(i);
            if (client && client->status() == WS_CONNECTED) {
                // Check if client's send queue is getting full
                size_t queue_length = client->queueLen();
                const size_t MAX_QUEUE_SIZE = 3; // Allow max 3 queued messages
                
                if (queue_length <= MAX_QUEUE_SIZE) {
                    client->text(json_data);
                } else {
                    if (g_serial_logging && g_serial_mode == 0) { Serial.printf("Dropping packet for client %u (queue: %d)\n", client->id(), queue_length); }
                    client->text("{\"dropped\":true}");
                }
            }
        }
    }
}

void CANWebServer::monitorClientQueues() {
    static unsigned long last_monitor = 0;
    unsigned long now = millis();
    
    // Monitor every 5 seconds
    if (now - last_monitor > 5000) {
        last_monitor = now;
        
        if (client_count > 0) {
            if (g_serial_logging && g_serial_mode == 0) {
              Serial.printf("WebSocket Client Queue Status:\n");
              for (uint32_t i = 0; i < ws.count(); i++) {
                  AsyncWebSocketClient* client = ws.client(i);
                  if (client && client->status() == WS_CONNECTED) {
                      size_t queue_length = client->queueLen();
                      Serial.printf("  Client %u: Queue=%d\n", client->id(), queue_length);
                  }
              }
            }
        }
    }
}

const char* CANWebServer::generateMainPage() {
    static const char content[] = R"html(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>CAN Reverse Engineering Tool</title>
    <link rel="stylesheet" href="/style.css">
</head>
<body>
    <div class="header-bar">
        <div class="header-left">
            <span class="tool-name">CAN REVERSE ENGINEERING</span>
            <span class="status">
                <span id="connection-status">○</span> WebSocket
            </span>
            <span class="bus-info">CAN0: 500k</span>
        </div>
        <div class="header-right">
            <span class="status">
                <span id="can-data-status">●</span> CAN Data
            </span>
            <span class="message-count">Messages: <span id="total-messages">0</span></span>
            <button id="serial-log-btn" class="btn small" onclick="toggleSerialLogging()" style="background:#e67e22;border-color:#e67e22">⬤ Serial Log</button>
            <button id="theme-btn" class="btn small secondary" onclick="toggleTheme()">☀ Theme</button>
            <a href="/config" class="btn small secondary" style="text-decoration:none">⚙ Config</a>
        </div>
    </div>

    <div class="control-panel">
        <div class="control-row">
            <input type="text" class="filter-input" placeholder="Filter by ID or description...">
            <button class="btn small secondary" id="clear-btn">Clear</button>
            <button class="btn small primary" id="export-btn">Export</button>
        </div>
    </div>

    <div class="main-container">
        <!-- CAN0 Messages (Left Panel) -->
        <div class="left-panel">
            <div class="panel-header">
                <h3>� CAN0 Messages (CAN0→CAN1)</h3>
                <div class="panel-controls">
                    <button class="btn small warning" id="block-can0-btn" title="Block selected CAN0 message">🚫 Block</button>
                    <button class="btn small secondary" id="pin-can0-btn" title="Pin selected CAN0 message">📌 Pin</button>
                </div>
            </div>

            <div class="sort-controls">
                <button class="sort-btn active" data-sort="id" data-bus="can0">ID</button>
                <button class="sort-btn" data-sort="period" data-bus="can0">Period</button>
                <button class="sort-btn" data-sort="count" data-bus="can0">Count</button>
                <button class="sort-btn" data-sort="activity" data-bus="can0">Activity</button>
            </div>

            <div class="message-section">
                <h4>📌 Pinned CAN0</h4>
                <div class="message-list" id="can0-pinned-list"></div>
            </div>

            <div class="message-section">
                <h4>� Active CAN0</h4>
                <div class="message-list" id="can0-active-list"></div>
            </div>
        </div>

        <!-- Center Analysis Panel -->
        <div class="center-panel">
            <div class="analysis-header">
                <h2>Message Analysis</h2>
                <span id="selected-source">No Selection</span>
            </div>

            <div id="no-selection" style="display: block; text-align: center; color: var(--text-dim); padding: 40px;">
                Select a message from CAN0 (left) or CAN1 (right) to analyze
            </div>

            <div class="message-details" style="display: none;">
                <div class="detail-stats">
                    <div>ID: <span id="selected-id">-</span></div>
                    <div>Source: <span id="selected-bus">-</span></div>
                    <div>DLC: <span id="selected-dlc">-</span></div>
                    <div>Period: <span id="selected-period">-</span></div>
                    <div>Count: <span id="selected-count">-</span></div>
                    <div>Status: <span id="selected-status">Active</span></div>
                </div>

                <div class="decoded-signals" id="decoded-signals" style="display:none;">
                    <h3>📊 Decoded Signals</h3>
                    <div class="sig-list" id="sig-list"></div>
                </div>

                <div class="payload-analysis">
                    <h3>🔍 Payload Analysis</h3>
                    <div class="hex-viewer">
                        <div class="hex-data" id="hex-data">
                            <div class="hex-labels">
                                Byte:<br>
                                Curr:<br>
                                Prev:<br>
                                Diff:
                            </div>
                            <div>
                                <div class="hex-row labels">
                                    <span>0</span><span>1</span><span>2</span><span>3</span><span>4</span><span>5</span><span>6</span><span>7</span>
                                </div>
                                <div class="hex-row current">
                                    <span>00</span><span>00</span><span>00</span><span>00</span><span>00</span><span>00</span><span>00</span><span>00</span>
                                </div>
                                <div class="hex-row previous">
                                    <span>00</span><span>00</span><span>00</span><span>00</span><span>00</span><span>00</span><span>00</span><span>00</span>
                                </div>
                                <div class="hex-row diff">
                                    <span>--</span><span>--</span><span>--</span><span>--</span><span>--</span><span>--</span><span>--</span><span>--</span>
                                </div>
                            </div>
                        </div>
                    </div>
                </div>

                <div class="testing-controls">
                    <h3>🧪 Testing Controls</h3>
                    <div class="control-buttons">
                        <button class="btn small secondary" id="capture-btn">Capture</button>
                    </div>
                    <div class="custom-payload">
                        <div class="payload-editor">
                            <input type="text" class="byte-input" maxlength="2" value="00">
                            <input type="text" class="byte-input" maxlength="2" value="00">
                            <input type="text" class="byte-input" maxlength="2" value="00">
                            <input type="text" class="byte-input" maxlength="2" value="00">
                            <input type="text" class="byte-input" maxlength="2" value="00">
                            <input type="text" class="byte-input" maxlength="2" value="00">
                            <input type="text" class="byte-input" maxlength="2" value="00">
                            <input type="text" class="byte-input" maxlength="2" value="00">
                        </div>
                        <div class="payload-actions">
                            <button class="btn small primary" id="send-custom">Send</button>
                            <button class="btn small secondary" id="copy-payload">Copy</button>
                        </div>
                    </div>
                    <div class="status-display">
                        <span>Status: <span id="test-status">Ready</span></span>
                        <span>Sent: <span id="sent-count">0</span></span>
                    </div>
                </div>
            </div>
        </div>

        <!-- CAN1 Messages (Right Panel) -->
        <div class="right-panel">
            <div class="panel-header">
                <h3>� CAN1 Messages (CAN1→CAN0)</h3>
                <div class="panel-controls">
                    <button class="btn small warning" id="block-can1-btn" title="Block selected CAN1 message">🚫 Block</button>
                    <button class="btn small secondary" id="pin-can1-btn" title="Pin selected CAN1 message">📌 Pin</button>
                </div>
            </div>

            <div class="sort-controls">
                <button class="sort-btn active" data-sort="id" data-bus="can1">ID</button>
                <button class="sort-btn" data-sort="period" data-bus="can1">Period</button>
                <button class="sort-btn" data-sort="count" data-bus="can1">Count</button>
                <button class="sort-btn" data-sort="activity" data-bus="can1">Activity</button>
            </div>

            <div class="message-section">
                <h4>� Pinned CAN1</h4>
                <div class="message-list" id="can1-pinned-list"></div>
            </div>

            <div class="message-section">
                <h4>🔄 Active CAN1</h4>
                <div class="message-list" id="can1-active-list"></div>
            </div>
        </div>
    </div>

    <script src="/script.js"></script>
</body>
</html>)html";
    return content;
}

const char* CANWebServer::generateCSS() {
    static const char content[] = R"css(
:root {
    --bg: #1a1a1a;
    --text: #e0e0e0;
    --text-secondary: #bdc3c7;
    --text-dim: #95a5a6;
    --text-on-header: #ecf0f1;
    --card: #2a2a2a;
    --panel: #252525;
    --panel-header: #34495e;
    --border: #34495e;
    --control-bg: #2c3e50;
    --input-bg: #2c3e50;
    --input-focus-bg: #34495e;
    --panel-section-bg: rgba(52, 73, 94, 0.1);
    --hex-viewer-bg: rgba(44, 62, 80, 0.3);
    --overlay: rgba(0, 0, 0, 0.2);
    --overlay-light: rgba(0, 0, 0, 0.1);
    --scrollbar-track: #2c3e50;
    --scrollbar-thumb: #34495e;
    --scrollbar-thumb-hover: #4a6a7c;
}
[data-theme="light"] {
    --bg: #f5f5f5;
    --text: #1a1a1a;
    --text-secondary: #444444;
    --text-dim: #666666;
    --text-on-header: #1a1a1a;
    --card: #ffffff;
    --panel: #eeeeee;
    --panel-header: #cccccc;
    --border: #c0c0c0;
    --control-bg: #e0e0e0;
    --input-bg: #ffffff;
    --input-focus-bg: #f0f0f0;
    --panel-section-bg: rgba(0, 0, 0, 0.03);
    --hex-viewer-bg: rgba(0, 0, 0, 0.05);
    --overlay: rgba(0, 0, 0, 0.05);
    --overlay-light: rgba(0, 0, 0, 0.03);
    --scrollbar-track: #e0e0e0;
    --scrollbar-thumb: #b0b0b0;
    --scrollbar-thumb-hover: #888888;
}
* {
    margin: 0;
    padding: 0;
    box-sizing: border-box;
}

body {
    font-family: 'Monaco', 'Menlo', 'Source Code Pro', monospace;
    font-size: 12px;
    background: var(--bg);
    color: var(--text);
    line-height: 1.4;
    overflow-x: hidden;
}

.header-bar {
    position: fixed;
    top: 0;
    left: 0;
    right: 0;
    height: 40px;
    background: linear-gradient(135deg, #2c3e50, #34495e);
    border-bottom: 2px solid #3498db;
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 0 20px;
    z-index: 1000;
    font-size: 11px;
    font-weight: 600;
}

.header-left {
    display: flex;
    gap: 15px;
    align-items: center;
}

.header-right {
    display: flex;
    gap: 15px;
    align-items: center;
}

.tool-name {
    color: #3498db;
    font-weight: 700;
    font-size: 13px;
}

.status {
    color: #2ecc71;
    background: rgba(46, 204, 113, 0.1);
    padding: 2px 8px;
    border-radius: 4px;
    border: 1px solid #2ecc71;
}

.bus-info {
    color: #f39c12;
    background: rgba(243, 156, 18, 0.1);
    padding: 2px 8px;
    border-radius: 4px;
}

#connection-status {
    color: #2ecc71;
    font-size: 16px;
}

#can-data-status {
    color: #2ecc71;
    font-size: 16px;
}

.message-count {
    color: #2ecc71;
    background: rgba(46, 204, 113, 0.1);
    padding: 2px 8px;
    border-radius: 4px;
    border: 1px solid #2ecc71;
    font-size: 10px;
}

.control-panel {
    position: fixed;
    top: 40px;
    left: 0;
    right: 0;
    background: var(--control-bg);
    border-bottom: 1px solid var(--border);
    padding: 10px 20px;
    z-index: 999;
}

.control-row {
    display: flex;
    align-items: center;
    gap: 10px;
    margin-bottom: 8px;
}

.control-row:last-child {
    margin-bottom: 0;
}


.main-container {
    position: fixed;
    top: 90px;
    left: 0;
    right: 0;
    bottom: 0;
    display: flex;
    gap: 2px;
    background: var(--bg);
    height: calc(100vh - 90px);
}

.left-panel,
.center-panel,
.right-panel {
    height: 100%;
    overflow-y: auto;
    padding: 10px;
    display: flex;
    flex-direction: column;
}

.left-panel {
    width: 30%;
    background: var(--panel);
    border-right: 2px solid var(--border);
}

.center-panel {
    width: 40%;
    background: var(--card);
    border-right: 2px solid var(--border);
}

.right-panel {
    width: 30%;
    background: var(--panel);
}

.panel-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 10px;
    padding: 8px 12px;
    background: var(--panel-header);
    border-radius: 6px;
}

.panel-header h3 {
    margin: 0;
    font-size: 11px;
    color: var(--text-on-header);
}

.panel-controls {
    display: flex;
    gap: 6px;
}

.panel-controls .btn {
    font-size: 9px;
    padding: 3px 6px;
}

.message-section {
    margin-bottom: 15px;
}

.message-section h4 {
    font-size: 10px;
    color: var(--text-dim);
    margin: 8px 0 4px 0;
    padding: 4px 8px;
    background: var(--overlay);
    border-radius: 3px;
}

.panel-section {
    margin-bottom: 20px;
    border: 1px solid var(--border);
    border-radius: 6px;
    background: var(--panel-section-bg);
}

.panel-section h3 {
    background: var(--panel-header);
    color: var(--text-on-header);
    padding: 8px 12px;
    margin: 0;
    font-size: 11px;
    font-weight: 600;
    border-radius: 6px 6px 0 0;
}

.message-list {
    flex: 1;
    overflow-y: auto;
    padding: 8px;
    min-height: 0;
}

.message-item {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 6px 8px;
    margin-bottom: 2px;
    background: rgba(255, 255, 255, 0.02);
    border: 1px solid transparent;
    border-radius: 4px;
    cursor: pointer;
    transition: all 0.2s;
    font-size: 11px;
}

.message-item:hover {
    background: rgba(52, 152, 219, 0.1);
    border-color: #3498db;
}

.message-item.selected {
    background: rgba(52, 152, 219, 0.2);
    border-color: #3498db;
}

.message-item.pinned {
    background: rgba(241, 196, 15, 0.1);
    border-color: #f1c40f;
}

.message-item.changed {
    background: rgba(231, 76, 60, 0.1);
    border-color: #e74c3c;
    animation: pulse 1s ease-in-out;
}

@keyframes pulse {
    0%, 100% { opacity: 1; }
    50% { opacity: 0.7; }
}

.message-direction {
    font-weight: bold;
    font-size: 14px;
    width: 20px;
    text-align: center;
}

.message-direction.can0-to-can1 {
    color: #e74c3c; /* Red - CAN0 to CAN1 */
}

.message-direction.can1-to-can0 {
    color: #27ae60; /* Green - CAN1 to CAN0 */
}

.message-direction.bidirectional {
    color: #f39c12; /* Orange - Bidirectional */
}

.message-direction.unknown {
    color: #7f8c8d; /* Gray - Unknown */
}

/* Bus-specific message styling */
.left-panel .message-item {
    border-left: 3px solid #e74c3c; /* Red border for CAN0 messages */
}

.right-panel .message-item {
    border-left: 3px solid #27ae60; /* Green border for CAN1 messages */
}

.left-panel .message-item:hover {
    background: rgba(231, 76, 60, 0.1);
}

.right-panel .message-item:hover {
    background: rgba(39, 174, 96, 0.1);
}

.left-panel .message-item.selected {
    background: rgba(231, 76, 60, 0.2);
    border-color: #e74c3c;
}

.right-panel .message-item.selected {
    background: rgba(39, 174, 96, 0.2);
    border-color: #27ae60;
}

.no-messages {
    text-align: center;
    color: #7f8c8d;
    padding: 20px;
    font-style: italic;
}

.message-item.blocked {
    background: rgba(231, 76, 60, 0.15);
    border-left-color: #e74c3c;
}

.direction-legend {
    font-size: 12px;
}

.legend-item {
    display: flex;
    align-items: center;
    margin-bottom: 4px;
    gap: 8px;
}

/* Hex Viewer Styles */
.payload-analysis {
    margin: 15px 0;
}

.hex-viewer {
    background: var(--hex-viewer-bg);
    border-radius: 6px;
    padding: 12px;
    margin: 10px 0;
}

.hex-data {
    display: flex;
    gap: 10px;
    font-family: 'Courier New', monospace;
    font-size: 11px;
}

.hex-labels {
    color: var(--text-dim);
    text-align: right;
    padding-right: 8px;
    border-right: 1px solid var(--border);
    min-width: 40px;
    line-height: 18px;
}

.hex-row {
    display: flex;
    gap: 4px;
    margin-bottom: 2px;
    line-height: 18px;
}

.hex-row span {
    display: inline-block;
    width: 30px;
    text-align: center;
    padding: 1px;
    border-radius: 2px;
}

.hex-row.labels span {
    color: #7f8c8d;
    font-weight: bold;
}

.hex-row.current span {
    background: rgba(52, 152, 219, 0.1);
    border: 1px solid rgba(52, 152, 219, 0.3);
}

.binary-byte {
    font-size: 8px !important;
    width: 30px !important;
    letter-spacing: 0.5px;
    color: var(--text-dim);
}

.change-bits {
    font-size: 8px !important;
    width: 30px !important;
    letter-spacing: 0.5px;
}

.message-id {
    font-family: 'Monaco', monospace;
    color: #3498db;
    font-weight: 600;
    min-width: 60px;
}

.message-desc {
    color: var(--text-secondary);
    flex-grow: 1;
    margin-left: 8px;
    font-size: 10px;
}

.message-period {
    color: #f39c12;
    font-weight: 600;
    min-width: 50px;
    text-align: right;
}

.message-pin {
    color: #f1c40f;
    cursor: pointer;
    padding: 2px;
    margin-left: 8px;
}

.message-block {
    color: #e74c3c;
    cursor: pointer;
    padding: 2px;
    margin-left: 4px;
    transition: all 0.2s;
}

.message-block:hover {
    transform: scale(1.2);
}

.sort-controls {
    display: flex;
    gap: 4px;
    padding: 8px;
    background: var(--overlay);
}

.sort-btn {
    padding: 4px 8px;
    background: transparent;
    border: 1px solid var(--border);
    color: var(--text-secondary);
    border-radius: 4px;
    cursor: pointer;
    font-size: 10px;
    transition: all 0.2s;
}

.sort-btn:hover {
    border-color: #3498db;
    color: #3498db;
}

.sort-btn.active {
    background: #3498db;
    color: white;
    border-color: #3498db;
}

.stat-grid {
    padding: 8px;
    display: grid;
    grid-template-columns: 1fr;
    gap: 4px;
}

.stat-item {
    display: flex;
    justify-content: space-between;
    padding: 4px 0;
    font-size: 10px;
    border-bottom: 1px solid var(--border);
    opacity: 0.7;
}

.stat-item:last-child {
    border-bottom: none;
}

.stat-item .label {
    color: var(--text-dim);
}

.analysis-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 15px;
    padding-bottom: 10px;
    border-bottom: 2px solid var(--border);
}

.analysis-header h2 {
    color: #3498db;
    font-size: 14px;
    margin: 0;
}

.message-details {
    display: block;
}

.message-details.hidden {
    display: none;
}

.detail-stats {
    display: flex;
    gap: 20px;
    margin-bottom: 15px;
    padding: 8px;
    background: var(--overlay);
    border-radius: 4px;
    font-size: 11px;
}

.detail-stats span {
    color: #f39c12;
    font-weight: 600;
}

.payload-analysis {
    margin-bottom: 20px;
    border: 1px solid var(--border);
    border-radius: 6px;
    background: var(--overlay-light);
}

.payload-analysis h3 {
    background: var(--panel-header);
    color: var(--text-on-header);
    padding: 8px 12px;
    margin: 0;
    font-size: 11px;
    border-radius: 6px 6px 0 0;
}

.hex-viewer {
    padding: 12px;
    font-family: 'Monaco', monospace;
    font-size: 12px;
}

.hex-data {
    display: grid;
    grid-template-columns: auto 1fr;
    gap: 8px;
    align-items: center;
}

.hex-row {
    display: flex;
    gap: 8px;
    padding: 4px 0;
    font-family: 'Monaco', monospace;
}

.hex-row.labels {
    color: var(--text-dim);
    font-weight: 600;
}

.hex-row.current {
    color: #2ecc71;
    font-weight: 600;
}

.hex-row.previous {
    color: var(--text-dim);
}

.hex-row.diff {
    color: #e74c3c;
    font-weight: 600;
}

.hex-labels {
    display: flex;
    flex-direction: column;
    gap: 4px;
    padding: 4px 0;
    min-width: 50px;
    font-size: 10px;
    color: #95a5a6;
    text-align: right;
}

.testing-controls {
    border: 1px solid var(--border);
    border-radius: 6px;
    background: var(--overlay-light);
}

.testing-controls h3 {
    background: var(--panel-header);
    color: var(--text-on-header);
    padding: 8px 12px;
    margin: 0;
    font-size: 11px;
    border-radius: 6px 6px 0 0;
}

.control-buttons {
    display: flex;
    gap: 8px;
    padding: 12px;
}

.custom-payload {
    padding: 0 12px 12px;
}

.payload-editor {
    display: flex;
    gap: 4px;
    margin-bottom: 8px;
}

.byte-input {
    width: 30px;
    padding: 4px;
    background: var(--input-bg);
    border: 1px solid var(--border);
    color: var(--text);
    text-align: center;
    font-family: 'Monaco', monospace;
    font-size: 11px;
    border-radius: 3px;
}

.byte-input:focus {
    outline: none;
    border-color: #3498db;
    background: var(--input-focus-bg);
}

.byte-input.modified {
    border-color: #f39c12;
    background: rgba(243, 156, 18, 0.2);
    box-shadow: 0 0 4px rgba(243, 156, 18, 0.3);
}

.btn.danger {
    background: #e74c3c;
    border-color: #c0392b;
}

.btn.danger:hover {
    background: #c0392b;
    border-color: #a93226;
}

.payload-actions {
    display: flex;
    gap: 8px;
}

.status-display {
    display: flex;
    gap: 20px;
    padding: 8px 12px;
    background: var(--overlay-light);
    border-top: 1px solid var(--border);
    font-size: 10px;
}

.activity-list {
    max-height: 200px;
    overflow-y: auto;
    padding: 8px;
    font-size: 10px;
}

.activity-item {
    padding: 4px 0;
    border-bottom: 1px solid var(--border);
    opacity: 0.7;
    color: var(--text-secondary);
}

.activity-item:last-child {
    border-bottom: none;
}

.activity-time {
    color: var(--text-dim);
    margin-right: 8px;
}

.mini-chart {
    padding: 8px;
    background: var(--overlay);
}

.mini-chart canvas {
    width: 100%;
    height: 80px;
    background: var(--overlay-light);
    border-radius: 4px;
}

.btn {
    padding: 6px 12px;
    border: none;
    border-radius: 4px;
    cursor: pointer;
    font-size: 11px;
    font-weight: 600;
    transition: all 0.2s;
    text-decoration: none;
    display: inline-block;
}

.btn.small {
    padding: 4px 8px;
    font-size: 10px;
}

.btn.primary {
    background: #3498db;
    color: white;
}

.btn.primary:hover {
    background: #2980b9;
}

.btn.secondary {
    background: #95a5a6;
    color: white;
}

.btn.secondary:hover {
    background: #7f8c8d;
}

.btn.danger {
    background: #e74c3c;
    color: white;
}

.btn.danger:hover {
    background: #c0392b;
}

.btn.warning {
    background: #f39c12;
    color: white;
}

.btn.warning:hover {
    background: #e67e22;
}

input[type="text"], input[type="number"], select {
    padding: 6px 8px;
    background: var(--input-bg);
    border: 1px solid var(--border);
    color: var(--text);
    border-radius: 4px;
    font-size: 11px;
}

input[type="text"]:focus, input[type="number"]:focus, select:focus {
    outline: none;
    border-color: #3498db;
    background: var(--input-focus-bg);
}

.action-input {
    min-width: 200px;
}

.filter-input {
    min-width: 120px;
}

input[type="checkbox"] {
    margin-right: 5px;
}

label {
    color: var(--text-secondary);
    font-size: 11px;
    cursor: pointer;
}

::-webkit-scrollbar {
    width: 8px;
    height: 8px;
}

::-webkit-scrollbar-track {
    background: var(--scrollbar-track);
}

::-webkit-scrollbar-thumb {
    background: var(--scrollbar-thumb);
    border-radius: 4px;
}

::-webkit-scrollbar-thumb:hover {
    background: var(--scrollbar-thumb-hover);
}

/* Decoded signal values panel */
.decoded-signals {
    margin-bottom: 20px;
    border: 1px solid var(--border);
    border-radius: 6px;
    background: var(--overlay-light);
}

.decoded-signals h3 {
    background: var(--panel-header);
    color: var(--text-on-header);
    padding: 8px 12px;
    margin: 0;
    font-size: 11px;
    border-radius: 6px 6px 0 0;
}

.sig-list {
    padding: 6px 12px;
}

.sig-row {
    display: flex;
    align-items: center;
    padding: 5px 0;
    border-bottom: 1px solid var(--border);
    font-size: 11px;
    gap: 6px;
}

.sig-row:last-child { border-bottom: none; }

.sig-name {
    color: var(--text-secondary);
    flex: 1;
    min-width: 0;
}

.sig-bar-wrap {
    width: 70px;
    height: 5px;
    background: var(--overlay);
    border-radius: 3px;
    flex-shrink: 0;
}

.sig-bar {
    height: 5px;
    background: #2ecc71;
    border-radius: 3px;
    transition: width 0.2s;
}

.sig-value {
    font-family: 'Monaco', 'Courier New', monospace;
    font-size: 12px;
    font-weight: bold;
    color: #2ecc71;
    text-align: right;
    min-width: 50px;
}

.sig-unit {
    color: var(--text-dim);
    font-size: 10px;
    min-width: 30px;
}

@media (max-width: 1200px) {
    .main-container {
        flex-direction: column;
    }

    .left-panel,
    .center-panel,
    .right-panel {
        width: 100%;
        height: auto;
        max-height: 400px;
    }
}

@media (max-width: 768px) {
    body {
        font-size: 11px;
    }

    .header-bar {
        flex-direction: column;
        height: auto;
        padding: 8px;
    }

    .control-panel {
        top: 60px;
    }

    .main-container {
        top: 140px;
    }

    .control-row {
        flex-wrap: wrap;
    }
}
)css";
    return content;
}

const char* CANWebServer::generateJavaScript() {
    static const char content[] = R"js(
const LEAF_CAN_IDS = {
    // sig fields: sb=startBit, bl=bitLength, mo=Motorola(true)/Intel(false),
    //             si=signed, sc=scale, of=offset, unit, min/max (optional, for bar)
    '0x1db': { desc: 'BMS Status (V/I/relay)', signals: [
        {name:'Current',     sb:7,  bl:11, mo:true,  si:true,  sc:0.5,  of:0,   unit:'A',  min:-500, max:500},
        {name:'Voltage',     sb:23, bl:10, mo:true,  si:false, sc:0.5,  of:0,   unit:'V',  min:200,  max:420},
        {name:'MainRelayON', sb:29, bl:1,  mo:false, si:false, sc:1,    of:0,   unit:''},
        {name:'Interlock',   sb:27, bl:1,  mo:false, si:false, sc:1,    of:0,   unit:''},
    ]},
    '0x1dc': { desc: 'BMS Charge Power Limits', signals: [
        {name:'DischargeLim',  sb:7,  bl:10, mo:true, si:false, sc:0.25, of:0,   unit:'kW', min:0, max:100},
        {name:'ChargeLim',     sb:13, bl:10, mo:true, si:false, sc:0.25, of:0,   unit:'kW', min:0, max:100},
        {name:'MaxChargerPwr', sb:19, bl:10, mo:true, si:false, sc:0.1,  of:-10, unit:'kW', min:0, max:50},
    ]},
    '0x1d4': { desc: 'VCM Charge Request (keepalive)' },
    '0x1f2': { desc: 'VCM Charging Status' },
    '0x55b': { desc: 'BMS SOC', signals: [
        {name:'SOC',          sb:7,  bl:10, mo:true,  si:false, sc:0.1, of:0, unit:'%', min:0, max:100},
        {name:'CapacityEmpty',sb:55, bl:1,  mo:false, si:false, sc:1,   of:0, unit:''},
    ]},
    '0x5bc': { desc: 'BMS GIDs / SOH (ZE0: also AvgTemp@byte3)', signals: [
        {name:'GIDs',        sb:7,  bl:10, mo:true,  si:false, sc:1, of:0,   unit:'GID', min:0, max:281},
        {name:'SOH',         sb:33, bl:7,  mo:false, si:false, sc:1, of:0,   unit:'%',  min:0, max:100},
        {name:'AvgTemp(ZE0)',sb:24, bl:8,  mo:false, si:false, sc:1, of:-40, unit:'\u00b0C', min:-40, max:60},
    ]},
    '0x5c0': { desc: 'BMS Temp/Heater (AZE0)' },
    '0x59e': { desc: 'BMS QC Capacity — LB_Full_Capacity_for_QC / LB_Remain_Capacity_for_QC' },
    '0x50b': { desc: 'VCM Wakeup/Sleep Command' },
    '0x50c': { desc: 'VCM Alive Counter' },
    '0x3b2': { desc: 'PDU CHAdeMO Charge Request' },
    '0x3b8': { desc: 'ZE1 BMS Keepalive (ZE1 only)' },
    '0x1ed': { desc: 'ZE1 BMS Max Charger Power (62kWh only)' },
    '0x5ec': { desc: 'ZE1 Keepalive 500ms' },
    '0x5c5': { desc: 'ZE1 Keepalive (clears U214E)' },
    '0x626': { desc: 'ZE1 Keepalive (clears U215B)' },
    '0x7bb': { desc: 'OBD BMS Diagnostic Response' },
    '0x79b': { desc: 'LeafSpy Polling Detect' },
    // CHAdeMO QC-CAN IDs
    '0x100': { desc: 'CHAdeMO EV\u2192EVSE: MaxBattV, MinChargeA, protocol' },
    '0x101': { desc: 'CHAdeMO EV\u2192EVSE: Max charge time, rated capacity' },
    '0x102': { desc: 'CHAdeMO EV\u2192EVSE: TargetV/A, status, ChargingRate%' },
    '0x108': { desc: 'CHAdeMO EVSE\u2192EV: AvailV/A, ThresholdV' },
    '0x109': { desc: 'CHAdeMO EVSE\u2192EV: OutputV/A, remaining time, status' },
    '0x200': { desc: 'CHAdeMO V2H: max discharge A, min discharge V' },
    // ZE1 CARCAN mirrors of CHAdeMO QC-CAN
    '0x3b9': { desc: 'ZE1 mirror \u2190 QC 0x100: CHAdeMO EV params' },
    '0x3bb': { desc: 'ZE1 mirror \u2190 QC 0x101: CHAdeMO EV timing' },
    '0x3bc': { desc: 'ZE1 mirror \u2190 QC 0x102: CHAdeMO EV status' },
    '0x3c8': { desc: 'ZE1 mirror \u2190 QC 0x108: CHAdeMO EVSE power' },
    '0x3c9': { desc: 'ZE1 mirror \u2190 QC 0x109: CHAdeMO EVSE status' },
    '0x3be': { desc: 'ZE1 mirror \u2190 QC 0x200: CHAdeMO V2H' },
    '0x4bc': { desc: 'ZE1 mirror \u2190 QC 0x700: CHAdeMO mfr optional' },
    // OBC IDs
    '0x380': { desc: 'ZE0 OBC Status (QC IR sensor, output power, QC relay, AC voltage)' },
    '0x5bf': { desc: 'ZE0 OBC J1772 Current Limiter' },
    '0x390': { desc: 'AZE0/ZE1 OBC (AC voltage type, QC relay, max charge power)' },
    '0x393': { desc: 'AZE0/ZE1 OBC Unknown' },
    // SPUD VCU \u2194 Dash messages (private CAN bus)
    '0x61a': { desc: 'SPUD Dash\u2192VCU: status, bmode, cruise, maxCharge' },
    '0x62a': { desc: 'SPUD VCU\u2192Dash: HV V/I, motor RPM, throttle, speed, VCU state, brake' },
    '0x62b': { desc: 'SPUD VCU\u2192Dash: relay states, gear, limp/cruise, LV V, SoC, charge rate, brake vac' },
    '0x62c': { desc: 'SPUD VCU\u2192Dash: odometer/hour meter, inv/motor/batt temps' },
    '0x62d': { desc: 'SPUD VCU\u2192Dash: min/max cell V, energy remaining, total capacity' },
    '0x63a': { desc: 'SPUD DTC Broadcast: lamp status, DTC type, DTC code' },
};

class CANAnalyzer {
    constructor() {
        this.socket = null;
        this.isConnected = false;
        this.lastCANMessageTime = 0;
        this.canDataActive = false;
        this.canData = new Map();
        this.pinnedMessages = new Set();
        this.selectedMessage = null;
        
        // Separate sorting state for each bus
        this.sortBy = {
            can0: 'id',
            can1: 'id'
        };
        this.sortDirection = {
            can0: 'asc',
            can1: 'asc'
        };
        this.lastUpdateTime = 0;
        this.activityLog = [];
        this.maxActivityItems = 100;
        this.payloadUpdateTimer = null;

        this.initWebSocket();
        this.setupEventHandlers();
        this.updateDisplay();
        
        // Set up periodic total message count refresh every 2 seconds
        setInterval(() => {
            this.refreshTotalMessageCount();
        }, 2000);

        // Set up CAN data status monitoring every 100ms
        setInterval(() => {
            this.updateCANDataStatus();
        }, 100);

        setInterval(() => this.updateDisplay(), 100);
    }

    initWebSocket() {
        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        const wsUrl = protocol + '//' + window.location.host + '/ws';

        this.socket = new WebSocket(wsUrl);

        this.socket.onopen = () => {
            this.isConnected = true;
            this.updateConnectionStatus();
            this.addActivity('WebSocket connected');
            // Request a fresh snapshot immediately on connect
            this.socket.send('snapshot');
        };

        this.socket.onmessage = (event) => {
            try {
                const data = JSON.parse(event.data);
                console.log('Received WebSocket data:', data); // Debug logging
                
                // Update CAN data timestamp
                this.lastCANMessageTime = Date.now();
                this.canDataActive = true;
                
                this.processCANData(data);
            } catch (e) {
                console.error('Error parsing WebSocket data:', e);
                console.log('Raw data:', event.data); // Debug logging
                // Don't try to process invalid JSON data
                return;
            }
        };

        this.socket.onclose = () => {
            this.isConnected = false;
            this.updateConnectionStatus();
            this.addActivity('WebSocket disconnected — reconnecting...');
            // Clear stale data so the display reflects the fresh state after reconnect
            this.canData.clear();
            this.updateMessageList();
            setTimeout(() => this.initWebSocket(), 1000);
        };

        this.socket.onerror = (error) => {
            console.error('WebSocket error:', error);
            this.addActivity('WebSocket error');
        };
    }

    setupEventHandlers() {
        document.querySelectorAll('.sort-btn').forEach(btn => {
            btn.onclick = () => this.setSortBy(btn.dataset.sort, btn.dataset.bus);
        });

        document.getElementById('clear-btn')?.addEventListener('click', () => this.clearData());
        document.getElementById('export-btn')?.addEventListener('click', () => this.exportData());
        document.getElementById('capture-btn')?.addEventListener('click', () => this.captureSnapshot());

        // Panel control buttons
        document.getElementById('block-can0-btn')?.addEventListener('click', () => this.blockSelectedMessage('can0'));
        document.getElementById('block-can1-btn')?.addEventListener('click', () => this.blockSelectedMessage('can1'));
        document.getElementById('pin-can0-btn')?.addEventListener('click', () => this.pinSelectedMessage('can0'));
        document.getElementById('pin-can1-btn')?.addEventListener('click', () => this.pinSelectedMessage('can1'));

        this.setupPayloadEditor();

        const filterInput = document.querySelector('.filter-input');
        if (filterInput) {
            filterInput.addEventListener('input', (e) => this.filterMessages(e.target.value));
        }

        // Event delegation for message list clicks
        const messageLists = document.querySelectorAll('.message-list');
        let lastHoveredItem = null;
        messageLists.forEach(messageList => {
            if (messageList) {
                // Make selection easier: respond to both click and mousedown
                const selectHandler = (e) => {
                    const messageItem = e.target.closest('.message-item');
                    if (!messageItem) return;

                    const messageId = messageItem.dataset.messageId;
                    const busType = messageItem.dataset.bus;
                    if (!messageId) return;

                    // Handle pin clicks
                    if (e.target.closest('.message-pin')) {
                        e.stopPropagation();
                        this.togglePin(messageId, e);
                        return;
                    }

                    // Handle block clicks
                    if (e.target.closest('.message-block')) {
                        e.stopPropagation();
                        this.toggleBlock(messageId, busType, e);
                        return;
                    }

                    // Handle message selection
                    this.selectMessage(messageId, busType);
                };
                messageList.addEventListener('click', selectHandler);
                messageList.addEventListener('mousedown', selectHandler);

                // Mouseover highlight (debounced, no flicker)
                messageList.addEventListener('mouseover', (e) => {
                    const item = e.target.closest('.message-item');
                    if (item && item !== lastHoveredItem) {
                        if (lastHoveredItem) lastHoveredItem.classList.remove('hovered');
                        item.classList.add('hovered');
                        lastHoveredItem = item;
                    }
                });
                messageList.addEventListener('mouseout', (e) => {
                    const item = e.target.closest('.message-item');
                    if (item && item === lastHoveredItem) {
                        item.classList.remove('hovered');
                        lastHoveredItem = null;
                    }
                });
            }
        });
    }

    setupPayloadEditor() {
        const byteInputs = document.querySelectorAll('.byte-input');
        byteInputs.forEach((input, index) => {
            input.addEventListener('input', (e) => {
                e.target.value = e.target.value.replace(/[^0-9A-Fa-f]/g, '').toUpperCase().slice(0, 2);
                
                // Visual feedback for modification
                e.target.classList.add('modified');
                setTimeout(() => {
                    e.target.classList.remove('modified');
                }, 1000);
            });

            input.addEventListener('keydown', (e) => {
                if (e.key === 'Enter' || e.key === 'Tab') {
                    const nextInput = byteInputs[index + 1];
                    if (nextInput) nextInput.focus();
                } else if (e.key === 'Escape') {
                    // Reset to original value on Escape
                    if (this.selectedMessage) {
                        const msg = this.canData.get(this.selectedMessage);
                        if (msg && msg.last_payload && msg.last_payload[index] !== undefined) {
                            e.target.value = msg.last_payload[index].toString(16).padStart(2, '0').toUpperCase();
                            this.addActivity('↶ Reset byte ' + index + ' to original value');
                        }
                    }
                }
            });

            // Add focus/blur handlers for better UX
            input.addEventListener('focus', (e) => {
                e.target.style.borderColor = '#3498db';
            });

            input.addEventListener('blur', (e) => {
                e.target.style.borderColor = '';
            });
        });

        document.getElementById('send-custom')?.addEventListener('click', () => this.sendCustomMessage());
        document.getElementById('copy-payload')?.addEventListener('click', () => this.copyCurrentPayload());
    }

    processCANData(data) {
        console.log('Processing CAN data:', data); // Debug logging
        
        // Handle compressed ID summary format
        if (data.ids) {
            console.log('Processing compressed format with', data.ids.length, 'IDs'); // Debug logging
            
            // Calculate total message count from all IDs
            let totalMessages = 0;
            data.ids.forEach(idInfo => {
                totalMessages += idInfo.c || 0; // Sum up count field from each ID
            });
            
            const stats = {
                totalMessages: totalMessages,
                activeIds: data.ids.length || 0,
                updateRate: 0
            };

            const now = Date.now();
            if (this.lastUpdateTime > 0) {
                const timeDiff = (now - this.lastUpdateTime) / 1000;
                stats.updateRate = Math.round(1 / timeDiff);
            }
            this.lastUpdateTime = now;

            this.updateStatistics(stats);

            let newMessages = 0;
            let changedMessages = 0;

            // Process compressed ID data
            data.ids.forEach(idInfo => {
                console.log('Processing ID:', idInfo.i, 'c0:', idInfo.c0, 'c1:', idInfo.c1); // Debug logging
                const existing = this.canData.get(idInfo.i);
                const isNew = !existing;

                if (isNew) {
                    newMessages++;
                    this.addActivity('New message: ' + idInfo.i);
                }

                // Create message object with compressed field names
                const canMsg = {
                    id: idInfo.i,
                    count: idInfo.c,
                    rate: idInfo.r,
                    can0_count: idInfo.c0 || 0,
                    can1_count: idInfo.c1 || 0,
                    last_bus: idInfo.b || 0,
                    blocked: idInfo.x === 1,
                    description: (LEAF_CAN_IDS[idInfo.i.toLowerCase()] || {}).desc || '',
                    period: idInfo.r,
                    data: existing ? existing.data : '', // Keep existing payload if available
                    length: existing ? existing.length : 0,
                    last_timestamp: existing ? existing.last_timestamp : Date.now()
                };

                if (canMsg.period > 0) {
                    canMsg.periodMs = Math.round(1000 / canMsg.period);
                }

                this.canData.set(canMsg.id, canMsg);
            });

            if (newMessages > 0 || changedMessages > 0) {
                this.addActivity(`Updated: +${newMessages} new, ${changedMessages} changed`);
            }

            // Update the display after processing compressed data
            this.updateMessageList();
            
            // Also refresh the total count in case WebSocket data is stale
            this.refreshTotalMessageCount();
        }

        // No data to process
        console.log('No valid data format found, skipping processing');
    }

    updateStatistics(stats) {
        // Update total message count in header
        const totalMsgEl = document.getElementById('total-messages');
        if (totalMsgEl) {
            totalMsgEl.textContent = stats.totalMessages;
        }
    }

    // Manual function to recalculate and update total message count
    refreshTotalMessageCount() {
        let totalMessages = 0;
        this.canData.forEach(msg => {
            totalMessages += (msg.can0_count || 0) + (msg.can1_count || 0);
        });
        
        const totalMsgEl = document.getElementById('total-messages');
        if (totalMsgEl) {
            totalMsgEl.textContent = totalMessages;
        }
    }

    updateMessageList() {
        console.log('Updating message lists, total messages:', this.canData.size); // Debug logging
        this.updateCAN0MessageList();
        this.updateCAN1MessageList();
    }

    updateCAN0MessageList() {
        const pinnedContainer = document.getElementById('can0-pinned-list');
        const activeContainer = document.getElementById('can0-active-list');
        if (!pinnedContainer || !activeContainer) {
            return;
        }

        const can0Messages = this.getCAN0Messages();
        const pinnedMessages = can0Messages.filter(msg => this.pinnedMessages.has(msg.id));
        const activeMessages = can0Messages.filter(msg => !this.pinnedMessages.has(msg.id));

        pinnedContainer.innerHTML = this.generateMessageHTML(pinnedMessages, 'can0');
        activeContainer.innerHTML = this.generateMessageHTML(activeMessages, 'can0');
    }

    updateCAN1MessageList() {
        const pinnedContainer = document.getElementById('can1-pinned-list');
        const activeContainer = document.getElementById('can1-active-list');
        if (!pinnedContainer || !activeContainer) {
            return;
        }

        const can1Messages = this.getCAN1Messages();
        const pinnedMessages = can1Messages.filter(msg => this.pinnedMessages.has(msg.id));
        const activeMessages = can1Messages.filter(msg => !this.pinnedMessages.has(msg.id));

        pinnedContainer.innerHTML = this.generateMessageHTML(pinnedMessages, 'can1');
        activeContainer.innerHTML = this.generateMessageHTML(activeMessages, 'can1');
    }

    getCAN0Messages() {
        const can0Messages = Array.from(this.canData.values())
            .filter(msg => msg.can0_count > 0);
        console.log('CAN0 messages found:', can0Messages.length); // Debug logging
        return can0Messages.sort((a, b) => this.compareMessages(a, b, 'can0'));
    }

    getCAN1Messages() {
        const can1Messages = Array.from(this.canData.values())
            .filter(msg => msg.can1_count > 0);
        console.log('CAN1 messages found:', can1Messages.length); // Debug logging
        return can1Messages.sort((a, b) => this.compareMessages(a, b, 'can1'));
    }

    compareMessages(a, b, bus) {
        let comparison = 0;
        switch (this.sortBy[bus]) {
            case 'id':
                comparison = parseInt(a.id, 16) - parseInt(b.id, 16);
                break;
            case 'period':
                comparison = (a.periodMs || 9999) - (b.periodMs || 9999);
                break;
            case 'count':
                comparison = (b.count || 0) - (a.count || 0);
                break;
            case 'activity':
                comparison = (b.lastSeen || 0) - (a.lastSeen || 0);
                break;
            default:
                comparison = parseInt(a.id, 16) - parseInt(b.id, 16);
        }
        return this.sortDirection[bus] === 'asc' ? comparison : -comparison;
    }

    generateMessageHTML(messages, busType) {
        if (messages.length === 0) {
            return '<div class="no-messages">No messages</div>';
        }

        let html = '';
        messages.forEach(msg => {
            const isSelected = this.selectedMessage === msg.id;
            const classes = ['message-item'];

            if (isSelected) classes.push('selected');
            if (msg.changed) classes.push('changed');
            if (msg.blocked) classes.push('blocked');

            const periodText = msg.periodMs ? msg.periodMs + 'ms' : (msg.period ? Math.round(msg.period) + 'Hz' : 'Static');

            html += '<div class="' + classes.join(' ') + '" data-message-id="' + msg.id + '" data-bus="' + busType + '">';
            html += '<span class="message-id">' + msg.id + '</span>';
            html += '<span class="message-desc">' + (msg.description || 'Unknown') + '</span>';
            html += '<span class="message-period">' + periodText + '</span>';
            html += '<span class="message-pin" data-pin-id="' + msg.id + '">';
            html += this.pinnedMessages.has(msg.id) ? '📌' : '📍';
            html += '</span>';
            html += '<span class="message-block" data-block-id="' + msg.id + '" title="Block this message">';
            html += msg.blocked ? '🚫' : '⭕';
            html += '</span></div>';
        });

        return html;
    }

    updateSelectedMessage() {
        console.log('Updating selected message:', this.selectedMessage); // Debug logging
        
        if (!this.selectedMessage) {
            // Clear timer when no message is selected
            if (this.payloadUpdateTimer) {
                clearInterval(this.payloadUpdateTimer);
                this.payloadUpdateTimer = null;
            }
            
            document.getElementById('no-selection').style.display = 'block';
            document.querySelectorAll('.message-details').forEach(el => el.style.display = 'none');
            return;
        }

        const msg = this.canData.get(this.selectedMessage);
        if (!msg) {
            console.log('Message not found in canData:', this.selectedMessage); // Debug logging
            return;
        }

        console.log('Message data for selected message:', msg); // Debug logging

        document.getElementById('no-selection').style.display = 'none';
        document.querySelectorAll('.message-details').forEach(el => el.style.display = 'block');

        document.getElementById('selected-id').textContent = msg.id;
        document.getElementById('selected-bus').textContent = this.selectedBus === 'can0' ? 'CAN0→CAN1' : 'CAN1→CAN0';
        document.getElementById('selected-dlc').textContent = msg.dlc || 0;
        document.getElementById('selected-period').textContent = msg.periodMs ? msg.periodMs + 'ms' : 'Static';
        document.getElementById('selected-count').textContent = (msg.can0_count || 0) + (msg.can1_count || 0);
        
        // Update status display
        const statusEl = document.getElementById('selected-status');
        if (statusEl) {
            if (msg.blocked) {
                statusEl.textContent = '🚫 Blocked';
                statusEl.style.color = '#e74c3c';
            } else {
                statusEl.textContent = '✅ Active';
                statusEl.style.color = '#2ecc71';
            }
        }

        // Decoded signal values (known IDs only)
        this.renderDecodedSignals(msg);
        // Bit-level visualization (hex viewer)
        this.updateHexViewer(msg);
        // Show payload editor with bit-level changes
        this.updatePayloadEditor(msg);
        // If you want to show a diff between previous and current payload, add here:
        // if (msg.prev_payload) {
        //   this.showBitDiff(msg.prev_payload, msg.last_payload);
        // }
        // (The updateHexViewer already shows bit-level changes)
    }

    updateHexViewer(msg) {
        console.log('Updating hex viewer for message:', msg.id, 'last_payload:', msg.last_payload); // Debug logging
        
        const hexData = document.querySelector('.hex-data');
        if (!hexData) {
            console.log('Hex data element not found'); // Debug logging
            return;
        }
        
        if (!msg.last_payload) {
            console.log('No last_payload data available for message:', msg.id); // Debug logging
            return;
        }

        const bytes = msg.last_payload || [];
        const changes = msg.changes || [];

        let html = '<div class="hex-labels">Byte:<br>Curr:<br>Bits:<br>Chng:</div><div>';

        // Byte index labels
        html += '<div class="hex-row labels">';
        for (let i = 0; i < 8; i++) {
            html += '<span>' + i + '</span>';
        }
        html += '</div>';

        // Current payload values
        html += '<div class="hex-row current">';
        for (let i = 0; i < 8; i++) {
            const byte = bytes[i] !== undefined ? bytes[i].toString(16).padStart(2, '0').toUpperCase() : '00';
            const hasChanges = changes[i] && changes[i] > 0;
            const style = hasChanges ? 'background: rgba(231, 76, 60, 0.3); color: #e74c3c; font-weight: bold;' : '';
            html += '<span style="' + style + '">' + byte + '</span>';
        }
        html += '</div>';

        // Binary representation of current byte
        html += '<div class="hex-row binary">';
        for (let i = 0; i < 8; i++) {
            const byte = bytes[i] !== undefined ? bytes[i] : 0;
            const binary = byte.toString(2).padStart(8, '0');
            html += '<span class="binary-byte">' + binary + '</span>';
        }
        html += '</div>';

        // Change indicators (which bits have ever changed)
        html += '<div class="hex-row changes">';
        for (let i = 0; i < 8; i++) {
            const changeMask = changes[i] || 0;
            let changeStr = '';
            for (let bit = 7; bit >= 0; bit--) {
                const hasChanged = (changeMask & (1 << bit)) !== 0;
                changeStr += hasChanged ? '●' : '○';
            }
            html += '<span class="change-bits" style="color: ' + (changeMask > 0 ? '#e74c3c' : '#7f8c8d') + '">' + changeStr + '</span>';
        }
        html += '</div>';

        html += '</div>';
        hexData.innerHTML = html;
    }

    updatePayloadEditor(msg) {
        const inputs = document.querySelectorAll('.byte-input');
        const bytes = msg.last_payload || [0, 0, 0, 0, 0, 0, 0, 0];
        const changes = msg.changes || [];

        inputs.forEach((input, index) => {
            if (index < bytes.length) {
                const byteValue = bytes[index].toString(16).padStart(2, '0').toUpperCase();
                input.value = byteValue;
                
                // Highlight changed bytes
                if (changes[index] && changes[index] > 0) {
                    input.classList.add('modified');
                } else {
                    input.classList.remove('modified');
                }
            } else {
                input.value = '00';
                input.classList.remove('modified');
            }
        });
    }

    decodeSignal(bytes, sig) {
        let raw = 0;
        if (sig.mo) {
            // Motorola big-endian: sig.sb is MSB position
            let cur = sig.sb;
            for (let i = 0; i < sig.bl; i++) {
                const byteIdx = Math.floor(cur / 8);
                const bitInByte = cur % 8; // 7=MSB, 0=LSB within byte
                if (byteIdx < bytes.length) raw = (raw << 1) | ((bytes[byteIdx] >> bitInByte) & 1);
                if (cur % 8 === 0) cur += 15; // wrap to MSB of next byte
                else cur -= 1;
            }
        } else {
            // Intel little-endian: sig.sb is LSB position
            for (let i = 0; i < sig.bl; i++) {
                const bit = sig.sb + i;
                const byteIdx = Math.floor(bit / 8);
                const bitInByte = bit % 8;
                if (byteIdx < bytes.length) raw |= ((bytes[byteIdx] >> bitInByte) & 1) << i;
            }
        }
        // Two's complement sign extension
        if (sig.si && (raw & (1 << (sig.bl - 1)))) raw -= (1 << sig.bl);
        return raw * (sig.sc || 1) + (sig.of || 0);
    }

    renderDecodedSignals(msg) {
        const container = document.getElementById('decoded-signals');
        if (!container) return;
        const known = LEAF_CAN_IDS[msg.id.toLowerCase()];
        if (!known || !known.signals || known.signals.length === 0 || !msg.last_payload) {
            container.style.display = 'none';
            return;
        }
        const bytes = msg.last_payload;
        let html = '';
        for (const sig of known.signals) {
            if (sig.sb === undefined) continue;
            const value = this.decodeSignal(bytes, sig);
            const displayVal = Number.isInteger(value) ? value : value.toFixed(2);
            let barHtml = '';
            if (sig.min !== undefined && sig.max !== undefined && sig.bl > 1) {
                const pct = Math.max(0, Math.min(100, (value - sig.min) / (sig.max - sig.min) * 100));
                barHtml = '<div class="sig-bar-wrap"><div class="sig-bar" style="width:' + pct.toFixed(1) + '%"></div></div>';
            }
            html += '<div class="sig-row"><span class="sig-name">' + sig.name + '</span>' +
                    barHtml +
                    '<span class="sig-value">' + displayVal + '</span>' +
                    '<span class="sig-unit">' + (sig.unit || '') + '</span></div>';
        }
        container.style.display = 'block';
        document.getElementById('sig-list').innerHTML = html;
    }

    getSortedMessages(bus = 'can0') {
        let messages = Array.from(this.canData.values());

        messages.sort((a, b) => {
            const aPinned = this.pinnedMessages.has(a.id);
            const bPinned = this.pinnedMessages.has(b.id);

            if (aPinned && !bPinned) return -1;
            if (!aPinned && bPinned) return 1;

            let comparison = 0;
            switch (this.sortBy[bus]) {
                case 'id':
                    comparison = parseInt(a.id, 16) - parseInt(b.id, 16);
                    break;
                case 'period':
                    comparison = (a.periodMs || 9999) - (b.periodMs || 9999);
                    break;
                case 'count':
                    comparison = (b.count || 0) - (a.count || 0);
                    break;
                case 'activity':
                    comparison = (b.lastSeen || 0) - (a.lastSeen || 0);
                    break;
            }

            return this.sortDirection[bus] === 'asc' ? comparison : -comparison;
        });

        return messages;
    }

    selectMessage(id, busType) {
        // Clear existing timer if any
        if (this.payloadUpdateTimer) {
            clearInterval(this.payloadUpdateTimer);
            this.payloadUpdateTimer = null;
        }
        
        this.selectedMessage = id;
        this.selectedBus = busType;
        
        // Update selected source display
        const sourceDisplay = document.getElementById('selected-source');
        if (sourceDisplay) {
            const busName = busType === 'can0' ? 'CAN0→CAN1' : 'CAN1→CAN0';
            sourceDisplay.textContent = `${id} (${busName})`;
        }
        
        // Request detailed payload data for selected message
        this.requestPayloadDetail(id);
        
        // Set up timer to refresh payload data every 250ms
        this.payloadUpdateTimer = setInterval(() => {
            if (this.selectedMessage === id) {
                this.requestPayloadDetail(id);
            }
        }, 250);
        
        // Update immediately for responsive UI
        this.updateSelectedMessage();
        
        // Delay message list update to avoid interference with click handling
        setTimeout(() => {
            this.updateMessageList();
        }, 10);
        
        this.addActivity('Selected message: ' + id + ' from ' + (busType === 'can0' ? 'CAN0' : 'CAN1'));
    }

    requestPayloadDetail(canId) {
        // Remove '0x' prefix if present for API call
        const idStr = canId.replace('0x', '');
        
        // Use HTTP API to get detailed payload data
        fetch(`/api/payload_detail?id=${idStr}`)
            .then(response => response.json())
            .then(data => {
                if (data.error) {
                    console.error('Error getting payload detail:', data.error);
                    this.addActivity('Error loading payload for ' + canId);
                    return;
                }
                
                // Update the message with detailed payload information
                const msg = this.canData.get(canId);
                if (msg) {
                    console.log('Updating message with payload detail:', data); // Debug logging
                    
                    msg.payload_detail = data.payload;
                    msg.analysis = data.analysis;
                    msg.length = data.payload.length;
                    msg.dlc = data.payload.length; // Add DLC field
                    
                    // Convert bytes array to hex string for display compatibility
                    if (data.payload.bytes) {
                        msg.data = data.payload.bytes.map(b => b.replace('0x', '')).join('');
                        // Also set last_payload as array for hex viewer
                        msg.last_payload = data.payload.bytes.map(b => parseInt(b, 16));
                        console.log('Set last_payload:', msg.last_payload); // Debug logging
                    }
                    
                    // Add changes array if available
                    if (data.analysis) {
                        msg.changes = data.analysis.map(item => parseInt(item.changes || '0x0', 16));
                        console.log('Set changes:', msg.changes); // Debug logging
                    }
                    
                    // Update the selected message display with new detailed data
                    this.updateSelectedMessage();
                    
                    // Refresh total message count since we got fresh data
                    this.refreshTotalMessageCount();
                    
                    this.addActivity('Loaded detailed payload for ' + canId);
                } else {
                    console.log('Message not found in canData when updating payload detail:', canId); // Debug logging
                }
            })
            .catch(error => {
                console.error('Error fetching payload detail:', error);
                this.addActivity('Network error loading payload for ' + canId);
            });
    }

    togglePin(id, event) {
        if (event) {
            event.stopPropagation();
        }

        if (this.pinnedMessages.has(id)) {
            this.pinnedMessages.delete(id);
            this.addActivity('Unpinned message: ' + id);
        } else {
            this.pinnedMessages.add(id);
            this.addActivity('Pinned message: ' + id);
        }

        // Delay update to avoid interference with event handling
        setTimeout(() => {
            this.updateMessageList();
        }, 10);
    }

    toggleBlock(id, busType, event) {
        if (event) {
            event.stopPropagation();
        }

        const msg = this.canData.get(id);
        if (!msg) return;

        if (msg.blocked) {
            msg.blocked = false;
            this.addActivity('🟢 Unblocked message: ' + id + ' on ' + busType.toUpperCase());
            // Send unblock command (need to implement backend support for unblock)
            if (this.socket && this.socket.readyState === WebSocket.OPEN) {
                this.socket.send('unblock:' + id);
            }
        } else {
            msg.blocked = true;
            this.addActivity('🚫 Blocked message: ' + id + ' on ' + busType.toUpperCase());
            // Send block command using the format the backend expects
            if (this.socket && this.socket.readyState === WebSocket.OPEN) {
                this.socket.send('block:' + id);
            }
        }

        // Delay update to avoid interference with event handling
        setTimeout(() => {
            this.updateMessageList();
        }, 10);
    }

    blockSelectedMessage(busType) {
        if (!this.selectedMessage || this.selectedBus !== busType) {
            this.addActivity('⚠️ Please select a message from ' + busType.toUpperCase() + ' first');
            return;
        }

        const msg = this.canData.get(this.selectedMessage);
        if (!msg) return;

        if (!msg.blocked) {
            msg.blocked = true;
            this.addActivity('🚫 Blocked selected message: ' + this.selectedMessage + ' from ' + busType.toUpperCase());
            // Send block command using the format the backend expects
            if (this.socket && this.socket.readyState === WebSocket.OPEN) {
                this.socket.send('block:' + this.selectedMessage);
            }
        } else {
            this.addActivity('⚠️ Message ' + this.selectedMessage + ' is already blocked');
        }

        this.updateMessageList();
        this.updateSelectedMessage();
    }

    pinSelectedMessage(busType) {
        if (!this.selectedMessage || this.selectedBus !== busType) {
            this.addActivity('⚠️ Please select a message from ' + busType.toUpperCase() + ' first');
            return;
        }

        this.togglePin(this.selectedMessage);
    }

    setSortBy(criterion, bus) {
        if (this.sortBy[bus] === criterion) {
            this.sortDirection[bus] = this.sortDirection[bus] === 'asc' ? 'desc' : 'asc';
        } else {
            this.sortBy[bus] = criterion;
            this.sortDirection[bus] = 'asc';
        }

        // Update only the buttons for this specific bus
        document.querySelectorAll(`[data-bus="${bus}"]`).forEach(btn => {
            btn.classList.remove('active');
        });
        document.querySelector(`[data-sort="${criterion}"][data-bus="${bus}"]`).classList.add('active');

        this.updateMessageList();
        this.addActivity(`${bus.toUpperCase()} sorted by: ${criterion} (${this.sortDirection[bus]})`);
    }

    addActivity(message) {
        const timestamp = new Date().toLocaleTimeString();
        this.activityLog.unshift({ time: timestamp, message });

        if (this.activityLog.length > this.maxActivityItems) {
            this.activityLog = this.activityLog.slice(0, this.maxActivityItems);
        }

        this.updateActivityFeed();
    }

    updateActivityFeed() {
        const container = document.querySelector('#live-activity .activity-list');
        if (!container) return;

        let html = '';
        this.activityLog.slice(0, 20).forEach(item => {
            html += '<div class="activity-item">';
            html += '<span class="activity-time">' + item.time + '</span>';
            html += item.message;
            html += '</div>';
        });

        container.innerHTML = html;
    }

    sendCustomMessage() {
        if (!this.selectedMessage) {
            this.addActivity('⚠️ Please select a message first');
            return;
        }

        const inputs = document.querySelectorAll('.byte-input');
        const payload = Array.from(inputs).map(input => {
            const val = input.value || '00';
            return parseInt(val, 16);
        });
        const payloadHex = payload.map(b => b.toString(16).padStart(2, '0').toUpperCase()).join('');

        if (this.socket && this.socket.readyState === WebSocket.OPEN) {
            this.socket.send(JSON.stringify({
                command: 'send_message',
                id: this.selectedMessage,
                payload: payload,
                bus: this.selectedBus || 'can0'
            }));
            
            // Update sent count
            const sentCountEl = document.getElementById('sent-count');
            if (sentCountEl) {
                const currentCount = parseInt(sentCountEl.textContent) || 0;
                sentCountEl.textContent = currentCount + 1;
            }
            
            // Update status
            const statusEl = document.getElementById('test-status');
            if (statusEl) {
                statusEl.textContent = 'Sent';
                statusEl.style.color = '#2ecc71';
                setTimeout(() => {
                    statusEl.textContent = 'Ready';
                    statusEl.style.color = '';
                }, 1000);
            }
            
            this.addActivity('📤 Sent custom message: ' + this.selectedMessage + ' [' + payloadHex + '] on ' + (this.selectedBus || 'CAN0'));
        } else {
            this.addActivity('❌ Cannot send: WebSocket not connected');
            
            const statusEl = document.getElementById('test-status');
            if (statusEl) {
                statusEl.textContent = 'Error';
                statusEl.style.color = '#e74c3c';
                setTimeout(() => {
                    statusEl.textContent = 'Ready';
                    statusEl.style.color = '';
                }, 2000);
            }
        }
    }

    copyCurrentPayload() {
        if (!this.selectedMessage) return;

        const msg = this.canData.get(this.selectedMessage);
        if (msg && msg.data) {
            navigator.clipboard.writeText(msg.data).then(() => {
                this.addActivity('Copied payload: ' + msg.data);
            });
        }
    }

    captureSnapshot() {
        // Freeze a copy of the current canData for comparison
        this.snapshot = new Map(Array.from(this.canData.entries()).map(([k, v]) => [k, Object.assign({}, v, {
            snap_payload: v.last_payload ? [...v.last_payload] : null
        })]));
        this.addActivity('📸 Snapshot captured (' + this.snapshot.size + ' IDs) \u2014 changed bytes will highlight');
        // Request a fresh summary from the ESP32 too
        if (this.socket && this.socket.readyState === WebSocket.OPEN) {
            this.socket.send('snapshot');
        }
    }

    clearData() {
        this.canData.clear();
        this.activityLog = [];
        this.selectedMessage = null;
        
        // Clear payload update timer
        if (this.payloadUpdateTimer) {
            clearInterval(this.payloadUpdateTimer);
            this.payloadUpdateTimer = null;
        }
        
        this.updateMessageList();
        this.updateSelectedMessage();
        this.updateActivityFeed();
        this.addActivity('Data cleared');
    }

    exportData() {
        const data = {
            timestamp: new Date().toISOString(),
            messages: Array.from(this.canData.values()),
            pinnedMessages: Array.from(this.pinnedMessages),
            activityLog: this.activityLog
        };

        const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' });
        const url = URL.createObjectURL(blob);

        const a = document.createElement('a');
        a.href = url;
        a.download = 'can-analysis-' + Date.now() + '.json';
        a.click();

        URL.revokeObjectURL(url);
        this.addActivity('Data exported');
    }

    updateConnectionStatus() {
        const statusEl = document.getElementById('connection-status');
        if (statusEl) {
            statusEl.textContent = this.isConnected ? '●' : '○';
            statusEl.style.color = this.isConnected ? '#2ecc71' : '#e74c3c';
        }
    }

    updateCANDataStatus() {
        const now = Date.now();
        const timeSinceLastMessage = now - this.lastCANMessageTime;
        
        // Consider CAN data inactive if no message received in 2 seconds
        const wasActive = this.canDataActive;
        this.canDataActive = timeSinceLastMessage < 2000;
        
        // Update indicator if status changed or on initial load
        if (wasActive !== this.canDataActive || this.lastCANMessageTime === 0) {
            const statusEl = document.getElementById('can-data-status');
            if (statusEl) {
                statusEl.textContent = this.canDataActive ? '●' : '●';
                statusEl.style.color = this.canDataActive ? '#2ecc71' : '#e74c3c';
            }
        }
    }

    updateDisplay() {
        this.updateConnectionStatus();

        const now = Date.now();
        if (now - this.lastUpdateTime > 5000) {
            // Element removed from UI
            // document.getElementById('update-rate').textContent = '0 Hz';
        }
    }

    filterMessages(query) {
        const items = document.querySelectorAll('.message-item');
        const searchTerm = query.toLowerCase();

        items.forEach(item => {
            const id = item.querySelector('.message-id').textContent.toLowerCase();
            const desc = item.querySelector('.message-desc').textContent.toLowerCase();

            if (id.includes(searchTerm) || desc.includes(searchTerm)) {
                item.style.display = 'flex';
            } else {
                item.style.display = 'none';
            }
        });
    }

    loadPayloadIntoEditor(msg) {
        const inputs = document.querySelectorAll('.byte-input');
        const bytes = msg.last_payload || [0, 0, 0, 0, 0, 0, 0, 0];
        
        inputs.forEach((input, index) => {
            if (index < bytes.length) {
                input.value = bytes[index].toString(16).padStart(2, '0').toUpperCase();
            } else {
                input.value = '00';
            }
        });
        
        this.addActivity('📝 Loaded payload into editor: ' + bytes.map(b => b.toString(16).padStart(2, '0')).join(' '));
    }

    startModifying(messageId) {
        // Send modify start command to backend
        if (this.socket && this.socket.readyState === WebSocket.OPEN) {
            this.socket.send(JSON.stringify({
                command: 'start_modify',
                id: messageId
            }));
        }
        
        // Enable continuous sending of modified payload
        this.modifyInterval = setInterval(() => {
            this.sendModifiedPayload(messageId);
        }, 100); // Send every 100ms
    }

    stopModifying(messageId) {
        // Clear the modify interval
        if (this.modifyInterval) {
            clearInterval(this.modifyInterval);
            this.modifyInterval = null;
        }
        
        // Send modify stop command to backend
        if (this.socket && this.socket.readyState === WebSocket.OPEN) {
            this.socket.send(JSON.stringify({
                command: 'stop_modify',
                id: messageId
            }));
        }
    }

    sendModifiedPayload(messageId) {
        const inputs = document.querySelectorAll('.byte-input');
        const payload = Array.from(inputs).map(input => {
            const val = input.value || '00';
            return parseInt(val, 16);
        });

        if (this.socket && this.socket.readyState === WebSocket.OPEN) {
            this.socket.send(JSON.stringify({
                command: 'send_modified',
                id: messageId,
                payload: payload
            }));
        }
    }
}

let analyzer;
document.addEventListener('DOMContentLoaded', () => {
    // Restore saved theme
    const savedTheme = localStorage.getItem('theme') || 'dark';
    if (savedTheme === 'light') {
        document.body.dataset.theme = 'light';
        const btn = document.getElementById('theme-btn');
        if (btn) btn.textContent = '\uD83C\uDF19 Theme';
    }
    analyzer = new CANAnalyzer();
    // Sync serial log button state on page load
    fetch('/api/serial_logging')
        .then(r => r.json())
        .then(data => updateSerialLogBtn(data.serial_logging));
});

function updateSerialLogBtn(active) {
    const btn = document.getElementById('serial-log-btn');
    if (!btn) return;
    if (active) {
        btn.style.background = '#e67e22';
        btn.style.borderColor = '#e67e22';
        btn.textContent = '\u2B24 Serial Log';
    } else {
        btn.style.background = '#7f8c8d';
        btn.style.borderColor = '#7f8c8d';
        btn.textContent = '\u25CB Serial Log';
    }
}

function toggleSerialLogging() {
    fetch('/api/serial_logging', {method: 'POST'})
        .then(r => r.json())
        .then(data => updateSerialLogBtn(data.serial_logging));
}

function toggleTheme() {
    const isDark = document.body.dataset.theme !== 'light';
    document.body.dataset.theme = isDark ? 'light' : '';
    localStorage.setItem('theme', isDark ? 'light' : 'dark');
    const btn = document.getElementById('theme-btn');
    if (btn) btn.textContent = isDark ? '\uD83C\uDF19 Theme' : '\u2600 Theme';
}
)js";
    return content;
}

String CANWebServer::generateConfigPage() {
    /* Build checked/selected state strings from current g_cfg values. */
    auto chk = [](bool v) -> const char* { return v ? " checked" : ""; };

    /* Build preset <option> list from the k_bus_presets[] table in can_config.h */
    String presetOpts = "";
    for (uint8_t i = 0; i < k_num_presets; i++) {
        presetOpts += "<option value=\"" + String(i) + "\"";
        if (g_cfg.bus_preset == i) presetOpts += " selected";
        presetOpts += ">" + String(k_bus_presets[i].label) + "</option>";
    }

    String page = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>CAN Logger - Config</title>
<style>
:root{--bg:#1a1a1a;--text:#e0e0e0;--text-dim:#7f8c8d;--text-hdr:#ecf0f1;--card:#252525;--card-hd:#34495e;--border:#34495e;--row-bdr:#2a2a2a;--in-bg:#2c3e50;--in-txt:#ecf0f1;--btn-s:#34495e;--btn-st:#ecf0f1;--hdr-dim:#95a5a6;--seg-off:#2c3e50;--seg-off-txt:#95a5a6}
[data-theme="light"]{--bg:#f5f5f5;--text:#1a1a1a;--text-dim:#555;--text-hdr:#1a1a1a;--card:#fff;--card-hd:#dde3ea;--border:#c0c0c0;--row-bdr:#e5e5e5;--in-bg:#fff;--in-txt:#1a1a1a;--btn-s:#dde3ea;--btn-st:#1a1a1a;--hdr-dim:#666;--seg-off:#e0e0e0;--seg-off-txt:#666}
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Monaco','Menlo',monospace;font-size:13px;background:var(--bg);color:var(--text);padding:0}
.topbar{background:linear-gradient(135deg,#2c3e50,#34495e);border-bottom:2px solid #3498db;
  padding:10px 20px;display:flex;justify-content:space-between;align-items:center}
.topbar a{color:#3498db;text-decoration:none;font-weight:700}
.topbar-r{display:flex;align-items:center;gap:12px}
#theme-btn{padding:4px 12px;font-size:11px;cursor:pointer;background:rgba(255,255,255,0.1);color:#ecf0f1;border:1px solid rgba(255,255,255,0.3);border-radius:4px;font-family:inherit}
#theme-btn:hover{background:rgba(255,255,255,0.2)}
h1{color:#3498db;font-size:16px}
.wrap{max-width:640px;margin:40px auto;padding:0 20px}
.card{background:var(--card);border:1px solid var(--border);border-radius:8px;margin-bottom:24px;overflow:hidden}
.card-head{background:var(--card-hd);padding:10px 16px;font-weight:700;color:var(--text-hdr);font-size:12px;
  display:flex;justify-content:space-between;align-items:center}
.card-head span{color:var(--hdr-dim);font-weight:400;font-size:11px}
.row{display:flex;align-items:flex-start;padding:12px 16px;border-bottom:1px solid var(--row-bdr);gap:16px}
.row:last-child{border-bottom:none}
.row label{display:flex;align-items:center;gap:8px;cursor:pointer;min-width:160px;flex-shrink:0}
.row label input[type=radio]{width:15px;height:15px;cursor:pointer;accent-color:#3498db}
.row .desc{color:var(--text-dim);font-size:11px;line-height:1.5;padding-top:1px}
select,input[type=text]{background:var(--in-bg);border:1px solid var(--border);color:var(--in-txt);
  padding:6px 10px;border-radius:4px;font-family:inherit;font-size:13px;width:100%}
select:focus,input[type=text]:focus{outline:none;border-color:#3498db}
/* Segmented button pair for CAN 2.0 / CAN FD */
.seg{display:inline-flex;border-radius:5px;overflow:hidden;border:1px solid var(--border)}
.seg button{padding:5px 16px;border:none;cursor:pointer;font-family:inherit;font-size:12px;
  font-weight:600;background:var(--seg-off);color:var(--seg-off-txt);transition:background .15s}
.seg button.active{background:#3498db;color:#fff}
.seg button:first-child{border-right:1px solid var(--border)}
.btn-row{padding:16px;display:flex;gap:12px;justify-content:flex-end}
.btn{padding:8px 20px;border:none;border-radius:4px;cursor:pointer;font-family:inherit;
  font-size:13px;font-weight:700;text-decoration:none;display:inline-block}
.btn.primary{background:#3498db;color:#fff}
.btn.primary:hover{background:#2980b9}
.btn.secondary{background:var(--btn-s);color:var(--btn-st)}
.btn.secondary:hover{background:#4a6a7c;color:#ecf0f1}
.note{background:rgba(243,156,18,0.1);border:1px solid #f39c12;border-radius:6px;
  color:#f39c12;padding:10px 14px;font-size:11px;margin-bottom:20px;line-height:1.6}
</style>
</head>
<body>
<div class="topbar">
  <h1>⚙ CAN Logger Config</h1>
  <div class="topbar-r">
    <button type="button" id="theme-btn" onclick="toggleTheme()">☀ Theme</button>
    <a href="/">← Back to analyser</a>
  </div>
</div>
<div class="wrap">
  <!-- ── LIVE SETTINGS: no reboot needed ─────────────────────────── -->
  <div class="card">
    <div class="card-head">Serial output mode <span style="color:var(--ok)">Takes effect immediately — no reboot needed</span></div>
    <div class="row">
      <label><input type="radio" name="serial_mode_live" value="0")html";
    page += (g_serial_mode == 0) ? " checked" : "";
    page += R"html(> CAN Diagnostics</label>
      <div class="desc">No frame logging. Periodic bus health report every 2 s: error counters, TEC/REC,
        bus state (Error Active / Passive / Bus Off) and actionable wiring/termination guidance.
        Use this when commissioning the bus or troubleshooting connection problems.</div>
    </div>
    <div class="row">
      <label><input type="radio" name="serial_mode_live" value="1")html";
    page += (g_serial_mode == 1) ? " checked" : "";
    page += R"html(> SavvyCAN CSV &mdash; file import</label>
      <div class="desc">Frames logged as comma-separated text (SavvyCAN native format).
        Save the serial output to a <code>.csv</code> file, then import in SavvyCAN:
        File &rarr; Load Log File &rarr; GVRET Logs.</div>
    </div>
    <div class="row">
      <label><input type="radio" name="serial_mode_live" value="2")html";
    page += (g_serial_mode == 2) ? " checked" : "";
    page += R"html(> Binary GVRET &mdash; live serial connection</label>
      <div class="desc">Frames sent as binary GVRET protocol for real-time capture.
        In SavvyCAN: Add New Device Connection &rarr; GVRET Serial &rarr; select this COM port.<br>
        Tip: disable <em>Validate Communications</em> in SavvyCAN settings for a stable connection.</div>
    </div>
    <div class="btn-row" style="margin-top:6px">
      <button type="button" class="btn primary" onclick="applySerialMode()">Apply now</button>
      <span id="serial-status" style="font-size:12px;margin-left:12px"></span>
    </div>
  </div>

  <!-- ── REBOOT SETTINGS ─────────────────────────────────────────── -->
  <div class="note">
    The settings below are saved to flash and applied after reboot. The device will restart automatically when you click Save.
  </div>

  <form method="POST" action="/api/set_config">

    <div class="card">
      <div class="card-head">WiFi Access Point <span>Requires reboot to apply</span></div>
      <div class="row">
        <label style="min-width:180px">Network name (SSID)</label>
        <div style="flex:1">
          <input type="text" name="wifi_ssid" maxlength="32" value=")html";
    page += String(g_cfg.wifi_ssid);
    page += R"html(">
          <div class="desc">The WiFi network name devices will see. Max 32 characters.</div>
        </div>
      </div>
      <div class="row">
        <label style="min-width:180px">Password</label>
        <div style="flex:1">
          <div style="display:flex;gap:8px;align-items:center">
            <input type="password" id="wifi_pass_input" name="wifi_pass" maxlength="63"
              autocomplete="new-password" value=")html";
    page += String(g_cfg.wifi_pass);
    page += R"html(">
            <button type="button" class="btn secondary" style="white-space:nowrap;padding:5px 10px"
              onclick="var i=document.getElementById('wifi_pass_input');i.type=i.type==='password'?'text':'password';this.textContent=i.type==='password'?'Show':'Hide'">Show</button>
          </div>
          <div class="desc">Leave blank for an open (no password) network.
            If a password is set it must be at least 8 characters (WPA2 minimum).</div>
        </div>
      </div>
    </div>

    <div class="card">
      <div class="card-head">Bus speed &amp; frame type <span>Applies to both CAN0 and CAN1 — requires reboot</span></div>
      <div class="row">
        <label style="min-width:180px">Preset</label>
        <div style="flex:1">
          <select name="bus_preset">)html";

    page += presetOpts;

    page += R"html(</select>
          <div class="desc">Speed and frame type are set together — both buses share the same setting.
            CAN 2.0 is classic CAN up to 8 bytes (most vehicles).
            CAN FD shows arbitration / data rates (e.g. 500k / 2M).
            All nodes on the bus must use the same preset.</div>
        </div>
      </div>
    </div>

    <div class="card">
      <div class="card-head">CAN0 — Bus 0 (left connector) <span>Physical CAN controller 0</span></div>
      <div class="row">
        <label><input type="checkbox" name="can0_enable")html";
    page += chk(g_cfg.can0_enable);
    page += R"html(> Enable CAN0</label>
        <div class="desc">Activate the CAN0 controller. Disable to save CPU time if only one bus is needed.</div>
      </div>
      <div class="row">
        <label><input type="checkbox" name="can0_print")html";
    page += chk(g_cfg.can0_print);
    page += R"html(> Print frames to serial</label>
        <div class="desc">Log CAN0 frames to USB serial. Disable on busy buses to prevent serial buffer overflow.</div>
      </div>
    </div>

    <div class="card">
      <div class="card-head">CAN1 — Bus 1 (right connector) <span>Physical CAN controller 1</span></div>
      <div class="row">
        <label><input type="checkbox" name="can1_enable")html";
    page += chk(g_cfg.can1_enable);
    page += R"html(> Enable CAN1</label>
        <div class="desc">Activate the CAN1 controller. Enable alongside CAN0 to bridge or log two buses simultaneously.</div>
      </div>
      <div class="row">
        <label><input type="checkbox" name="can1_print")html";
    page += chk(g_cfg.can1_print);
    page += R"html(> Print frames to serial</label>
        <div class="desc">Log CAN1 frames to USB serial.</div>
      </div>
    </div>

    <div class="btn-row">
      <a href="/" class="btn secondary">Cancel</a>
      <button type="submit" class="btn primary">Save &amp; Reboot</button>
    </div>

  </form>
</div>
<script>
document.querySelector('form').addEventListener('submit', function(e) {
  e.preventDefault();
  if (!confirm('Save settings and reboot the device?')) return;
  if (!confirm('Are you sure? The device will restart and lose all current CAN data.')) return;
  this.submit();
});
function applySerialMode() {
  var m = document.querySelector('input[name="serial_mode_live"]:checked');
  if (!m) { return; }
  var fd = new FormData();
  fd.append('serial_mode', m.value);
  fetch('/api/set_serial_mode', { method: 'POST', body: fd })
    .then(function(r){ return r.json(); })
    .then(function(j){
      var el = document.getElementById('serial-status');
      if (el) { el.textContent = 'Applied \u2713'; el.style.color = 'var(--ok, #2ecc71)'; }
    })
    .catch(function(){
      var el = document.getElementById('serial-status');
      if (el) { el.textContent = 'Failed'; el.style.color = '#e74c3c'; }
    });
}
(function(){
  var t=localStorage.getItem('theme')||'dark';
  if(t==='light'){document.body.dataset.theme='light';var b=document.getElementById('theme-btn');if(b)b.textContent='\uD83C\uDF19 Theme';}
})();
function toggleTheme(){
  var d=document.body.dataset.theme!=='light';
  document.body.dataset.theme=d?'light':'';
  localStorage.setItem('theme',d?'light':'dark');
  var b=document.getElementById('theme-btn');
  if(b)b.textContent=d?'\uD83C\uDF19 Theme':'\u2600 Theme';
}
</script>
</body>
</html>)html";

    return page;
}

String CANWebServer::get_can_ids_summary() {
    if (!can_bridge || !can_bridge->getAnalysis()) {
        return "{\"ids\":[]}";
    }

    String summary = "{\"ids\":[";
    bool first = true;
    
    // Get all CAN IDs from the analysis data
    auto analysis = can_bridge->getAnalysis();
    auto active_ids = analysis->getActiveCAN_IDs();
    
    for (uint32_t can_id : active_ids) {
        if (!first) summary += ",";
        first = false;
        
        auto stats = analysis->getStats(can_id);
        
        summary += "{\"i\":\"0x" + String(can_id, HEX) + "\",";
        summary += "\"c\":" + String(stats.message_count) + ",";
        summary += "\"r\":" + String(stats.frequency_hz) + ",";
        summary += "\"c0\":" + String(stats.can0_count) + ",";
        summary += "\"c1\":" + String(stats.can1_count) + ",";
        summary += "\"b\":" + String(stats.last_bus_id) + ",";
        summary += "\"x\":" + String(can_bridge->is_can_id_blocked(can_id) ? "1" : "0");
        summary += "}";
    }
    
    summary += "]}";
    return summary;
}

String CANWebServer::get_selected_payload_detail(uint32_t can_id) {
    if (!can_bridge || !can_bridge->getAnalysis()) {
        return "{\"error\":\"No analysis data available\"}";
    }

    auto analysis = can_bridge->getAnalysis();
    auto stats = analysis->getStats(can_id);
    
    if (stats.message_count == 0) {
        return "{\"error\":\"CAN ID not found\"}";
    }

    String detail = "{";
    detail += "\"id\":\"0x" + String(can_id, HEX) + "\",";
    detail += "\"count\":" + String(stats.message_count) + ",";
    detail += "\"rate\":" + String(stats.frequency_hz) + ",";
    detail += "\"last_timestamp\":" + String(stats.last_timestamp) + ",";
    detail += "\"blocked\":" + String(can_bridge->is_can_id_blocked(can_id) ? "true" : "false") + ",";
    
    // Add current payload data
    detail += "\"payload\":{";
    detail += "\"length\":8,"; // CANIDStats always stores 8 bytes
    detail += "\"bytes\":[";
    
    for (int i = 0; i < 8; i++) {
        if (i > 0) detail += ",";
        detail += "\"0x" + String(stats.last_payload[i], HEX) + "\"";
    }
    detail += "],";
    
    // Add byte-by-byte analysis
    detail += "\"analysis\":[";
    for (int i = 0; i < 8; i++) {
        if (i > 0) detail += ",";
        detail += "{";
        detail += "\"byte\":" + String(i) + ",";
        detail += "\"value\":\"0x" + String(stats.last_payload[i], HEX) + "\",";
        detail += "\"decimal\":" + String(stats.last_payload[i]) + ",";
        detail += "\"binary\":\"" + String(stats.last_payload[i], BIN) + "\",";
        detail += "\"changes\":\"0x" + String(stats.payload_changes[i], HEX) + "\"";
        detail += "}";
    }
    detail += "]";
    
    detail += "}";
    detail += "}";
    
    return detail;
}
