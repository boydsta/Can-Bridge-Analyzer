/** @file dbc_parser.cpp
    @brief Minimal DBC file parser and CAN signal decoder implementation.
*/

#include "dbc_parser.h"
#include "can_config.h"
#include <math.h>

DBCParser::DBCParser() {
    mtx = xSemaphoreCreateMutex();
}

// ---------------------------------------------------------------------------
// Signal line counter (used for files that aren't loaded)
// ---------------------------------------------------------------------------

void DBCSignalCounter::feed(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\n') { state = 0; continue; }
        switch (state) {
            case 0: state = (c == ' ' || c == '\t') ? 1 : 4; break;
            case 1: state = (c == ' ' || c == '\t') ? 1 : (c == 'S' ? 2 : 4); break;
            case 2: state = (c == 'G') ? 3 : 4; break;
            case 3: if (c == '_') count++; state = 4; break;
            default: break;
        }
    }
}

int32_t DBCSignalCounter::count_file(const char* path) {
    File f = SPIFFS.open(path, "r");
    if (!f) return -1;
    DBCSignalCounter c;
    uint8_t buf[512];
    size_t n;
    while ((n = f.read(buf, sizeof(buf))) > 0) {
        c.feed(buf, n);
        yield();
    }
    f.close();
    return (int32_t)c.count;
}

// ---------------------------------------------------------------------------
// Bit extraction
// ---------------------------------------------------------------------------

int64_t DBCParser::extract_raw(const uint8_t* data, uint8_t start_bit,
                                uint8_t length, bool little_endian) {
    if (length == 0 || length > 64) return 0;

    uint64_t result = 0;

    if (little_endian) {
        /* Intel byte order (@1): start_bit is the LSB position.
           Bit numbering: bit n = data[n/8] bit (n%8), where bit 0 is the
           LSB of data[0]. Signal extends toward higher bit indices. */
        for (int i = 0; i < (int)length; i++) {
            int bit      = (int)start_bit + i;
            int byte_idx = bit / 8;
            int bit_idx  = bit % 8;
            if (byte_idx < 8 && ((data[byte_idx] >> bit_idx) & 1U)) {
                result |= (1ULL << i);
            }
        }
    } else {
        /* Motorola byte order (@0): start_bit is the MSB position using the
           same bit numbering as Intel (bit n = data[n/8] bit n%8).
           Signal continues toward the LSB; when it reaches bit 0 of a byte
           it wraps to bit 7 of the next byte (+15 step). */
        int bit_pos = (int)start_bit;
        for (int i = (int)length - 1; i >= 0; i--) {
            int byte_idx = bit_pos / 8;
            int bit_idx  = bit_pos % 8;
            if (byte_idx >= 0 && byte_idx < 8 &&
                ((data[byte_idx] >> bit_idx) & 1U)) {
                result |= (1ULL << i);
            }
            if (bit_pos % 8 == 0) {
                bit_pos += 15;  /* LSB of byte → MSB of next byte */
            } else {
                bit_pos--;
            }
        }
    }

    return (int64_t)result;
}

// ---------------------------------------------------------------------------
// Line parser
// ---------------------------------------------------------------------------

bool DBCParser::can_store() {
    if (limit != DBC_LIMIT_NONE) return false;   /* sticky once hit */
    if (sig_total >= DBC_MAX_SIGNALS)            { limit = DBC_LIMIT_CAP;  return false; }
    if (ESP.getFreeHeap() < DBC_MIN_FREE_HEAP)   { limit = DBC_LIMIT_HEAP; return false; }
    return true;
}

