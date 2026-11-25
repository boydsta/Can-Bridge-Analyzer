/** @file can_bridge.cpp
    @brief CAN Bridge implementation.
*/

#include "can_bridge.h"

CANBridge::CANBridge(CANController* rx_can, CANController* tx_can)
    : can_rx(rx_can), can_tx(tx_can), last_update(0),
      message_count(0), forwarded_count(0), blocked_count(0), failed_count(0) {
}

void CANBridge::begin() {
    analysis.begin();
    last_update = millis();
    if (g_serial_logging && g_serial_mode == 0) {
      Serial.println("CAN Bridge initialized");
      Serial.println("Analysis features active, forwarding enabled");
    }
}

void CANBridge::onMessageReceived(const CANFrame& frame, uint8_t bus_id) {
    message_count++;

    analysis.processMessage(frame, bus_id);

    if (shouldForward(frame)) {
        bool forwarded = false;

        if (can_rx != nullptr && can_tx != nullptr) {
            if (bus_id == 0) {
                forwarded = can_controller_send(*can_tx, frame);
                if (!forwarded) {
                  /* Recover both controllers on TX fail — either may be in
                     Restricted Operation Mode (bus-off). Without recovery the
                     TX FIFO stays blocked until the ESP32 is reset. */
                  can_controller_recover(*can_rx);
                  can_controller_recover(*can_tx);
                }
            } else if (bus_id == 1) {
                forwarded = can_controller_send(*can_rx, frame);
                if (!forwarded) {
                  can_controller_recover(*can_rx);
                  can_controller_recover(*can_tx);
                }
            }
        }

        if (forwarded) {
            forwarded_count++;
        } else if (can_tx != nullptr) {
            failed_count++;  /* reported in the periodic bridge stats */
        }
    } else {
        blocked_count++;  /* reported in the periodic bridge stats */
    }
}

bool CANBridge::shouldForward(const CANFrame& frame) {
  if (is_can_id_blocked(frame.id)) {
    return false;
  }
  return !analysis.shouldBlock(frame.id);
}

bool CANBridge::forwardMessage(const CANFrame& frame) {
    if (can_tx == nullptr) {
        return false;
    }
    bool result = can_controller_send(*can_tx, frame);
    if (!result) {
        if (g_serial_logging && g_serial_mode == 0) { Serial.printf("Failed to forward CAN ID: 0x%03X\n", frame.id); }
    }
    return result;
}

int CANBridge::sendUsingExistingCode(const CANFrame& frame) {
    if (can_tx == nullptr) {
        return 1;
    }
    bool res = can_controller_send(*can_tx, frame);
    return res ? 0 : 2;
}

void CANBridge::update() {
    unsigned long now = millis();

    if (now - last_update >= 100) {
        static unsigned long last_stats = 0;
        if (now - last_stats >= 10000) {
            if (g_serial_logging && g_serial_mode == 0) {
              Serial.printf("Bridge Stats: %u total, %u forwarded, %u blocked, %u failed to forward\n",
                           message_count, forwarded_count, blocked_count, failed_count);
              Serial.printf("Active CAN IDs: %u, Total messages: %u\n",
                           analysis.getActiveCAN_IDCount(), analysis.getTotalMessages());
            }
            last_stats = now;
        }
        last_update = now;
    }
}

void CANBridge::printAnalysisStats() {
    if (g_serial_logging && g_serial_mode == 0) {
      Serial.printf("CAN Bridge Statistics:\n");
      Serial.printf("  Total messages: %u\n", message_count);
      Serial.printf("  Forwarded: %u\n", forwarded_count);
      Serial.printf("  Blocked: %u\n", blocked_count);
      Serial.printf("  Failed to forward: %u\n", failed_count);
      Serial.printf("Analysis Engine:\n");
      Serial.printf("  Active CAN IDs: %u\n", analysis.getActiveCAN_IDCount());
      Serial.printf("  Total messages: %u\n", analysis.getTotalMessages());
    }
}

void CANBridge::block_can_id(uint32_t can_id) {
    if (std::find(blocked_ids.begin(), blocked_ids.end(), can_id) == blocked_ids.end()) {
        blocked_ids.push_back(can_id);
        if (g_serial_logging && g_serial_mode == 0) { Serial.printf("Blocked CAN ID: 0x%03X\n", can_id); }
    } else {
        if (g_serial_logging && g_serial_mode == 0) { Serial.printf("CAN ID 0x%03X already blocked\n", can_id); }
    }
}

void CANBridge::unblock_can_id(uint32_t can_id) {
    auto it = std::find(blocked_ids.begin(), blocked_ids.end(), can_id);
    if (it != blocked_ids.end()) {
        blocked_ids.erase(it);
        if (g_serial_logging && g_serial_mode == 0) { Serial.printf("Unblocked CAN ID: 0x%03X\n", can_id); }
    } else {
        if (g_serial_logging && g_serial_mode == 0) { Serial.printf("CAN ID 0x%03X was not in block list\n", can_id); }
    }
}

bool CANBridge::is_can_id_blocked(uint32_t can_id) const {
    return std::find(blocked_ids.begin(), blocked_ids.end(), can_id) != blocked_ids.end();
}

std::vector<uint32_t> CANBridge::get_blocked_ids() const {
    return blocked_ids;
}
