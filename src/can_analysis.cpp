/** @file can_analysis.cpp
    @brief CAN Message Analysis Engine Implementation
*/

#include "can_analysis.h"
#include "can_config.h"

void CANAnalysis::begin() {
    // Clear any existing data
    can_stats.clear();
    snapshots.clear();
    filter_rules.clear();

    total_messages = 0;
    next_snapshot_id = 1;
    last_json_update = 0;
    cached_json = "";

    if (g_serial_logging && g_serial_mode == 0) { Serial.println("CAN Analysis Engine initialized"); }
}

void CANAnalysis::processMessage(const CANFDMessage& frame, uint8_t bus_id) {
    total_messages++;
    uint32_t current_time = millis();

    // Get or create statistics for this CAN ID
    CANIDStats& stats = can_stats[frame.id];

    // Update message count and timing
    stats.message_count++;
    stats.updateInterval(current_time);

    // Track bus direction
    stats.last_bus_id = bus_id;
    if (bus_id == 0) {
        stats.can0_count++;
    } else if (bus_id == 1) {
        stats.can1_count++;
    }

    // Update bit change tracking
    updateBitChanges(stats, frame.data, frame.len);

    // Store last payload
    uint8_t copy_len = (frame.len < 8) ? frame.len : 8;
    memcpy(stats.last_payload, frame.data, copy_len);

    // Invalidate cached JSON
    cached_json = "";
}

void CANAnalysis::updateBitChanges(CANIDStats& stats, const uint8_t* new_payload, uint8_t payload_len) {
    // Track which bits have changed since last message
    uint8_t max_len = (payload_len < 8) ? payload_len : 8;
    for (uint8_t i = 0; i < max_len; i++) {
        uint8_t changes = stats.last_payload[i] ^ new_payload[i];
        stats.payload_changes[i] |= changes; // Accumulate all changes
    }
}

uint32_t CANAnalysis::captureSnapshot(const String& description) {
    CANSnapshot snapshot;
    snapshot.timestamp = millis();
    snapshot.description = description;

    // Copy current statistics
    snapshot.stats = can_stats;

    // Create messages from last payload data
    for (const auto& pair : can_stats) {
        uint32_t can_id = pair.first;
        const CANIDStats& stats = pair.second;

        CANFDMessage msg;
        msg.id = can_id;
        msg.len = 8; // Assume 8 bytes for now
        msg.ext = false;
        memcpy(msg.data, stats.last_payload, 8);

        snapshot.messages[can_id] = msg;
    }

    // Store snapshot
    snapshots.push_back(snapshot);
    uint32_t snapshot_id = next_snapshot_id++;

    if (g_serial_logging && g_serial_mode == 0) {
      Serial.printf("Captured snapshot %u: %s (%u CAN IDs)\n",
                   snapshot_id, description.c_str(), snapshot.messages.size());
    }

    return snapshot_id;
}

std::vector<uint32_t> CANAnalysis::compareSnapshots(uint32_t snapshot1_id, uint32_t snapshot2_id) {
    std::vector<uint32_t> changed_ids;

    if (snapshot1_id == 0 || snapshot2_id == 0 ||
        snapshot1_id > snapshots.size() || snapshot2_id > snapshots.size()) {
        if (g_serial_logging && g_serial_mode == 0) { Serial.println("Invalid snapshot IDs for comparison"); }
        return changed_ids;
    }

    const CANSnapshot& snap1 = snapshots[snapshot1_id - 1];
    const CANSnapshot& snap2 = snapshots[snapshot2_id - 1];

    // Find CAN IDs that exist in both snapshots but have different data
    for (const auto& pair : snap1.messages) {
        uint32_t can_id = pair.first;
        const CANFDMessage& msg1 = pair.second;

        auto it = snap2.messages.find(can_id);
        if (it != snap2.messages.end()) {
            const CANFDMessage& msg2 = it->second;

            // Compare payload data
            uint8_t len1 = (msg1.len < 8) ? msg1.len : 8;
            uint8_t len2 = (msg2.len < 8) ? msg2.len : 8;
            uint8_t compare_len = (len1 < len2) ? len1 : len2;
            if (memcmp(msg1.data, msg2.data, compare_len) != 0) {
                changed_ids.push_back(can_id);
            }
        }
    }

    // Also check for new CAN IDs in snapshot2
    for (const auto& pair : snap2.messages) {
        uint32_t can_id = pair.first;
        if (snap1.messages.find(can_id) == snap1.messages.end()) {
            changed_ids.push_back(can_id);
        }
    }

    if (g_serial_logging && g_serial_mode == 0) { Serial.printf("Snapshot comparison: %u changed CAN IDs\n", changed_ids.size()); }
    return changed_ids;
}

