#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "config.hpp" // Ini, HandPair

namespace iwb {

// Collects human-readable, per-effect notes while a category pass runs, so the
// log can show EXACTLY which SpEffect each grease / spell / ash / consumable
// buff maps to, its new duration, and (for near-misses) why it was NOT extended.
// One line per SpEffect id (deduped -- an effect reachable from many sources is
// reported once, against the first source that reached it).
struct AffectLog {
    bool enabled = false;
    // Set by the caller before each add_buffs() call to describe the source.
    const char* category = "";  // "grease" / "spell" / "consumable" / ...
    std::string source;         // named item/spell, e.g. "2170:Fire Grease"
    // Formatted result lines (with Paramdex names), gathered across all sources.
    std::vector<std::string> affected; // "+ <id:Name>  ->  <dur>   (via ...)"
    std::vector<std::string> skipped;  // "- <id:Name>  NOT extended (<reason>) ..."
    std::unordered_set<int>  seen;     // ids already reported (either list)
};

// Add the timed buffs reachable from `entries` to `target` at `dur`.
//   followChain      : follow the SpEffect replace/cycle chain (consumables /
//                      spells) vs. only the entry effect itself (greases).
//   applyBuffFilter  : "extend everything except debuffs" gate for discovery-
//                      based sources (consumables, spell buffs) -- drops effects
//                      that harm the bearer (`is_debuff`) or target enemies only
//                      (`is_foe_only`), but keeps self/ally and neutral buffs.
//                      Off for trusted, id/source-curated sources (greases,
//                      ashes of war), which are only required to be on a timer.
//                      `skippedNonBuff` (optional) counts effects dropped here.
// System effects (`is_system_effect`) and protected effects are never added,
// regardless of the filter. First writer wins (target.emplace). `alog`
// (optional) records the readable per-effect notes described above.
int add_buffs(const std::vector<int>& entries, float dur,
              bool followChain, bool applyBuffFilter,
              const std::unordered_set<int>& protectedSp,
              std::unordered_map<int, float>& target,
              int* skippedNonBuff = nullptr,
              AffectLog* alog = nullptr);

// ---- the param passes ---------------------------------------
void apply(const Ini& ini, const std::unordered_set<int>& extraGoods,
           const std::unordered_set<int>& horseGoods,
           const std::unordered_set<int>& ashIds,
           const std::unordered_set<int>& extraSpeffects,
           const std::vector<HandPair>& artPairs);

} // namespace iwb
