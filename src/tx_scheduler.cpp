/** @file tx_scheduler.cpp
    @brief Generic periodic CAN transmit scheduler implementation.
*/

#include "tx_scheduler.h"
#include "can_config.h"

/* Global instance */
TxScheduler g_tx_sched;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TxScheduler::TxScheduler()
    : entry_count(0), mutex(nullptr) {
    memset(entries, 0, sizeof(entries));
    memset(last_fire, 0, sizeof(last_fire));
    for (int i = 0; i < 2; i++) can[i] = nullptr;
}

// ---------------------------------------------------------------------------
// begin() — init mutex, load NVS, start task
// ---------------------------------------------------------------------------

void TxScheduler::begin(ACAN2517FD* can0, ACAN2517FD* can1) {
    can[0] = can0;
    can[1] = can1;

    mutex = xSemaphoreCreateMutex();

    load(); /* Populates entries from NVS, or loads Leaf defaults if empty. */

    /* Start the scheduler task on Core 1 at low priority so CAN ISRs
       and the web server remain responsive. Stack 2 KB is ample. */
    xTaskCreatePinnedToCore(task_fn, "tx_sched", 2048, this,
                            1 /* priority */, nullptr, 1 /* core */);

    if (g_serial_logging && g_serial_mode == 0) {
        Serial.printf("[TxSched] Started — %u entries\n", (unsigned)entry_count);
    }
}

// ---------------------------------------------------------------------------
// FreeRTOS task
// ---------------------------------------------------------------------------

void TxScheduler::task_fn(void* arg) {
    static_cast<TxScheduler*>(arg)->run();
}

void TxScheduler::run() {
    while (true) {
        unsigned long now = millis();

        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            for (int i = 0; i < (int)entry_count; i++) {
                TxEntry& e = entries[i];
                if (!e.enabled || e.interval_ms == 0) continue;

                ACAN2517FD* ctrl = can[e.bus < 2 ? e.bus : 0];
                if (!ctrl) continue;

                if ((now - last_fire[i]) >= (unsigned long)e.interval_ms) {
                    last_fire[i] = now;

                    /* Auto-increment counter byte before sending. */
                    if (e.counter_byte >= 0 && e.counter_byte < 8) {
                        e.data[e.counter_byte]++;
                    }

                    CANFDMessage frame;
                    frame.id     = e.can_id;
                    frame.len    = 8;
                    frame.ext    = false;
                    frame.type   = CANFDMessage::CAN_DATA;
                    memcpy(frame.data, e.data, 8);

                    /* tryToSend is non-blocking — if TX FIFO is full the frame
                       is dropped for this cycle. Do not hold the mutex across
                       SPI — release first, re-acquire. */
                    xSemaphoreGive(mutex);
                    ctrl->tryToSend(frame);
                    xSemaphoreTake(mutex, portMAX_DELAY);
                }
            }
            xSemaphoreGive(mutex);
        }

        /* 1 ms resolution is sufficient for 100 ms keepalive intervals. */
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ---------------------------------------------------------------------------
// Entry management
// ---------------------------------------------------------------------------

int TxScheduler::add(const TxEntry& entry) {
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) != pdTRUE) return -1;

    /* Check for existing entry with same ID + bus — update it. */
    for (int i = 0; i < (int)entry_count; i++) {
        if (entries[i].can_id == entry.can_id && entries[i].bus == entry.bus) {
            entries[i] = entry;
            last_fire[i] = 0; /* Fire immediately on next cycle. */
            xSemaphoreGive(mutex);
            save();
            return i;
        }
    }

    if (entry_count >= TX_SCHED_MAX_ENTRIES) {
        xSemaphoreGive(mutex);
        return -1;
    }

    int idx = entry_count++;
    entries[idx] = entry;
    last_fire[idx] = 0;

    xSemaphoreGive(mutex);
    save();
    return idx;
}

