/** @file can_bridge.h
    @brief CAN Bridge — bidirectional forwarding with per-ID analysis and filtering.
*/

#ifndef CAN_BRIDGE_H
#define CAN_BRIDGE_H

#include <Arduino.h>
#include <ACAN2517FD.h>
#include <vector>
#include <algorithm>
#include "can_analysis.h"
#include "can_config.h"

/**
 * @brief CAN Bridge for analysis and bidirectional forwarding.
 * @details If tx_can is nullptr the bridge operates in monitor-only mode.
 *          If both controllers are provided, messages are forwarded between them.
 */
class CANBridge {
public:
    /**
     * @brief Construct the bridge.
     * @param rx_can Primary CAN controller (always monitored).
     * @param tx_can Second CAN controller, nullptr for monitor-only mode.
     */
    CANBridge(ACAN2517FD* rx_can, ACAN2517FD* tx_can);

    /** @brief Initialize the bridge and analysis engine. */
    void begin();

    /**
     * @brief Process a received CAN message.
     * @param frame CAN message from the receive handler.
     * @param bus_id Source bus (0 = CAN0, 1 = CAN1).
     */
    void onMessageReceived(const CANFDMessage& frame, uint8_t bus_id);

    /**
     * @brief Check whether a message should be forwarded.
     * @param frame CAN message to evaluate.
     * @return true if the message should be forwarded.
     */
    bool shouldForward(const CANFDMessage& frame);

    /**
     * @brief Forward a message to the other bus.
     * @param frame CAN message to send.
     * @return true if sent successfully.
     */
    bool forwardMessage(const CANFDMessage& frame);

    /** @brief Update statistics — call periodically from loop(). */
    void update();

    /** @return Pointer to the analysis engine. */
    CANAnalysis* getAnalysis() { return &analysis; }

    /** @brief Print bridge statistics to Serial. */
    void printAnalysisStats();

    /** @brief Block a CAN ID from being forwarded. */
    void block_can_id(uint32_t can_id);

    /** @brief Remove a CAN ID from the block list. */
    void unblock_can_id(uint32_t can_id);

    /** @brief Return true if the given CAN ID is currently blocked. */
    bool is_can_id_blocked(uint32_t can_id) const;

    /** @brief Return a copy of the current block list. */
    std::vector<uint32_t> get_blocked_ids() const;

private:
    ACAN2517FD* can_rx;
    ACAN2517FD* can_tx;
    CANAnalysis analysis;

    unsigned long last_update;
    uint32_t message_count;
    uint32_t forwarded_count;
    uint32_t blocked_count;

    std::vector<uint32_t> blocked_ids;

    int sendUsingExistingCode(const CANFDMessage& frame);
};

#endif // CAN_BRIDGE_H