DBCParser::LineResult DBCParser::parse_line(const String& line, uint32_t& current_id) {
    /* BO_ <id> <name>: <dlc> <sender> */
    if (line.startsWith("BO_ ")) {
        unsigned long id = 0;
        char name[64] = "";
        if (sscanf(line.c_str(), "BO_ %lu %63[^:]:", &id, name) >= 1) {
            current_id = (uint32_t)id;
            /* Touch the map so the message is visible even if it has no signals —
               but not once we've stopped storing, so dropped messages cost no RAM. */
            if (can_store()) signals[current_id];
        }
        return LINE_OTHER;
    }

    /* SG_ lines must follow a BO_ context.
       Normal:      SG_ <name> : <start>|<len>@<bo><sign> (<f>,<o>) [<min>|<max>] "<unit>"
       Multiplexed: SG_ <name> m<n> : ...   (parsed as a plain signal) */
    if (!(line.startsWith(" SG_") || line.startsWith("\tSG_"))) return LINE_OTHER;
    if (current_id == UINT32_MAX) return LINE_OTHER;

    /* Past the cap: just count it, skip the parse work. */
    if (!can_store()) return LINE_DROPPED;

    char   sig_name[DBC_NAME_LEN] = "";
    int    start_bit = 0, length = 0, byte_order = 1;
    char   sign_char = '+';
    float  factor = 1.0f, offset = 0.0f;
    char   unit[DBC_UNIT_LEN] = "";

    /* Try standard (non-multiplexed) format first.
       Width limiter must match DBC_NAME_LEN-1 (31) to prevent stack overflow. */
    int n = sscanf(line.c_str(), " SG_ %31s : %d|%d@%d%c (%f,%f)",
                   sig_name, &start_bit, &length, &byte_order, &sign_char,
                   &factor, &offset);

    if (n < 7) {
        /* Multiplexed signal: SG_ <name> <mux> : ... — need an extra field. */
        char mux[16] = "";
        n = sscanf(line.c_str(), " SG_ %31s %15s : %d|%d@%d%c (%f,%f)",
                   sig_name, mux, &start_bit, &length, &byte_order, &sign_char,
                   &factor, &offset);
        if (n < 8) return LINE_OTHER; /* Cannot parse — skip. */
    }

    if (sig_name[0] == '\0') return LINE_OTHER;

    /* Extract quoted unit string. */
    const char* q1 = strchr(line.c_str(), '"');
    if (q1) {
        const char* q2 = strchr(q1 + 1, '"');
        if (q2) {
            int unit_len = (int)(q2 - q1 - 1);
            if (unit_len > DBC_UNIT_LEN - 1) unit_len = DBC_UNIT_LEN - 1;
            strncpy(unit, q1 + 1, unit_len);
            unit[unit_len] = '\0';
        }
    }

    /* Clamp length to valid range. */
    if (length <= 0 || length > 64) return LINE_OTHER;
    if (start_bit < 0 || start_bit > 63) return LINE_OTHER;

    DBCSignal sig;
    strncpy(sig.name, sig_name, DBC_NAME_LEN - 1);
    sig.name[DBC_NAME_LEN - 1] = '\0';
    sig.start_bit     = (uint8_t)start_bit;
    sig.length        = (uint8_t)length;
    sig.little_endian = (byte_order == 1);
    sig.is_signed     = (sign_char == '-');
    sig.factor        = factor;
    sig.offset        = offset;
    strncpy(sig.unit, unit, DBC_UNIT_LEN - 1);
    sig.unit[DBC_UNIT_LEN - 1] = '\0';

    /* Extract [min|max] from DBC signal line (appears after the (factor,offset) group). */
    sig.min_val = 0.0f;
    sig.max_val = 0.0f;
    const char* paren_end = strchr(line.c_str(), ')');
    const char* bracket   = paren_end ? strchr(paren_end, '[') : nullptr;
    if (bracket) {
        float mn = 0.0f, mx = 0.0f;
        if (sscanf(bracket, "[%f|%f]", &mn, &mx) == 2) {
            sig.min_val = mn;
            sig.max_val = mx;
        }
    }

    signals[current_id].push_back(sig);
    sig_total++;
    return LINE_STORED;
}

// ---------------------------------------------------------------------------
// File loading
// ---------------------------------------------------------------------------

void DBCParser::clear_locked() {
    signals.clear();
    file_stats.clear();
    sig_total = 0;
    dropped   = 0;
    limit     = DBC_LIMIT_NONE;
    generation++;
}

void DBCParser::clear() {
    xSemaphoreTake(mtx, portMAX_DELAY);
    clear_locked();
    xSemaphoreGive(mtx);
}

void DBCParser::load_files(const std::vector<String>& paths) {
    xSemaphoreTake(mtx, portMAX_DELAY);
    clear_locked();
    for (const String& p : paths) {
        load_file(p.c_str());
    }
    generation++;
    xSemaphoreGive(mtx);
}