void TxScheduler::remove(int index) {
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    if (index < 0 || index >= (int)entry_count) {
        xSemaphoreGive(mutex);
        return;
    }
    /* Shift remaining entries down. */
    for (int i = index; i < (int)entry_count - 1; i++) {
        entries[i]   = entries[i + 1];
        last_fire[i] = last_fire[i + 1];
    }
    entry_count--;
    xSemaphoreGive(mutex);
    save();
}

void TxScheduler::set_enabled(int index, bool en) {
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    if (index >= 0 && index < (int)entry_count) {
        entries[index].enabled = en;
        last_fire[index] = 0;
    }
    xSemaphoreGive(mutex);
    save();
}

void TxScheduler::set_data(int index, const uint8_t* data, uint8_t len) {
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    if (index >= 0 && index < (int)entry_count) {
        memset(entries[index].data, 0, 8);
        memcpy(entries[index].data, data, len < 8 ? len : 8);
        last_fire[index] = 0;
    }
    xSemaphoreGive(mutex);
}

std::vector<TxEntry> TxScheduler::get_entries() const {
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) != pdTRUE) return {};
    std::vector<TxEntry> v(entries, entries + entry_count);
    xSemaphoreGive(mutex);
    return v;
}

size_t TxScheduler::count() const {
    return entry_count;
}

// ---------------------------------------------------------------------------
// JSON serialisation
// ---------------------------------------------------------------------------

String TxScheduler::to_json() const {
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) != pdTRUE) return "[]";

    String json = "[";
    for (int i = 0; i < (int)entry_count; i++) {
        const TxEntry& e = entries[i];
        if (i > 0) json += ",";

        char buf[256];
        snprintf(buf, sizeof(buf),
                 "{\"idx\":%d,\"id\":\"0x%lX\",\"bus\":%u,\"interval\":%u,"
                 "\"counter_byte\":%d,\"enabled\":%s,\"label\":\"%s\","
                 "\"data\":[%u,%u,%u,%u,%u,%u,%u,%u]}",
                 i,
                 (unsigned long)e.can_id,
                 (unsigned)e.bus,
                 (unsigned)e.interval_ms,
                 (int)e.counter_byte,
                 e.enabled ? "true" : "false",
                 e.label,
                 e.data[0], e.data[1], e.data[2], e.data[3],
                 e.data[4], e.data[5], e.data[6], e.data[7]);
        json += buf;
    }
    json += "]";
    xSemaphoreGive(mutex);
    return json;
}

// ---------------------------------------------------------------------------
// NVS persistence
// ---------------------------------------------------------------------------

void TxScheduler::save() const {
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;

    Preferences prefs;
    prefs.begin(TX_SCHED_NVS_NS, false);
    prefs.putUChar("count", entry_count);
    for (int i = 0; i < (int)entry_count; i++) {
        const TxEntry& e = entries[i];
        char key[16];
        snprintf(key, sizeof(key), "id%d",   i); prefs.putULong(key, e.can_id);
        snprintf(key, sizeof(key), "d%d",    i); prefs.putBytes(key, e.data, 8);
        snprintf(key, sizeof(key), "iv%d",   i); prefs.putUShort(key, e.interval_ms);
        snprintf(key, sizeof(key), "bus%d",  i); prefs.putUChar(key, e.bus);
        snprintf(key, sizeof(key), "cb%d",   i); prefs.putChar(key, e.counter_byte);
        snprintf(key, sizeof(key), "en%d",   i); prefs.putBool(key, e.enabled);
        snprintf(key, sizeof(key), "lbl%d",  i); prefs.putString(key, e.label);
    }
    prefs.end();

    xSemaphoreGive(mutex);
}

