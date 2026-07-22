#include "apply.hpp"
#include "speffect_lookup.hpp"
#include "buff_filters.hpp"
#include "horse_protection.hpp"
#include "dual_wield.hpp"
#include "names.hpp"
#include "utils.hpp"

#include <cstdio>

#include <param/param.hpp>

namespace iwb {

// Dual-wield mirror re-application cadence. rr->cycleOccurrenceSpEffectId
// makes the engine re-apply (and reset the countdown of) the Left SpEffect
// every rr->motionInterval seconds while Right is active, so Left only needs
// a short bridge duration that's continuously topped up, not the main hand's
// full duration -- see the mirror-wiring block below.
constexpr float kDualWieldCycleInterval  = 1.0f;   // rr->motionInterval, seconds
constexpr float kDualWieldBridgeDuration = 2.0f;   // ll->effectEndurance, seconds

// Human-readable duration for the log: "infinite" for the permanent sentinel
// (-1), otherwise "<n>s".
static std::string fmt_dur(float dur) {
    if (dur < 0.0f) return "infinite";
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%.0fs", dur);
    return buf;
}

// Record a per-effect note against the current source (deduped by id).
static void note_affected(AffectLog* a, int id, float dur) {
    if (!a || !a->enabled || !a->seen.insert(id).second) return;
    char buf[512];
    std::snprintf(buf, sizeof(buf), "+ %-52s -> %-9s (via %s %s)",
                  named_speffect(id).c_str(), fmt_dur(dur).c_str(),
                  a->category, a->source.c_str());
    a->affected.emplace_back(buf);
}
static void note_skipped(AffectLog* a, int id, const char* reason) {
    if (!a || !a->enabled || !a->seen.insert(id).second) return;
    char buf[512];
    std::snprintf(buf, sizeof(buf), "- %-52s NOT extended (%s) (via %s %s)",
                  named_speffect(id).c_str(), reason,
                  a->category, a->source.c_str());
    a->skipped.emplace_back(buf);
}

int add_buffs(const std::vector<int>& entries, float dur,
              bool followChain, bool applyBuffFilter,
              const std::unordered_set<int>& protectedSp,
              std::unordered_map<int, float>& target,
              int* skippedNonBuff,
              AffectLog* alog) {
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
            if (protectedSp.count(t)) { note_skipped(alog, t, "horse-protected"); continue; }
            if (is_system_effect(t)) { note_skipped(alog, t, "system effect"); continue; }
            auto* r = sp_row(t);
            // "Extend everything except debuffs": drop only effects that harm
            // the bearer or target enemies only. Neutral/utility and self/ally
            // buffs pass. (Trusted sources skip this entirely.)
            if (applyBuffFilter && (is_debuff(r) || is_foe_only(r))) {
                if (skippedNonBuff) ++*skippedNonBuff;
                // Log the near-misses worth a human's attention -- effects that
                // harm the BEARER (a self debuff / self-cost rider on something
                // that might otherwise look like a buff). The enemy-only hit
                // effects reached through an offensive spell's bullets are
                // obviously not your buffs, and logging all of them would bury
                // the useful lines -- so those are counted but not listed.
                if (is_debuff(r)) note_skipped(alog, t, "debuff");
                continue;
            }
            if (target.emplace(t, dur).second) { ++added; note_affected(alog, t, dur); }
        }
    }
    return added;
}

// Flush an AffectLog's collected notes to the log file, most useful first
// (affected buffs, then the near-miss "not extended" lines).
static void flush_affect_log(const AffectLog& a) {
    if (!a.enabled) return;
    flog("---- AFFECTED buffs (%zu SpEffect(s) extended; id:Name -> new duration) ----",
         a.affected.size());
    for (const std::string& s : a.affected) log_line("    " + s);
    if (!a.skipped.empty()) {
        flog("---- NOT affected (%zu buff-like SpEffect(s) the mod deliberately "
             "left alone) ----", a.skipped.size());
        for (const std::string& s : a.skipped) log_line("    " + s);
    }
    flog("---- (any spell/item/ash NOT listed above is not affected by this mod) ----");
}