CANIDStats CANAnalysis::getStats(uint32_t can_id) {
    auto it = can_stats.find(can_id);
    if (it != can_stats.end()) {
        return it->second;
    }
    return CANIDStats(); // Return empty stats if not found
}

std::vector<uint32_t> CANAnalysis::getActiveCAN_IDs() {
    std::vector<uint32_t> ids;
    for (const auto& pair : can_stats) {
        ids.push_back(pair.first);
    }
    return ids;
}

void CANAnalysis::addFilterRule(const FilterRule& rule) {
    filter_rules[rule.can_id] = rule;
    if (g_serial_logging && g_serial_mode == 0) {
      Serial.printf("Added filter rule for CAN ID: 0x%03X (block=%s, modify=%s)\n",
                   rule.can_id, rule.block ? "yes" : "no", rule.modify ? "yes" : "no");
    }
}

void CANAnalysis::removeFilterRule(uint32_t can_id) {
    auto it = filter_rules.find(can_id);
    if (it != filter_rules.end()) {
        filter_rules.erase(it);
        if (g_serial_logging && g_serial_mode == 0) { Serial.printf("Removed filter rule for CAN ID: 0x%03X\n", can_id); }
    }
}

bool CANAnalysis::shouldBlock(uint32_t can_id) {
    auto it = filter_rules.find(can_id);
    if (it != filter_rules.end() && it->second.active) {
        return it->second.block;
    }
    return false; // Default: don't block
}

bool CANAnalysis::shouldModify(CANFDMessage& frame) {
    auto it = filter_rules.find(frame.id);
    if (it != filter_rules.end() && it->second.active && it->second.modify) {
        const FilterRule& rule = it->second;

        // Apply payload modification with mask
        uint8_t max_len = (frame.len < 8) ? frame.len : 8;
        for (uint8_t i = 0; i < max_len; i++) {
            uint8_t mask = rule.payload_mask[i];
            if (mask != 0) {
                frame.data[i] = (frame.data[i] & ~mask) | (rule.new_payload[i] & mask);
            }
        }

        return true; // Message was modified
    }
    return false; // No modification
}

String CANAnalysis::getJSONData() {
    unsigned long now = millis();

    // Use cached JSON if recent (within 500ms)
    if (cached_json.length() > 0 && (now - last_json_update) < 500) {
        return cached_json;
    }

    // Generate new JSON
    String json = "{";
    json += "\"timestamp\":" + String(now) + ",";
    json += "\"total_messages\":" + String(total_messages) + ",";
    json += "\"active_ids\":" + String(can_stats.size()) + ",";
    json += "\"can_ids\":[";

    bool first = true;
    for (const auto& pair : can_stats) {
        if (!first) json += ",";
        json += generateCAN_IDJSON(pair.first, pair.second);
        first = false;
    }

    json += "]}";
    //Serial.println(json);

    cached_json = json;
    last_json_update = now;

    return json;
}

String CANAnalysis::generateCAN_IDJSON(uint32_t can_id, const CANIDStats& stats) {
    String json = "{";
    json += "\"id\":\"0x" + String(can_id, HEX) + "\",";
    json += "\"count\":" + String(stats.message_count) + ",";
    json += "\"frequency\":" + String(stats.frequency_hz) + ",";
    json += "\"periodic\":" + String(stats.is_periodic ? "true" : "false") + ",";
    json += "\"last_bus\":" + String(stats.last_bus_id) + ",";
    json += "\"can0_count\":" + String(stats.can0_count) + ",";
    json += "\"can1_count\":" + String(stats.can1_count) + ",";
    json += "\"last_payload\":[";

    for (int i = 0; i < 8; i++) {
        if (i > 0) json += ",";
        //json += String(stats.last_payload[i], HEX); // Was causing errors.
        json += String(stats.last_payload[i]); // Works OK, so does next.
        //json += String("\"" + String(stats.last_payload[i], HEX) + "\"");
    }
    json += "],";

    json += "\"changes\":[";
    for (uint8_t i = 0; i < 8; i++) {
        if (i > 0) json += ",";
        json += String(stats.payload_changes[i]);
    }
    json += "]";

    json += "}";
    return json;
}

void CANAnalysis::clearAll() {
    can_stats.clear();
    snapshots.clear();
    filter_rules.clear();
    total_messages = 0;
    next_snapshot_id = 1;
    cached_json = "";

    if (g_serial_logging && g_serial_mode == 0) { Serial.println("Analysis data cleared"); }
}
