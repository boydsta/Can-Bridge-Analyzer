/** @file marker_plan.h
    @brief Test-plan marker sequencer.

    Stores an ordered list of step labels (loaded from SPIFFS).
    Calling mark() injects a synthetic CAN frame (ID 0x7FE) into the active
    serial log stream so timing events are embedded directly in SavvyCAN /
    GVRET recordings — no external correlation file needed.

    Plan file: SPIFFS /markers/plan.txt — one label per line.
    NVS namespace "marker": keys "step" (uint8) and "run" (uint8).
*/

#pragma once
#include <Arduino.h>
#include <vector>

#define MARKER_CAN_ID    0x7FEU        /* Reserved synthetic frame ID */
#define MARKER_MAGIC     0xBE          /* Byte 7 — helps identify in SavvyCAN */
#define MARKER_PLAN_FILE "/markers/plan.txt"
#define MARKER_NVS_NS    "marker"

class MarkerPlan {
public:
    MarkerPlan();

    /** @brief Load plan from SPIFFS and restore NVS state. Call after SPIFFS.begin(). */
    void begin();

    /** @brief Emit a marker frame for the current step and advance to the next. */
    void mark();

    /** @brief Reset to step 0 and increment the run index. */
    void reset();

    /** @brief Save a new plan (newline-separated labels) to SPIFFS and reload. */
    bool save_plan(const String& text);

    /** @brief Serialise current state as JSON for the browser. */
    String to_json() const;

    uint8_t current_step() const { return _step; }
    uint8_t run_index()    const { return _run; }
    uint8_t total_steps()  const { return (uint8_t)_steps.size(); }

private:
    std::vector<String> _steps;
    uint8_t _step = 0;
    uint8_t _run  = 0;

    bool load_plan();
    void save_state();
    const String& label_at(uint8_t i) const;
};

extern MarkerPlan g_marker_plan;
