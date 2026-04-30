/** @file can_analysis.h
    @brief CAN Message Analysis Engine
    @details Statistical analysis and pattern detection for reverse engineering
*/

#ifndef CAN_ANALYSIS_H
#define CAN_ANALYSIS_H

#include <Arduino.h>
#include <ACAN2517FD.h>
#include <map>
#include <vector>
#include "can_config.h"

/**
 * @brief Statistics for individual CAN IDs
 */
struct CANIDStats {
    uint32_t message_count = 0;         ///< Total messages received
    uint32_t last_timestamp = 0;        ///< Last message timestamp (ms)
    uint32_t frequency_hz = 0;          ///< Calculated frequency
    uint8_t last_payload[8] = {0};      ///< Last payload data
    uint8_t payload_changes[8] = {0};   ///< Bit change tracking
    bool is_periodic = false;           ///< Periodic message detection
    uint32_t min_interval = 0xFFFFFFFF; ///< Minimum interval between messages
    uint32_t max_interval = 0;          ///< Maximum interval between messages
    uint32_t total_interval = 0;        ///< Sum of intervals for average
    uint8_t last_bus_id = 0xFF;         ///< Last seen bus ID (0=CAN0, 1=CAN1)
    uint32_t can0_count = 0;            ///< Messages seen on CAN0
    uint32_t can1_count = 0;            ///< Messages seen on CAN1
    
    void updateInterval(uint32_t current_time) {
        if (last_timestamp > 0) {
            uint32_t interval = current_time - last_timestamp;
            if (interval < min_interval) min_interval = interval;
            if (interval > max_interval) max_interval = interval;
            total_interval += interval;
            
            // Simple periodic detection: consistent timing
            if (message_count > 10) {
                uint32_t avg_interval = total_interval / (message_count - 1);
                uint32_t tolerance = avg_interval / 10; // 10% tolerance
                is_periodic = (max_interval - min_interval) < tolerance;
                frequency_hz = avg_interval > 0 ? 1000 / avg_interval : 0;
            }
        }
        last_timestamp = current_time;
    }
};

/**
 * @brief Snapshot of CAN traffic for comparison
 */
struct CANSnapshot {
    uint32_t timestamp;                                 ///< Capture timestamp
    String description;                                 ///< User description
    std::map<uint32_t, CANFDMessage> messages;         ///< Messages by ID
    std::map<uint32_t, CANIDStats> stats;              ///< Statistics by ID
};

/**
 * @brief Message filtering and modification rules
 */
struct FilterRule {
    uint32_t can_id;                    ///< CAN ID to filter
    bool block = false;                 ///< Block this message
    bool modify = false;                ///< Modify this message
    uint8_t new_payload[8] = {0};       ///< New payload data
    uint8_t payload_mask[8] = {0};      ///< Mask for payload modification
    bool active = true;                 ///< Rule is active
};

/**
 * @brief CAN Analysis Engine
 * @details Provides statistical analysis and pattern detection
 */
class CANAnalysis {
public:
    /**
     * @brief Initialize analysis engine
     */
    void begin();
    
    /**
     * @brief Process a received CAN message
     * @param frame CAN message to analyze
     * @param bus_id Bus ID (0 or 1)
     */
    void processMessage(const CANFDMessage& frame, uint8_t bus_id);
    
    /**
     * @brief Capture snapshot of current traffic
     * @param description User description of the snapshot
     * @return Snapshot ID for later reference
     */
    uint32_t captureSnapshot(const String& description);
    
    /**
     * @brief Compare two snapshots
     * @param snapshot1_id First snapshot ID
     * @param snapshot2_id Second snapshot ID
     * @return Vector of CAN IDs that changed between snapshots
     */
    std::vector<uint32_t> compareSnapshots(uint32_t snapshot1_id, uint32_t snapshot2_id);
    
    /**
     * @brief Get statistics for a specific CAN ID
     * @param can_id CAN ID to query
     * @return Statistics structure
     */
    CANIDStats getStats(uint32_t can_id);
    
    /**
     * @brief Get all active CAN IDs
     * @return Vector of active CAN IDs
     */
    std::vector<uint32_t> getActiveCAN_IDs();
    
    /**
     * @brief Add filter rule for message blocking/modification
     * @param rule Filter rule to add
     */
    void addFilterRule(const FilterRule& rule);
    
    /**
     * @brief Remove filter rule
     * @param can_id CAN ID of rule to remove
     */
    void removeFilterRule(uint32_t can_id);
    
    /**
     * @brief Check if message should be blocked
     * @param can_id CAN ID to check
     * @return true if message should be blocked
     */
    bool shouldBlock(uint32_t can_id);
    
    /**
     * @brief Check if message should be modified
     * @param frame CAN message (may be modified)
     * @return true if message was modified
     */
    bool shouldModify(CANFDMessage& frame);
    
    /**
     * @brief Get JSON data for web interface
     * @return JSON string with current statistics
     */
    String getJSONData();
    
    /**
     * @brief Clear all statistics and snapshots
     */
    void clearAll();
    
    /**
     * @brief Get total message count
     * @return Total messages processed
     */
    uint32_t getTotalMessages() const { return total_messages; }
    
    /**
     * @brief Get active CAN ID count
     * @return Number of unique CAN IDs seen
     */
    uint32_t getActiveCAN_IDCount() const { return can_stats.size(); }

private:
    std::map<uint32_t, CANIDStats> can_stats;          ///< Statistics by CAN ID
    std::vector<CANSnapshot> snapshots;                ///< Captured snapshots
    std::map<uint32_t, FilterRule> filter_rules;       ///< Filter rules by CAN ID
    
    uint32_t total_messages = 0;                       ///< Total messages processed
    uint32_t next_snapshot_id = 1;                     ///< Next snapshot ID
    unsigned long last_json_update = 0;                ///< Last JSON update time
    String cached_json;                                 ///< Cached JSON data
    
    /**
     * @brief Update bit change tracking
     * @param stats Statistics to update
     * @param new_payload New payload data
     * @param payload_len Payload length
     */
    void updateBitChanges(CANIDStats& stats, const uint8_t* new_payload, uint8_t payload_len);
    
    /**
     * @brief Generate JSON for specific CAN ID
     * @param can_id CAN ID
     * @param stats Statistics
     * @return JSON string
     */
    String generateCAN_IDJSON(uint32_t can_id, const CANIDStats& stats);
};

#endif // CAN_ANALYSIS_H