void TxScheduler::load() {
    Preferences prefs;
    prefs.begin(TX_SCHED_NVS_NS, true);
    uint8_t cnt = prefs.getUChar("count", 0);
    if (cnt > TX_SCHED_MAX_ENTRIES) cnt = TX_SCHED_MAX_ENTRIES;

    entry_count = 0;
    for (int i = 0; i < (int)cnt; i++) {
        TxEntry e;
        memset(&e, 0, sizeof(e));
        char key[16];
        snprintf(key, sizeof(key), "id%d",  i); e.can_id       = (uint32_t)prefs.getULong(key, 0);
        snprintf(key, sizeof(key), "d%d",   i); prefs.getBytes(key, e.data, 8);
        snprintf(key, sizeof(key), "iv%d",  i); e.interval_ms  = prefs.getUShort(key, 100);
        snprintf(key, sizeof(key), "bus%d", i); e.bus          = prefs.getUChar(key, 0);
        snprintf(key, sizeof(key), "cb%d",  i); e.counter_byte = prefs.getChar(key, -1);
        snprintf(key, sizeof(key), "en%d",  i); e.enabled      = prefs.getBool(key, false);
        snprintf(key, sizeof(key), "lbl%d", i);
        String lbl = prefs.getString(key, "");
        strncpy(e.label, lbl.c_str(), sizeof(e.label) - 1);
        e.label[sizeof(e.label) - 1] = '\0';
        entries[entry_count++] = e;
    }
    prefs.end();

    /* First boot or cleared NVS — install Leaf CHAdeMO defaults. */
    if (entry_count == 0) {
        load_leaf_defaults();
    }
}

// ---------------------------------------------------------------------------
// Leaf CHAdeMO default entries
// ---------------------------------------------------------------------------

void TxScheduler::load_leaf_defaults() {
    /* All entries start DISABLED — user must enable via /tx page. */

    /* 0x50B — VCM Wakeup/Sleep Command (100 ms) */
    {
        TxEntry e;
        memset(&e, 0, sizeof(e));
        e.can_id       = 0x50B;
        e.interval_ms  = 100;
        e.bus          = 0;
        e.counter_byte = -1;
        e.enabled      = false;
        e.data[0] = 0x00;
        strncpy(e.label, "Leaf 0x50B wakeup", sizeof(e.label) - 1);
        entries[entry_count++] = e;
    }

    /* 0x50C — VCM Alive Counter (100 ms, byte 0 auto-increments) */
    {
        TxEntry e;
        memset(&e, 0, sizeof(e));
        e.can_id       = 0x50C;
        e.interval_ms  = 100;
        e.bus          = 0;
        e.counter_byte = 0;   /* Byte 0 is the rolling counter */
        e.enabled      = false;
        memset(e.data, 0, 8);
        strncpy(e.label, "Leaf 0x50C alive cnt", sizeof(e.label) - 1);
        entries[entry_count++] = e;
    }

    /* 0x1D4 — VCM Charge Request / CHAdeMO keepalive (100 ms) */
    {
        TxEntry e;
        memset(&e, 0, sizeof(e));
        e.can_id       = 0x1D4;
        e.interval_ms  = 100;
        e.bus          = 0;
        e.counter_byte = -1;
        e.enabled      = false;
        /* Typical VCM 0x1D4 idle keepalive payload (no charge request active) */
        e.data[0] = 0x00; e.data[1] = 0x00; e.data[2] = 0x00; e.data[3] = 0x00;
        e.data[4] = 0x00; e.data[5] = 0x00; e.data[6] = 0x00; e.data[7] = 0x00;
        strncpy(e.label, "Leaf 0x1D4 charge req", sizeof(e.label) - 1);
        entries[entry_count++] = e;
    }

    /* 0x1F2 — VCM Charging Status (100 ms) */
    {
        TxEntry e;
        memset(&e, 0, sizeof(e));
        e.can_id       = 0x1F2;
        e.interval_ms  = 100;
        e.bus          = 0;
        e.counter_byte = -1;
        e.enabled      = false;
        memset(e.data, 0, 8);
        strncpy(e.label, "Leaf 0x1F2 chg status", sizeof(e.label) - 1);
        entries[entry_count++] = e;
    }

    save();

    if (g_serial_logging && g_serial_mode == 0) {
        Serial.println("[TxSched] Loaded Leaf CHAdeMO defaults (all disabled)");
    }
}