void apply(const Ini& ini, const std::unordered_set<int>& extraGoods,
           const std::unordered_set<int>& horseGoods,
           const std::unordered_set<int>& ashIds,
           const std::unordered_set<int>& extraSpeffects,
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

    // Per-effect readable logging (default on): one line per affected SpEffect
    // (Paramdex name + new duration + which item/spell/ash reached it), plus the
    // buff-like effects deliberately left alone (with the reason). Lets the user
    // grep the log for a buff by name/id and confirm what the mod did to it.
    AffectLog alog;
    alog.enabled = ini.get_bool("logging", "log_affected", true);

    // speffect id -> target duration. Priority (first writer wins on overlap):
    // greases, then spell buffs, then consumables, then ashes, then extras.
    std::unordered_map<int, float> target;

    if (ini.get_bool("greases", "enabled", true)) {
        const float d = ini.get_float("greases", "duration", -1.0f);
        int added = 0;
        for (auto [id, row] : from::param::EquipParamGoods) {
            if (!is_grease(row)) continue;
            alog.category = "grease";
            alog.source   = named_goods(static_cast<int>(id));
            std::vector<int> entries = { row.refId_default, row.refId_1 };
            added += add_buffs(entries, d, /*followChain*/false,
                               /*applyBuffFilter*/false,
                               protectedSp, target, nullptr, &alog);
        }
        flog("greases: %d effect(s) (duration=%.1f)", added, d);
    }
    if (ini.get_bool("spell_buffs", "enabled", true)) {
        const float d = ini.get_float("spell_buffs", "duration", -1.0f);
        int added = 0, skippedNonBuff = 0;
        for (auto [id, row] : from::param::Magic) {
            std::vector<int> entries;
            gather_magic_entries(row, entries);
            alog.category = "spell";
            alog.source   = named_magic(static_cast<int>(id));
            // Follow the chain (self+ally buffs land via a bullet -> chain) and
            // filter: keep self/ally, non-debuff, timed -- so offensive spell
            // payloads (enemy-only hit effects reached through the same bullets)
            // are left alone.
            added += add_buffs(entries, d, /*followChain*/true,
                               /*applyBuffFilter*/true,
                               protectedSp, target, &skippedNonBuff, &alog);
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
            // Spirit summon ashes sort in group 20 but resolve to summon STATE,
            // not a player buff -- never extend them. (extra_goods can still
            // force one if a user really wants to.)
            if (static_cast<int>(row.goodsType) == kGoodsTypeSpiritSummon &&
                !extraGoods.count(static_cast<int>(id)))
                continue;
            std::vector<int> entries;
            gather_goods_entry_speffects(row, entries);
            alog.category = "consumable";
            alog.source   = named_goods(static_cast<int>(id));
            const int before = added;
            added += add_buffs(entries, d, /*followChain*/true,
                               /*applyBuffFilter*/true,
                               protectedSp, target, &skippedNonBuff, &alog);
            // For items the user EXPLICITLY allowlisted (extra_goods / the
            // built-in extras like Starlight Shards 1290): if nothing timed came
            // out, say so plainly -- an instant item (vanilla Starlight Shards ->
            // instant FP) has no duration to extend. Only for the curated set, so
            // the broad sort-group-20 scan doesn't flood the log with these.
            if (alog.enabled && added == before &&
                extraGoods.count(static_cast<int>(id)))
                flog("consumable %s: no timed buff to extend (instant/no-duration "
                     "item, or its buff was filtered) -- left unchanged",
                     named_goods(static_cast<int>(id)).c_str());
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
        alog.category = "ash-of-war";
        alog.source   = "allowlist";
        const int added = add_buffs(entries, d, /*followChain*/false,
                                    /*applyBuffFilter*/false,
                                    protectedSp, target, nullptr, &alog);
        flog("ashes_of_war: %d effect(s) (duration=%.1f) from %d allowlisted id(s)",
             added, d, static_cast<int>(ashIds.size()));
    }
    // Extra raw SpEffect ids (any category). This is the escape hatch for a buff
    // whose SpEffect id you know but which the automatic discovery doesn't reach
    // (or which sorts outside the tracked groups). NB: this takes SPEFFECT ids,
    // NOT goods ids -- that distinction is the fix for "I put a speffect id in
    // extra_goods and nothing happened" (extra_goods is a GOODS-id allowlist).
    // Uses the consumables duration. Trusted (id-curated): only a finite timer
    // is required; system/protected effects are still excluded.
    if (!extraSpeffects.empty()) {
        const float d = ini.get_float("consumables", "duration", 300.0f);
        std::vector<int> entries(extraSpeffects.begin(), extraSpeffects.end());
        alog.category = "extra";
        alog.source   = "extra_speffect_ids";
        const int added = add_buffs(entries, d, /*followChain*/true,
                                    /*applyBuffFilter*/false,
                                    protectedSp, target, nullptr, &alog);
        flog("extra_speffect_ids: %d effect(s) (duration=%.1f) from %d id(s)",
             added, d, static_cast<int>(extraSpeffects.size()));
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

    // Per-effect readable summary (named). Printed after the counts above so the
    // detail sits next to the totals it explains.
    flush_affect_log(alog);

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
