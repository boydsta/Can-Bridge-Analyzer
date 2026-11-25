/** @file marker_plan.cpp
    @brief Test-plan marker sequencer implementation.
*/

#include "marker_plan.h"
#include <Preferences.h>
#include <SPIFFS.h>

/* print_can_mesg is defined in main.cpp — emits one CAN frame to the active
   serial log stream (mode 1 = SavvyCAN CSV, mode 2 = binary GVRET). */
extern void print_can_mesg(unsigned short bus, long unsigned int id,
                            unsigned short len, uint8_t *buf);

extern volatile bool    g_serial_logging;
extern volatile uint8_t g_serial_mode;

/* ---------------------------------------------------------------------------
   Global instance
--------------------------------------------------------------------------- */
MarkerPlan g_marker_plan;

/* ---------------------------------------------------------------------------
   Lifecycle
--------------------------------------------------------------------------- */

MarkerPlan::MarkerPlan() {}

void MarkerPlan::begin() {
    Preferences prefs;
    prefs.begin(MARKER_NVS_NS, /* readOnly= */ true);
    _step = prefs.getUChar("step", 0);
    _run  = prefs.getUChar("run",  0);
    prefs.end();
    load_plan();
    /* Guard: if plan shrank since last boot, clamp step. */
    if (!_steps.empty() && _step >= (uint8_t)_steps.size()) {
        _step = 0;
        save_state();
    }
}

/* ---------------------------------------------------------------------------
   Mark
--------------------------------------------------------------------------- */

void MarkerPlan::mark() {
    uint8_t total = (uint8_t)_steps.size();
    if (total == 0) return;

    uint32_t now = millis();
    uint8_t buf[8];
    buf[0] = _step;
    buf[1] = _run;
    buf[2] = (uint8_t)( now        & 0xFF);
    buf[3] = (uint8_t)((now >>  8) & 0xFF);
    buf[4] = (uint8_t)((now >> 16) & 0xFF);
    buf[5] = (uint8_t)((now >> 24) & 0xFF);
    buf[6] = total;
    buf[7] = MARKER_MAGIC;

    if (g_serial_mode == 1 || g_serial_mode == 2) {
        /* Embed as a real frame in the CSV / GVRET stream. */
        print_can_mesg(0, MARKER_CAN_ID, 8, buf);
    } else {
        /* Diagnostics mode — emit human-readable text. */
        Serial.printf("[MARKER] Run#%u  Step %u/%u: %s\n",
                      _run, _step + 1, total, label_at(_step).c_str());
    }

    /* Advance — stay on last step once reached. */
    if (_step < total - 1) {
        _step++;
    }
    save_state();
}

/* ---------------------------------------------------------------------------
   Reset
--------------------------------------------------------------------------- */

void MarkerPlan::reset() {
    _step = 0;
    _run  = (_run < 255) ? (uint8_t)(_run + 1) : 0;
    save_state();
}

/* ---------------------------------------------------------------------------
   Plan persistence
--------------------------------------------------------------------------- */

bool MarkerPlan::load_plan() {
    _steps.clear();
    if (!SPIFFS.exists(MARKER_PLAN_FILE)) return false;
    File f = SPIFFS.open(MARKER_PLAN_FILE, "r");
    if (!f) return false;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) _steps.push_back(line);
    }
    f.close();
    return !_steps.empty();
}

bool MarkerPlan::save_plan(const String& text) {
    File f = SPIFFS.open(MARKER_PLAN_FILE, "w");
    if (!f) return false;
    f.print(text);
    f.close();

    /* Reload from the text in-memory rather than re-reading SPIFFS. */
    _steps.clear();
    int start = 0;
    while (start <= (int)text.length()) {
        int nl = text.indexOf('\n', start);
        String line = (nl < 0) ? text.substring(start) : text.substring(start, nl);
        line.trim();
        if (line.length() > 0) _steps.push_back(line);
        if (nl < 0) break;
        start = nl + 1;
    }

    /* Clamp step if plan shrank. */
    if (!_steps.empty() && _step >= (uint8_t)_steps.size()) {
        _step = 0;
        save_state();
    }
    return true;
}

/* ---------------------------------------------------------------------------
   JSON serialisation
--------------------------------------------------------------------------- */

String MarkerPlan::to_json() const {
    String j;
    j.reserve(256);
    j += "{\"step\":";    j += String(_step);
    j += ",\"run\":";     j += String(_run);
    j += ",\"total\":";   j += String(_steps.size());
    j += ",\"steps\":[";
    for (size_t i = 0; i < _steps.size(); i++) {
        if (i > 0) j += ',';
        String lbl = _steps[i];
        lbl.replace("\\", "\\\\");
        lbl.replace("\"", "\\\"");
        j += '"';
        j += lbl;
        j += '"';
    }
    j += "]}";
    return j;
}

/* ---------------------------------------------------------------------------
   Helpers
--------------------------------------------------------------------------- */

const String& MarkerPlan::label_at(uint8_t i) const {
    static const String empty;
    if (i >= _steps.size()) return empty;
    return _steps[i];
}

void MarkerPlan::save_state() {
    Preferences prefs;
    prefs.begin(MARKER_NVS_NS, /* readOnly= */ false);
    prefs.putUChar("step", _step);
    prefs.putUChar("run",  _run);
    prefs.end();
}