bool DBCParser::load_file(const char* path) {
    File f = SPIFFS.open(path, "r");
    if (!f) {
        if (g_serial_logging && g_serial_mode == 0) {
            Serial.printf("[DBC] Cannot open %s\n", path);
        }
        return false;
    }

    if (g_serial_logging && g_serial_mode == 0) {
        Serial.printf("[DBC] Loading %s (heap free: %u B)\n", path, (unsigned)ESP.getFreeHeap());
    }

    DBCFileStat st;
    const char* nm = (path[0] == '/') ? path + 1 : path;
    strncpy(st.name, nm, DBC_PATH_LEN - 1);
    st.name[DBC_PATH_LEN - 1] = '\0';
    uint32_t defined = 0, loaded = 0;

    uint32_t current_id = UINT32_MAX;
    int line_count = 0;
    bool ok = true;
    String line;
    line.reserve(256);
    uint8_t buf[512];

    auto handle = [&](void) {
        if (line.length() > 0 && line[line.length() - 1] == '\r') {
            line.remove(line.length() - 1);
        }
        if (line.length() > 0) {
            LineResult r = parse_line(line, current_id);
            if (r == LINE_STORED)  { defined++; loaded++; }
            if (r == LINE_DROPPED) { defined++; dropped++; }
            if (++line_count % 200 == 0) yield();   /* keep the WDT fed */
        }
        line = "";
    };

    try {
        size_t n;
        while ((n = f.read(buf, sizeof(buf))) > 0) {
            for (size_t i = 0; i < n; i++) {
                char c = (char)buf[i];
                if (c == '\n') handle();
                else if (line.length() < 512) line += c;  /* drop runaway lines */
            }
        }
        handle();  /* file without trailing newline */
    } catch (const std::bad_alloc&) {
        limit = DBC_LIMIT_HEAP;
        ok = false;
        if (g_serial_logging && g_serial_mode == 0) {
            Serial.printf("[DBC] OOM while parsing %s — signals loaded so far: %u (heap: %u B)\n",
                          path, (unsigned)sig_total, (unsigned)ESP.getFreeHeap());
        }
    }
    f.close();

    st.defined = (uint16_t)(defined > 0xFFFF ? 0xFFFF : defined);
    st.loaded  = (uint16_t)(loaded  > 0xFFFF ? 0xFFFF : loaded);
    file_stats.push_back(st);

    if (g_serial_logging && g_serial_mode == 0) {
        Serial.printf("[DBC] Loaded %s — %u of %u signals (total %u, heap free %u B)\n",
                      path, (unsigned)loaded, (unsigned)defined,
                      (unsigned)sig_total, (unsigned)ESP.getFreeHeap());
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Status snapshot
// ---------------------------------------------------------------------------

DBCSummary DBCParser::summary() {
    DBCSummary s;
    s.busy = false; s.messages = 0; s.signals = 0; s.dropped = 0; s.limit = DBC_LIMIT_NONE;
    if (xSemaphoreTake(mtx, 0) != pdTRUE) { s.busy = true; return s; }
    s.messages = signals.size();
    s.signals  = sig_total;
    s.dropped  = dropped;
    s.limit    = limit;
    s.files    = file_stats;
    xSemaphoreGive(mtx);
    return s;
}

// ---------------------------------------------------------------------------
// Signal decode → JSON
// ---------------------------------------------------------------------------

int DBCParser::decode(uint32_t can_id, const uint8_t* data, uint8_t len,
                      String& json_out) {
    json_out = "[]";
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(20)) != pdTRUE) return 0;  /* loading */

    auto it = signals.find(can_id);
    if (it == signals.end() || it->second.empty()) { xSemaphoreGive(mtx); return 0; }

    /* Pad to 8 bytes so bit extraction never reads past the buffer. */
    uint8_t buf[8] = {0};
    uint8_t copy_len = (len < 8) ? len : 8;
    memcpy(buf, data, copy_len);

    json_out = "[";
    bool first = true;

    for (const auto& sig : it->second) {
        int64_t raw = extract_raw(buf, sig.start_bit, sig.length, sig.little_endian);

        /* Sign extend if the signal is declared signed. */
        if (sig.is_signed && sig.length < 64) {
            uint64_t sign_bit = 1ULL << (sig.length - 1);
            if ((uint64_t)raw & sign_bit) {
                raw |= (int64_t)(~((1ULL << sig.length) - 1));
            }
        }

        /* Apply DBC scaling: physical = raw * factor + offset. */
        double physical = (double)raw * (double)sig.factor + (double)sig.offset;

        if (!first) json_out += ",";
        first = false;

        json_out += "{\"n\":\"";
        json_out += sig.name;
        json_out += "\",\"v\":";

        /* Format the physical value: use fixed notation unless very large/small. */
        char vbuf[32];
        if (sig.factor != 0.0f &&
            fabs(physical) < 1e6 && (fabs(physical) >= 0.001 || physical == 0.0)) {
            snprintf(vbuf, sizeof(vbuf), "%.4g", physical);
        } else {
            snprintf(vbuf, sizeof(vbuf), "%g", physical);
        }
        json_out += vbuf;
        json_out += ",\"u\":\"";
        json_out += sig.unit;
        json_out += "\"}";
    }

    json_out += "]";
    int count = (int)it->second.size();
    xSemaphoreGive(mtx);
    return count;
}

