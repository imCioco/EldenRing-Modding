#include "apply.hpp"
#include "speffect_lookup.hpp"
#include "buff_filters.hpp"
#include "horse_protection.hpp"
#include "dual_wield.hpp"
#include "utils.hpp"

#include <param/param.hpp>

namespace iwb {

// Dual-wield mirror re-application cadence. rr->cycleOccurrenceSpEffectId
// makes the engine re-apply (and reset the countdown of) the Left SpEffect
// every rr->motionInterval seconds while Right is active, so Left only needs
// a short bridge duration that's continuously topped up, not the main hand's
// full duration -- see the mirror-wiring block below.
constexpr float kDualWieldCycleInterval  = 1.0f;   // rr->motionInterval, seconds
constexpr float kDualWieldBridgeDuration = 2.0f;   // ll->effectEndurance, seconds

int add_buffs(const std::vector<int>& entries, float dur,
              bool followChain, bool applyBuffFilter,
              const std::unordered_set<int>& protectedSp,
              std::unordered_map<int, float>& target,
              int* skippedNonBuff) {
    int added = 0;
    for (int e : entries) {
        std::vector<int> timed;
        if (followChain) {
            std::unordered_set<int> visited;
            collect_timed_chain(e, timed, visited);
        } else {
            auto* r = sp_row(e);
            if (r && r->effectEndurance > 0.0f) timed.push_back(e);
        }
        for (int t : timed) {
            if (protectedSp.count(t) || is_system_effect(t)) continue;
            auto* r = sp_row(t);
            // "Extend everything except debuffs": drop only effects that harm
            // the bearer or target enemies only. Neutral/utility and self/ally
            // buffs pass. (Trusted sources skip this entirely.)
            if (applyBuffFilter && (is_debuff(r) || is_foe_only(r))) {
                if (skippedNonBuff) ++*skippedNonBuff;
                continue;
            }
            if (target.emplace(t, dur).second) ++added;
        }
    }
    return added;
}

void apply(const Ini& ini, const std::unordered_set<int>& extraGoods,
           const std::unordered_set<int>& horseGoods,
           const std::unordered_set<int>& ashIds,
           const std::vector<HandPair>& artPairs) {
    // Pass 1: make every weapon buffable.
    if (ini.get_bool("general", "all_weapons_buffable", true)) {
        int n = 0;
        for (auto [id, row] : from::param::EquipParamWeapon) {
            row.isEnhance = true;
            ++n;
        }
        flog("all_weapons_buffable: isEnhance set on %d weapon rows", n);
    } else {
        flog("all_weapons_buffable: disabled in config");
    }

    // Fence off everything a horse-summon item can reach.
    std::unordered_set<int> protectedSp;
    build_protected_set(horseGoods, protectedSp);
    flog("protected: %zu SpEffect(s) fenced off from horse-summon items",
         protectedSp.size());

    const bool stackingBonuses = ini.get_bool("stacking", "stacking_bonuses", false);

    // speffect id -> target duration. Priority (first writer wins on overlap):
    // greases, then spell buffs, then consumables.
    std::unordered_map<int, float> target;

    if (ini.get_bool("greases", "enabled", true)) {
        const float d = ini.get_float("greases", "duration", -1.0f);
        int added = 0;
        for (auto [id, row] : from::param::EquipParamGoods) {
            if (!is_grease(row)) continue;
            std::vector<int> entries = { row.refId_default, row.refId_1 };
            added += add_buffs(entries, d, /*followChain*/false,
                               /*applyBuffFilter*/false,
                               protectedSp, target);
        }
        flog("greases: %d effect(s) (duration=%.1f)", added, d);
    }
    if (ini.get_bool("spell_buffs", "enabled", true)) {
        const float d = ini.get_float("spell_buffs", "duration", -1.0f);
        int added = 0, skippedNonBuff = 0;
        for (auto [id, row] : from::param::Magic) {
            std::vector<int> entries;
            gather_magic_entries(row, entries);
            // Follow the chain (self+ally buffs land via a bullet -> chain) and
            // filter: keep self/ally, non-debuff, timed -- so offensive spell
            // payloads (enemy-only hit effects reached through the same bullets)
            // are left alone.
            added += add_buffs(entries, d, /*followChain*/true,
                               /*applyBuffFilter*/true,
                               protectedSp, target, &skippedNonBuff);
        }
        flog("spell_buffs: %d effect(s) (duration=%.1f), %d non-buff skipped "
             "(debuffs/enemy-targeted)", added, d, skippedNonBuff);
    }
    if (ini.get_bool("consumables", "enabled", true)) {
        const float d = ini.get_float("consumables", "duration", 300.0f);
        int added = 0, skippedHorse = 0, skippedNonBuff = 0;
        for (auto [id, row] : from::param::EquipParamGoods) {
            const bool inScope =
                static_cast<int>(row.sortGroupId) == kSortGroupConsumable ||
                extraGoods.count(static_cast<int>(id));
            if (!inScope) continue;
            if (row.isSummonHorse || horseGoods.count(static_cast<int>(id))) {
                ++skippedHorse;
                continue;
            }
            std::vector<int> entries;
            gather_goods_entry_speffects(row, entries);
            added += add_buffs(entries, d, /*followChain*/true,
                               /*applyBuffFilter*/true,
                               protectedSp, target, &skippedNonBuff);
        }
        flog("consumables: %d effect(s) (duration=%.1f), %d horse-summon skipped, "
             "%d non-buff skipped (debuffs/enemy-targeted)",
             added, d, skippedHorse, skippedNonBuff);
    }
    if (ini.get_bool("ashes_of_war", "enabled", true)) {
        const float d = ini.get_float("ashes_of_war", "duration", -1.0f);
        // Curated allowlist (built-in + .ini): a trusted id/source-curated set,
        // so like greases it skips the debuff/target filter (which misreads
        // these effects -- their timer row often carries no stat field) -- only
        // the id's own finite timer is required, and system/protected effects
        // are still excluded.
        std::vector<int> entries(ashIds.begin(), ashIds.end());
        const int added = add_buffs(entries, d, /*followChain*/false,
                                    /*applyBuffFilter*/false,
                                    protectedSp, target);
        flog("ashes_of_war: %d effect(s) (duration=%.1f) from %d allowlisted id(s)",
             added, d, static_cast<int>(ashIds.size()));
    }

    // Decide the dual-wield pairs BEFORE rewriting durations: the off-hand match
    // disambiguates full vs. drawstring variants by nearest duration, which must
    // be read while the rows still hold their vanilla effectEndurance.
    const bool mirrorOn = ini.get_bool("dual_wield", "mirror_to_offhand", false);
    std::unordered_map<int, int> mirror;
    if (mirrorOn) build_dualwield_mirror(artPairs, protectedSp, mirror);

    // Apply durations. `target` already excludes protected/non-timed effects, so
    // every entry here is a buff we mean to change.
    if (!target.empty()) {
        int patched = 0;
        for (auto [id, row] : from::param::SpEffectParam) {
            const auto it = target.find(static_cast<int>(id));
            if (it == target.end()) continue;
            row.effectEndurance = it->second;
            if (stackingBonuses) row.spCategory = 0;
            ++patched;
        }
        flog("durations: patched %d SpEffect(s)", patched);
        if (stackingBonuses)
            flog("stacking: stacking_bonuses ON -- spCategory zeroed on patched buffs (no mutual exclusion)");
    } else {
        flog("durations: nothing to do (all categories disabled)");
    }

    // Dual-wield: wire the off-hand mirror. Opt-in.
    // The off-hand no longer "inherits" the main hand's duration: reapplying
    // Left every motionInterval seconds RESETS its countdown, so giving it a
    // long/matching effectEndurance made it outlive the main hand by a full
    // extra duration once cycling stopped (reported bug: off-hand buff never
    // expiring with the main hand). Instead: cycle fast and give the off-hand
    // only a short bridge duration that's continuously topped up -- it then
    // lags the main hand's expiry by at most the bridge duration, not a full
    // extra one. Applies uniformly to finite and infinite (-1) right-hand
    // durations (infinite right never stops cycling, so left stays
    // effectively infinite too -- no special case needed). Unconditionally
    // overwrites ll->effectEndurance even for greases, whose Left id was
    // already set to the full grease duration by the pass above -- that
    // earlier write isn't final.
    if (mirrorOn) {
        int wired = 0;
        for (const auto& [r, l] : mirror) {
            auto* rr = sp_row(r);
            auto* ll = sp_row(l);
            if (!rr || !ll) continue;
            rr->cycleOccurrenceSpEffectId = l;
            rr->motionInterval = kDualWieldCycleInterval;
            ll->effectEndurance = kDualWieldBridgeDuration;
            ++wired;
        }
        flog("dual_wield: wired %d offhand mirror(s) (right->left), cycle=%.1fs bridge=%.1fs",
             wired, kDualWieldCycleInterval, kDualWieldBridgeDuration);
    }
}

} // namespace iwb
