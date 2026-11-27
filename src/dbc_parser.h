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
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/** @brief Maximum signal name length (including null terminator). */
#define DBC_NAME_LEN   32
/** @brief Maximum unit string length (including null terminator). */
#define DBC_UNIT_LEN    8
/** @brief Maximum DBC filepath length (including null terminator). */
#define DBC_PATH_LEN   48
/** @brief Maximum file upload size in bytes (512 KB). */
#define DBC_MAX_FILE_BYTES (512 * 1024UL)
/** @brief Hard cap on total signals loaded across all DBC files (~60 B each in RAM). */
#define DBC_MAX_SIGNALS   2000
/** @brief Stop loading signals when free heap falls below this, whatever the cap. */
#define DBC_MIN_FREE_HEAP (64 * 1024UL)

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

/** @brief Per-file load result. */
struct DBCFileStat {
    char     name[DBC_PATH_LEN];  ///< File name without leading '/'
    uint16_t defined;             ///< Signals the file defines
    uint16_t loaded;              ///< Signals actually stored (rest dropped at cap)
};

/** @brief Why loading stopped storing signals. */
enum DBCLimit : uint8_t { DBC_LIMIT_NONE = 0, DBC_LIMIT_CAP = 1, DBC_LIMIT_HEAP = 2 };

/** @brief Consistent snapshot of the loaded database, for status pages. */
struct DBCSummary {
    bool     busy;                ///< true = a load was in progress; other fields are empty
    uint32_t messages;
    uint32_t signals;
    uint32_t dropped;             ///< Signals defined in active files but not stored
    DBCLimit limit;
    std::vector<DBCFileStat> files;
};

/**
 * @brief Streaming counter of signal (SG_) lines, fed arbitrary byte chunks.
 * @details Counts lines of the form "<whitespace>SG_", which is what the parser accepts.
 */
struct DBCSignalCounter {
    uint32_t count = 0;
    uint8_t  state = 0;   ///< 0=line start, 1=leading ws seen, 2='S', 3='G', 4=rest of line
    void feed(const uint8_t* data, size_t len);
    /** @brief Count SG_ lines in a SPIFFS file. @return count, or -1 if it can't be opened. */
    static int32_t count_file(const char* path);
};

/** @brief Cursor for streaming the signal database as JSON across several calls. */
struct DBCJsonCursor {
    uint32_t gen        = 0;      ///< Database generation the stream started on
    uint32_t next_id    = 0;      ///< CAN ID of the message being emitted
    size_t   sig_idx    = 0;      ///< Next signal index within that message
    uint8_t  phase      = 0;      ///< 0=not started, 1=messages, 2=done
    bool     first_msg  = true;
    char     pend[320];           ///< Text generated but not yet copied out
    size_t   pend_len   = 0;
    size_t   pend_off   = 0;
};

/**
 * @brief Minimal DBC file parser.
 * @details Parses BO_ (message) and SG_ (signal) lines. Comments, attributes, and
 *          value tables are ignored.
 *
 *          Signals are stored keyed by message ID. Loading runs in the loop task while
 *          readers run in the AsyncTCP task, so all access goes through an internal mutex.
 */
class DBCParser {
public:
    DBCParser();

    /** @brief Unload, then load each file in turn, merging signals by CAN ID. */
    void load_files(const std::vector<String>& paths);

    /** @brief Unload current database and free memory. */
    void clear();

    /**
     * @brief Decode all signals for a given CAN ID into a JSON array string.
     * @param can_id   CAN message identifier.
     * @param data     Pointer to the payload bytes (up to 8).
     * @param len      Payload length in bytes.
     * @param json_out Output: JSON array of decoded signals, e.g.
     *                 [{"n":"PackVoltage","v":387.5,"u":"V"}, ...]
     *                 Empty array "[]" if no signals are defined (or a load is in progress).
     * @return Number of signals decoded (0 if none found).
     */
    int decode(uint32_t can_id, const uint8_t* data, uint8_t len, String& json_out);

    /** @brief Snapshot of counts and per-file results. Never blocks for long. */
    DBCSummary summary();

    /**
     * @brief Write the next piece of the signal database JSON into buf.
     * @details Keys are lowercase hex CAN IDs (e.g. "0x1db"). Values are arrays of
     *          signal objects: {n, sb, bl, le, si, sc, of, u, mn, mx}.
     * @return Bytes written; 0 when finished (or when the database changed mid-stream);
     *         RESPONSE_TRY_AGAIN-style value `try_again` while a load holds the lock.
     */
    size_t json_chunk(DBCJsonCursor& cur, uint8_t* buf, size_t max_len, size_t try_again);

private:
    std::map<uint32_t, std::vector<DBCSignal>> signals; ///< signals keyed by CAN ID
    std::vector<DBCFileStat> file_stats;
    uint32_t sig_total = 0;
    uint32_t dropped   = 0;
    DBCLimit limit     = DBC_LIMIT_NONE;
    volatile uint32_t generation = 0;     ///< Bumped on every clear/load
    SemaphoreHandle_t mtx;

    enum LineResult : uint8_t { LINE_OTHER, LINE_STORED, LINE_DROPPED };
    LineResult parse_line(const String& line, uint32_t& current_id);
    bool load_file(const char* path);     ///< Internal: parse file without clearing first.
    void clear_locked();
    bool can_store();                      ///< Cap / heap check; records the limit hit.
    void fill_pending(DBCJsonCursor& cur); ///< Generate the next record into cur.pend

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
