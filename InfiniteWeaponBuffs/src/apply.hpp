#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "config.hpp" // Ini, HandPair

namespace iwb {

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
// regardless of the filter. First writer wins (target.emplace).
int add_buffs(const std::vector<int>& entries, float dur,
              bool followChain, bool applyBuffFilter,
              const std::unordered_set<int>& protectedSp,
              std::unordered_map<int, float>& target,
              int* skippedNonBuff = nullptr);

// ---- the param passes ---------------------------------------
void apply(const Ini& ini, const std::unordered_set<int>& extraGoods,
           const std::unordered_set<int>& horseGoods,
           const std::unordered_set<int>& ashIds,
           const std::vector<HandPair>& artPairs);

} // namespace iwb
