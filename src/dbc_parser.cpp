/** @file dbc_parser.cpp
    @brief Minimal DBC file parser and CAN signal decoder implementation.
*/

#include "dbc_parser.h"
#include "can_config.h"
#include <math.h>

DBCParser::DBCParser() {
    active_dbc_path[0] = '\0';
}

void DBCParser::clear() {
    signals.clear();
    active_dbc_path[0] = '\0';
}

size_t DBCParser::signal_count() const {
    size_t count = 0;
    for (const auto& pair : signals) {
        count += pair.second.size();
    }
    return count;
}

bool DBCParser::has_signals_for(uint32_t can_id) const {
    auto it = signals.find(can_id);
    return it != signals.end() && !it->second.empty();
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

void DBCParser::parse_line(const String& line, uint32_t& current_id) {
    /* BO_ <id> <name>: <dlc> <sender> */
    if (line.startsWith("BO_ ")) {
        unsigned long id = 0;
        char name[64] = "";
        // sscanf format: skip "BO_ ", then parse unsigned id and name-up-to-colon
        if (sscanf(line.c_str(), "BO_ %lu %63[^:]:", &id, name) >= 1) {
            current_id = (uint32_t)id;
            /* Touch the map so the message is visible even if it has no signals. */
            signals[current_id];
        }
        return;
    }

    /* SG_ lines must follow a BO_ context.
       Normal:      SG_ <name> : <start>|<len>@<bo><sign> (<f>,<o>) [<min>|<max>] "<unit>"
       Multiplexed: SG_ <name> m<n> : ...   (skip — not yet supported)
       Multiplexer: SG_ <name> M : ...      (skip) */
    if (!(line.startsWith(" SG_") || line.startsWith("\tSG_"))) return;
    if (current_id == UINT32_MAX) return;

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
        if (n < 8) return; /* Cannot parse — skip. */
    }

    /* Reject any signal name that looks like a mux indicator (pure digits or
       leading 'm'/'M') that slipped through. */
    if (sig_name[0] == '\0') return;

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
    if (length <= 0 || length > 64) return;
    if (start_bit < 0 || start_bit > 63) return;

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

    /* Guard: don't push if at cap (belt-and-suspenders; load_file also checks). */
    if (signal_count() < DBC_MAX_SIGNALS) {
        signals[current_id].push_back(sig);
    }
}

// ---------------------------------------------------------------------------
// File loading
// ---------------------------------------------------------------------------

bool DBCParser::load_from_spiffs(const char* path) {
    signals.clear();
    active_dbc_path[0] = '\0';
    return load_file(path);
}

bool DBCParser::load_additive_from_spiffs(const char* path) {
    /* Don't clear — merge into existing signal map. */
    return load_file(path);
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

    uint32_t current_id = UINT32_MAX;
    int line_count = 0;
    bool capped = false;
    String line;
    line.reserve(256);

    try {
        while (f.available()) {
            /* Bail early if we've hit the global signal cap. */
            if (signal_count() >= DBC_MAX_SIGNALS) {
                if (g_serial_logging && g_serial_mode == 0) {
                    Serial.printf("[DBC] Signal cap (%u) reached — stopping parse of %s\n",
                                  DBC_MAX_SIGNALS, path);
                }
                capped = true;
                break;
            }
            char c = (char)f.read();
            if (c == '\n') {
                /* Strip trailing carriage return if present (Windows line endings). */
                if (line.length() > 0 && line[line.length() - 1] == '\r') {
                    line.remove(line.length() - 1);
                }
                if (line.length() > 0) {
                    parse_line(line, current_id);
                    line_count++;
                    /* Yield every 200 lines to prevent WDT reset on large files. */
                    if (line_count % 200 == 0) yield();
                }
                line = "";
            } else {
                /* Discard lines that grow unreasonably long (corrupt/binary file). */
                if (line.length() < 512) line += c;
            }
        }
        /* Handle file without trailing newline. */
        if (!capped && line.length() > 0) {
            parse_line(line, current_id);
        }
    } catch (const std::bad_alloc&) {
        if (g_serial_logging && g_serial_mode == 0) {
            Serial.printf("[DBC] OOM while parsing %s — signals loaded so far: %u (heap: %u B)\n",
                          path, (unsigned)signal_count(), (unsigned)ESP.getFreeHeap());
        }
        f.close();
        return false;
    }

    f.close();

    strncpy(active_dbc_path, path, DBC_PATH_LEN - 1);
    active_dbc_path[DBC_PATH_LEN - 1] = '\0';

    if (g_serial_logging && g_serial_mode == 0) {
        Serial.printf("[DBC] Loaded %s — %u messages, %u signals\n",
                      path, (unsigned)message_count(), (unsigned)signal_count());
    }
    return true;
}

// ---------------------------------------------------------------------------
// Signal decode → JSON
// ---------------------------------------------------------------------------

int DBCParser::decode(uint32_t can_id, const uint8_t* data, uint8_t len,
                      String& json_out) const {
    json_out = "[]";

    auto it = signals.find(can_id);
    if (it == signals.end() || it->second.empty()) return 0;

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
    return (int)it->second.size();
}

// ---------------------------------------------------------------------------
// Full signal map → JSON (for /api/dbc/signals browser cache endpoint)
// ---------------------------------------------------------------------------

String DBCParser::to_json() const {
    String json;
    json.reserve(8192);
    json = "{";
    bool first_msg = true;

    for (const auto& kv : signals) {
        const std::vector<DBCSignal>& sigs = kv.second;
        if (sigs.empty()) continue; /* Skip messages with no parsed signals. */

        if (!first_msg) json += ",";
        first_msg = false;

        char id_str[16];
        snprintf(id_str, sizeof(id_str), "\"0x%lx\":", (unsigned long)kv.first);
        json += id_str;
        json += "[";

        bool first_sig = true;
        for (const auto& sig : sigs) {
            if (!first_sig) json += ",";
            first_sig = false;

            /* Escape backslash and double-quote characters in unit string. */
            char esc_unit[DBC_UNIT_LEN * 2 + 1];
            size_t j = 0;
            for (size_t i = 0; sig.unit[i] && j < sizeof(esc_unit) - 2; i++) {
                if (sig.unit[i] == '"' || sig.unit[i] == '\\') esc_unit[j++] = '\\';
                esc_unit[j++] = sig.unit[i];
            }
            esc_unit[j] = '\0';

            char buf[256];
            snprintf(buf, sizeof(buf),
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
            json += buf;
        }
        json += "]";
    }
    json += "}";
    return json;
}