// ---------------------------------------------------------------------------
// Full signal map → JSON, streamed (for /api/dbc/signals browser cache endpoint)
// ---------------------------------------------------------------------------

void DBCParser::fill_pending(DBCJsonCursor& cur) {
    cur.pend_len = cur.pend_off = 0;
    const size_t cap = sizeof(cur.pend);

    if (cur.phase == 0) {
        cur.pend[0] = '{'; cur.pend_len = 1; cur.phase = 1;
        return;
    }

    /* Find the message to emit; skip messages with no stored signals. */
    auto it = signals.lower_bound(cur.next_id);
    while (it != signals.end() && it->second.empty()) ++it;
    if (it == signals.end()) {
        cur.pend[0] = '}'; cur.pend_len = 1; cur.phase = 2;
        return;
    }
    if (it->first != cur.next_id) { cur.next_id = it->first; cur.sig_idx = 0; }

    const std::vector<DBCSignal>& sigs = it->second;
    int n;
    if (cur.sig_idx == 0) {
        n = snprintf(cur.pend, cap, "%s\"0x%lx\":[",
                     cur.first_msg ? "" : ",", (unsigned long)it->first);
        cur.first_msg = false;
    } else {
        n = snprintf(cur.pend, cap, ",");
    }

    const DBCSignal& sig = sigs[cur.sig_idx];

    /* Escape backslash and double-quote characters in unit string. */
    char esc_unit[DBC_UNIT_LEN * 2 + 1];
    size_t j = 0;
    for (size_t i = 0; sig.unit[i] && j < sizeof(esc_unit) - 2; i++) {
        if (sig.unit[i] == '"' || sig.unit[i] == '\\') esc_unit[j++] = '\\';
        esc_unit[j++] = sig.unit[i];
    }
    esc_unit[j] = '\0';

    n += snprintf(cur.pend + n, cap - n,
                  "{\"n\":\"%s\",\"sb\":%u,\"bl\":%u,\"le\":%s,"
                  "\"si\":%s,\"sc\":%.8g,\"of\":%.8g,\"u\":\"%s\","
                  "\"mn\":%.8g,\"mx\":%.8g}",
                  sig.name,
                  (unsigned)sig.start_bit,
                  (unsigned)sig.length,
                  sig.little_endian ? "true" : "false",
                  sig.is_signed    ? "true" : "false",
                  (double)sig.factor,
                  (double)sig.offset,
                  esc_unit,
                  (double)sig.min_val,
                  (double)sig.max_val);

    if (++cur.sig_idx >= sigs.size()) {
        n += snprintf(cur.pend + n, cap - n, "]");
        cur.next_id = it->first + 1;
        cur.sig_idx = 0;
    }
    cur.pend_len = (n < (int)cap) ? (size_t)n : cap - 1;
}

size_t DBCParser::json_chunk(DBCJsonCursor& cur, uint8_t* buf, size_t max_len, size_t try_again) {
    /* Never block the AsyncTCP task: if a load holds the lock, ask to be called again. */
    if (xSemaphoreTake(mtx, 0) != pdTRUE) return try_again;

    if (cur.phase == 0) {
        cur.gen = generation;
    } else if (cur.gen != generation) {
        /* Database was reloaded mid-stream — end it; the client refetches. */
        xSemaphoreGive(mtx);
        return 0;
    }

    size_t out = 0;
    while (out < max_len) {
        if (cur.pend_off < cur.pend_len) {
            size_t n = cur.pend_len - cur.pend_off;
            if (n > max_len - out) n = max_len - out;
            memcpy(buf + out, cur.pend + cur.pend_off, n);
            cur.pend_off += n;
            out += n;
            continue;
        }
        if (cur.phase == 2) break;
        fill_pending(cur);
    }
    xSemaphoreGive(mtx);
    return out;
}
