// The param work: classify catalysts, enable cross-casting, mirror the
// magic/holy scaling sides, and (optionally) retarget all catalyst scaling to
// the player's highest casting stat.
#pragma once

#include <cstdint>

namespace omni {

enum class ScalingMode {
    Off,         // cross-casting only, scaling untouched
    Equipped,    // spells scale with whatever catalyst is held (mirror)
    HighestStat, // all catalysts scale both spell types off max(INT, FAI)
};

struct Config {
    bool        cast_anything = true;
    ScalingMode mode          = ScalingMode::Equipped;
    // Spell INT/FAI requirements follow max(INT, FAI): the number stays the
    // same, only the stat it is checked against changes. Independent of `mode`.
    bool        requirement_swap = false;
    bool        dump             = false;
};

// One-shot param pass after wait_for_params(). Safe to call exactly once.
void apply_all(const Config& cfg);

// Highest-stat poll body (call ~1x/s from the worker thread): reads the
// player's INT/FAI and, when the higher stat changes, flips the catalyst
// stat-correction bits (scaling_mode = highest_stat) and/or the spell
// requirements (requirement_swap). No-ops when neither feature is on.
void highest_stat_tick(const Config& cfg);

} // namespace omni
