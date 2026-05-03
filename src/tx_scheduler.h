/** @file tx_scheduler.h
    @brief Generic periodic CAN transmit scheduler.
    @details Maintains a table of (ID, payload, interval_ms, counter_byte) entries.
             A FreeRTOS task fires each entry at its configured interval on CAN0
             or CAN1 (configurable per entry). Entries are persisted to NVS so
             the table survives reboots.

             Vehicle-specific defaults (e.g. Leaf CHAdeMO keepalives) are loaded
             only when the table is empty — they are just regular entries and can
             be edited or removed via the /tx web page.
*/

#ifndef TX_SCHEDULER_H
#define TX_SCHEDULER_H

#include <Arduino.h>
#include <ACAN2517FD.h>
#include <Preferences.h>
#include <vector>

/** @brief Maximum number of scheduler entries stored. */
#define TX_SCHED_MAX_ENTRIES  16
/** @brief NVS namespace for scheduler persistence. */
#define TX_SCHED_NVS_NS       "tx_sched"

/**
 * @brief One periodic transmit entry.
 */
struct TxEntry {
    uint32_t can_id;          ///< CAN identifier
    uint8_t  data[8];         ///< Payload bytes (always 8, zero-padded)
    uint16_t interval_ms;     ///< Transmit interval in milliseconds (1–60000)
    uint8_t  bus;             ///< Target bus: 0 = CAN0, 1 = CAN1
    int8_t   counter_byte;    ///< Byte index (0–7) for auto-incrementing counter, -1 = disabled
    bool     enabled;         ///< Whether this entry fires
    char     label[24];       ///< Human-readable label (e.g. "Leaf 0x50C keepalive")
};

/**
 * @brief Generic periodic CAN transmit scheduler.
 * @details Call begin() once in setup(). The scheduler runs its own FreeRTOS
 *          task so the main loop is not involved.  CAN controllers are written
 *          via tryToSend() — the same path used by the bridge.
 *
 *          Thread safety: entries are protected by a mutex. Web API calls
 *          (add/remove/toggle) acquire the mutex briefly.
 */
class TxScheduler {
public:
    TxScheduler();

    /**
     * @brief Initialise the scheduler.
     * @param can0  Pointer to CAN0 controller (may be nullptr).
     * @param can1  Pointer to CAN1 controller (may be nullptr).
     */
    void begin(ACAN2517FD* can0, ACAN2517FD* can1);

    /**
     * @brief Add or update an entry.
     * @return Index of the new/updated entry, or -1 if the table is full.
     */
    int  add(const TxEntry& entry);

    /** @brief Remove entry at index. */
    void remove(int index);

    /** @brief Enable or disable entry at index. */
    void set_enabled(int index, bool enabled);

    /** @brief Update payload bytes for an entry at runtime. */
    void set_data(int index, const uint8_t* data, uint8_t len);

    /** @brief Return a copy of all entries (thread-safe). */
    std::vector<TxEntry> get_entries() const;

    /** @brief Return entry count. */
    size_t count() const;

    /** @brief Serialise the table to a JSON array string. */
    String to_json() const;

    /** @brief Save table to NVS. */
    void save() const;

    /** @brief Load table from NVS. Loads Leaf CHAdeMO defaults if table empty. */
    void load();

private:
    ACAN2517FD* can[2];                ///< CAN controllers indexed by bus
    TxEntry     entries[TX_SCHED_MAX_ENTRIES];
    uint8_t     entry_count;
    SemaphoreHandle_t mutex;
    unsigned long last_fire[TX_SCHED_MAX_ENTRIES]; ///< Last transmit timestamp per entry

    static void task_fn(void* arg);   ///< FreeRTOS task body
    void        run();                ///< Called from task_fn

    void load_leaf_defaults();        ///< Populate Leaf CHAdeMO keepalive entries
};

/** @brief Global scheduler instance — extern so web_server and main can reach it. */
extern TxScheduler g_tx_sched;

#endif // TX_SCHEDULER_H
