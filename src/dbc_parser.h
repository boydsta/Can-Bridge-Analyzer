/** @file dbc_parser.h
    @brief Minimal DBC file parser and CAN signal decoder.
    @details Loads a DBC file from SPIFFS and decodes physical signal values
             from raw CAN frame bytes. Supports Intel (little-endian @1) and
             Motorola (big-endian @0) byte orders.
*/

#ifndef DBC_PARSER_H
#define DBC_PARSER_H

#include <Arduino.h>
#include <SPIFFS.h>
#include <map>
#include <vector>

/** @brief Maximum signal name length (including null terminator). */
#define DBC_NAME_LEN   32
/** @brief Maximum unit string length (including null terminator). */
#define DBC_UNIT_LEN    8
/** @brief Maximum DBC filepath length (including null terminator). */
#define DBC_PATH_LEN   48
/** @brief Maximum file upload size in bytes (512 KB). */
#define DBC_MAX_FILE_BYTES (512 * 1024UL)
/** @brief Hard cap on total signals loaded across all DBC files. */
#define DBC_MAX_SIGNALS   768

/**
 * @brief Decoded CAN signal definition parsed from a DBC file.
 */
struct DBCSignal {
    char    name[DBC_NAME_LEN];   ///< Signal name (e.g. "PackVoltage")
    uint8_t start_bit;            ///< LSB position (Intel) or MSB position (Motorola)
    uint8_t length;               ///< Signal length in bits (1–64)
    bool    little_endian;        ///< true = Intel @1, false = Motorola @0
    bool    is_signed;            ///< true = signed value (twos complement)
    float   factor;               ///< Scale factor  (physical = raw * factor + offset)
    float   offset;               ///< Offset
    char    unit[DBC_UNIT_LEN];   ///< Physical unit string (e.g. "V", "A", "rpm")
    float   min_val;              ///< Minimum physical value from DBC [min|max] field
    float   max_val;              ///< Maximum physical value from DBC [min|max] field
};

/**
 * @brief Minimal DBC file parser.
 * @details Parses BO_ (message) and SG_ (signal) lines. Multiplexed signals
 *          are ignored. Comments, attributes, and value tables are ignored.
 *
 *          Signals are stored keyed by message ID. Call decode() to extract
 *          physical values from a received CAN frame.
 */
class DBCParser {
public:
    DBCParser();

    /**
     * @brief Load and parse a DBC file from SPIFFS.
     * @param path Full SPIFFS path, e.g. "/leaf.dbc"
     * @return true on success, false if file could not be opened.
     */
    bool load_from_spiffs(const char* path);

    /**
     * @brief Load and parse an additional DBC file, merging signals with any already loaded.
     * @details Existing signals are kept. If two files define signals for the same CAN ID,
     *          signals from the new file are appended to that ID's list.
     * @param path Full SPIFFS path, e.g. "/tesla.dbc"
     * @return true on success.
     */
    bool load_additive_from_spiffs(const char* path);

    /** @brief Unload current database and free memory. */
    void clear();

    /**
     * @brief Decode all signals for a given CAN ID into a JSON array string.
     * @param can_id   CAN message identifier.
     * @param data     Pointer to the payload bytes (up to 8).
     * @param len      Payload length in bytes.
     * @param json_out Output: JSON array of decoded signals, e.g.
     *                 [{"n":"PackVoltage","v":387.5,"u":"V"}, ...]
     *                 Empty array "[]" if no signals are defined for this ID.
     * @return Number of signals decoded (0 if none found).
     */
    int decode(uint32_t can_id, const uint8_t* data, uint8_t len, String& json_out) const;

    /** @return true if the loaded database contains signals for can_id. */
    bool has_signals_for(uint32_t can_id) const;

    /** @return Currently loaded DBC file path, empty string if none. */
    const char* active_path() const { return active_dbc_path[0] ? active_dbc_path : ""; }

    /** @return Number of message definitions in the loaded database. */
    size_t message_count() const { return signals.size(); }

    /** @return Total number of signals in the loaded database. */
    size_t signal_count() const;

    /**
     * @brief Serialise the entire signal database as a JSON object.
     * @details Keys are lowercase hex CAN IDs (e.g. \"0x1db\"). Values are arrays of signal
     *          objects: {n, sb, bl, le, si, sc, of, u, mn, mx}.
     *          Intended for serving to the browser once at page load.
     * @return JSON string.
     */
    String to_json() const;

private:
    std::map<uint32_t, std::vector<DBCSignal>> signals; ///< signals keyed by CAN ID
    char active_dbc_path[DBC_PATH_LEN];                 ///< path of loaded file

    /**
     * @brief Parse a single line from the DBC file.
     * @param line       Line text (trimmed of \\r).
     * @param current_id In/out: current message ID context (UINT32_MAX = none).
     */
    void parse_line(const String& line, uint32_t& current_id);
    bool load_file(const char* path);  ///< Internal: parse file without clearing first.

    /**
     * @brief Extract raw unsigned integer value from CAN payload bits.
     * @param data         Payload bytes (8 bytes assumed).
     * @param start_bit    Start bit as per DBC convention.
     * @param length       Number of bits.
     * @param little_endian true = Intel byte order, false = Motorola byte order.
     * @return Raw extracted value (not yet scaled).
     */
    static int64_t extract_raw(const uint8_t* data, uint8_t start_bit,
                               uint8_t length, bool little_endian);
};

#endif // DBC_PARSER_H
