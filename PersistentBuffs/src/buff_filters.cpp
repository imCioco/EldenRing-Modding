#include "buff_filters.hpp"
#include "buff_discovery.hpp"
#include "config.hpp"
#include "utils.hpp"

#include <unordered_set>

namespace pb {

from::paramdef::SP_EFFECT_PARAM_ST* sp_row(int id) {
    if (id < 0) return nullptr;
    auto [row, ok] = from::param::SpEffectParam[id];
    return ok ? &row : nullptr;
}

// Applied to the caster themselves? (self / player target). Ported verbatim from
// InfiniteWeaponBuffs -- used by buff_discovery to keep only self-buffs among
// consumables. Ash/grease/spell sources are trusted and skip this.
bool is_self_buff(const from::paramdef::SP_EFFECT_PARAM_ST* r) {
    return r && (r->effectTargetSelf || r->effectTargetPlayer);
}

// Weapon/shield grease? Ported verbatim from InfiniteWeaponBuffs.
bool is_grease(const from::paramdef::EQUIP_PARAM_GOODS_ST& row) {
    return row.isEnhance || row.isShieldEnchant ||
           static_cast<int>(row.sortGroupId) == kSortGroupGrease;
}

// Engine state / animation / environment effects that must never be persisted --
// the engine applies & clears them itself, so re-applying sticks the player in
// that state. Matched by id only (robust even if SpEffectParam field offsets
// drift across game versions). This is the same id set InfiniteWeaponBuffs uses
// (is_system_effect), plus the two evergaol ids PersistentBuffs already carried.
// The headline case (confirmed in-game via log_effects): 9621 "Disallow Hostile
// Actions" -- Roundtable Hold's no-combat block, which got snapshotted in the
// Hold and re-applied on the way out, locking you out of attacks/spells/arts.
// Ids from soulsmods/Paramdex (ER SpEffectParam names).
bool is_system_effect(int id) {
    if (id >= 100000 && id <= 100999)  return true; // [HKS] state block + grace
    if (id >= 131    && id <= 147)     return true; // jump / attack anim states
    if (id >= 170    && id <= 176)     return true; // guard anim states
    // Spirit-ash summon effects (20207000 "[Spirit Summon] Messmer Soldier Ashes"
    // etc.). These are transient summon state, NOT player buffs -- re-applying one
    // makes the game think a summon is still active, so the bell rings but nothing
    // spawns until restart. The 202xxxxx range is spirit summons; consumable item
    // buffs we DO want live in 205xxxxx, so this range is safe. Confirmed in-game.
    if (id >= 20200000 && id <= 20299999) return true; // spirit ash summon state
    // Negative status ailments (Hemorrhage/Poison/Scarlet Rot/Frostbite/Madness/
    // Sleep/Blight) -- their proc/DoT SpEffects. NEVER persist these: re-applying
    // one after respawn re-inflicts the ailment. The headline case (Nexus bug
    // report): DEATH BLIGHT instantly kills, and re-applying it on respawn caused
    // an inescapable death loop (only removable by quitting + deleting the mod).
    // The allowlist should already drop debuffs, but block them by id as a hard,
    // drift-proof safety net. Ranges/ids from soulsmods/Paramdex (ER SpEffectParam):
    //   70          "Blight Effect"
    //   500-507     status build-up / behavior states (Cycled/Presence of/Behavior)
    //   6400-6805   ailment proc effects ("<Ailment> - Type N - Special M")
    if (id == 70)                       return true; // Blight Effect
    if (id >= 500  && id <= 507)        return true; // status build-up states
    if (id >= 6400 && id <= 6810)       return true; // ailment procs (incl. death blight)
    switch (id) {
        case 45:      // [HKS] Counter Frames
        case 514:     // evargaol
        case 190:     // evargaol
        case 8001:    // [HKS] Is Stealth
        case 9540:    // Spirit Summon Active -- re-applying blocks re-summoning
        case 10665:   // [HKS] Event action not possible
        case 530007:  // [HKS] Goods stamina cost
        case 530012:  // [HKS] Goods stamina cost
        case 9621:    // Disallow Hostile Actions (Roundtable Hold no-combat)
        case 4600:    // Wet (Rain) -- environment, shouldn't follow you out
            return true;
        default:
            return false;
    }
}

// Does this SpEffect actually improve a combat/vitality stat? This is the robust
// auto-filter: engine state / evergaol / no-combat / animation effects change no
// stat, so they're rejected here WITHOUT needing their ids -- which is what stops
// new soft-lock effects from leaking through as the game patches. Copied verbatim
// from InfiniteWeaponBuffs' is_beneficial_buff (proven on this game build). An
// effect that buffs at least one stat counts, even if it also has a downside.
// Known gap: a few buffs (some AoW self-buffs, poise-only effects) don't show up
// in these fields -- those are handled by the g_always_persist / g_force_persist
// allowlists, which bypass this check.
bool is_beneficial_buff(const from::paramdef::SP_EFFECT_PARAM_ST* r) {
    if (!r) return false;
    // Attack up (rates >1, flat >0).
    if (r->physicsAttackPowerRate > 1.f || r->magicAttackPowerRate > 1.f ||
        r->fireAttackPowerRate > 1.f    || r->thunderAttackPowerRate > 1.f)
        return true;
    if (r->physicsAttackPower > 0 || r->magicAttackPower > 0 ||
        r->fireAttackPower > 0    || r->thunderAttackPower > 0)
        return true;
    if (r->physicsAttackRate > 1.f || r->magicAttackRate > 1.f ||
        r->fireAttackRate > 1.f    || r->thunderAttackRate > 1.f ||
        r->staminaAttackRate > 1.f)
        return true;
    // Defense up (rates >1, flat >0) / damage taken down (cut rates <1).
    if (r->physicsDiffenceRate > 1.f || r->magicDiffenceRate > 1.f ||
        r->fireDiffenceRate > 1.f    || r->thunderDiffenceRate > 1.f)
        return true;
    if (r->physicsDiffence > 0 || r->magicDiffence > 0 ||
        r->fireDiffence > 0    || r->thunderDiffence > 0)
        return true;
    if (r->slashDamageCutRate < 1.f  || r->blowDamageCutRate < 1.f   ||
        r->thrustDamageCutRate < 1.f || r->neutralDamageCutRate < 1.f ||
        r->magicDamageCutRate < 1.f  || r->fireDamageCutRate < 1.f   ||
        r->thunderDamageCutRate < 1.f)
        return true;
    // Vitality / regen.
    if (r->maxHpRate > 1.f || r->maxMpRate > 1.f || r->maxStaminaRate > 1.f)
        return true;
    if (r->hpRecoverRate > 0.f || r->mpRecoverChangeSpeed > 0 ||
        r->staminaRecoverChangeSpeed > 0)
        return true;
    // Status resistance up.
    if (r->registPoizonChangeRate > 1.f || r->registDiseaseChangeRate > 1.f ||
        r->registBloodChangeRate > 1.f  || r->registCurseChangeRate > 1.f)
        return true;
    // Rune acquisition up (Gold-Pickled Fowl Foot etc.).
    if (r->haveSoulRate > 1.f || r->soulRate > 0.f)
        return true;
    return false;
}

// Does this SpEffect HARM the bearer? The blacklist half of the "accept
// everything except a debuff" gate (buff_discovery). Ported verbatim from
// InfiniteWeaponBuffs' is_debuff. Where is_beneficial_buff needs a recognised
// POSITIVE stat (and so drops self+ally buffs whose timer row carries no stat of
// its own -- the Golden Vow spell, Rallying Standard, Blessing of the Erdtree),
// this only rejects rows that make the bearer WORSE: HP/FP/stamina drain or
// reduced max, weaker attack/defence, more damage taken (cut rate > 1), or
// lowered status resistance / stat corrections. Neutral and beneficial rows both
// pass. Applied to the wide sources (consumables, spell bullets, AoW behaviors);
// greases/spell-direct refs stay trusted and skip it.
bool is_debuff(const from::paramdef::SP_EFFECT_PARAM_ST* r) {
    if (!r) return false;
    // HP / FP / stamina DRAIN or damage-over-time.
    // ⚠ SIGN CONVENTION (verified against the vanilla regulation.bin): for these
    // periodic change* fields a NEGATIVE value RESTORES the resource (heal /
    // regen) and a POSITIVE value REMOVES it (damage / drain). So the DEBUFF is
    // the POSITIVE case, not the negative one. Ground truth: Blessing of the
    // Erdtree (changeHpPoint -12), Bestial Vitality (-5), Blessing's Boon (-8),
    // Starlight Shards (changeMpPoint -2), Crimsonburst Crystal Tear (-7) all
    // RESTORE and must NOT be debuffs; Destined Death's DoT damages via
    // changeHpPoint +1. The prior "< 0 == debuff" test was inverted and wrongly
    // dropped every regen buff (they were never tracked -> never persisted).
    if (r->changeHpRate > 0.f      || r->changeHpPoint > 0)      return true;
    if (r->changeMpRate > 0.f      || r->changeMpPoint > 0)      return true;
    if (r->changeStaminaRate > 0.f || r->changeStaminaPoint > 0) return true;
    // Reduced maximum HP / FP / stamina.
    if (r->maxHpRate < 1.f || r->maxMpRate < 1.f || r->maxStaminaRate < 1.f)
        return true;
    // Negative regen (recovery slowed below baseline).
    if (r->hpRecoverRate < 0.f || r->mpRecoverChangeSpeed < 0 ||
        r->staminaRecoverChangeSpeed < 0)
        return true;
    // Lowered attack power (rates <1, flats <0).
    if (r->physicsAttackPowerRate < 1.f || r->magicAttackPowerRate < 1.f ||
        r->fireAttackPowerRate < 1.f    || r->thunderAttackPowerRate < 1.f ||
        r->darkAttackPowerRate < 1.f)
        return true;
    if (r->physicsAttackPower < 0 || r->magicAttackPower < 0 ||
        r->fireAttackPower < 0    || r->thunderAttackPower < 0 ||
        r->darkAttackPower < 0)
        return true;
    // Lowered defense (rates <1, flats <0).
    if (r->physicsDiffenceRate < 1.f || r->magicDiffenceRate < 1.f ||
        r->fireDiffenceRate < 1.f    || r->thunderDiffenceRate < 1.f ||
        r->darkDiffenceRate < 1.f)
        return true;
    if (r->physicsDiffence < 0 || r->magicDiffence < 0 ||
        r->fireDiffence < 0    || r->thunderDiffence < 0 || r->darkDiffence < 0)
        return true;
    // Increased damage taken (cut rate >1 means MORE damage; 1 is neutral).
    if (r->slashDamageCutRate > 1.f  || r->blowDamageCutRate > 1.f   ||
        r->thrustDamageCutRate > 1.f || r->neutralDamageCutRate > 1.f ||
        r->magicDamageCutRate > 1.f  || r->fireDamageCutRate > 1.f   ||
        r->thunderDamageCutRate > 1.f|| r->darkDamageCutRate > 1.f)
        return true;
    // Lowered status resistance (rates <1, or negative point changes).
    if (r->registPoizonChangeRate < 1.f  || r->registDiseaseChangeRate < 1.f ||
        r->registBloodChangeRate < 1.f   || r->registCurseChangeRate < 1.f  ||
        r->registFreezeChangeRate < 1.f  || r->registSleepChangeRate < 1.f  ||
        r->registMadnessChangeRate < 1.f)
        return true;
    if (r->changePoisonResistPoint < 0  || r->changeDiseaseResistPoint < 0 ||
        r->changeBloodResistPoint < 0    || r->changeCurseResistPoint < 0  ||
        r->changeFreezeResistPoint < 0   || r->changeSleepResistPoint < 0  ||
        r->changeMadnessResistPoint < 0)
        return true;
    // Lowered stat corrections (weapon scaling stats reduced).
    if (r->changeStrengthPoint < 0 || r->changeAgilityPoint < 0 ||
        r->changeMagicPoint < 0    || r->changeFaithPoint < 0   ||
        r->changeLuckPoint < 0)
        return true;
    return false;
}

// Does this SpEffect target ONLY foes (enemy / oppose-target) with no self/
// player/friend flag? Such a row is an enemy debuff delivered by an AoE buff's
// bullet -- it must never be tracked as one of OUR persistable buffs even though
// it isn't a self-debuff. Ported verbatim from InfiniteWeaponBuffs' is_foe_only.
bool is_foe_only(const from::paramdef::SP_EFFECT_PARAM_ST* r) {
    if (!r) return false;
    const bool ally = r->effectTargetSelf   || r->effectTargetPlayer ||
                      r->effectTargetFriend || r->effectTargetSelfTarget ||
                      r->effectTargetFriendlyTarget;
    const bool foe  = r->effectTargetEnemy  || r->effectTargetOpposeTarget;
    return foe && !ally;
}

bool is_persistable(int id) {
    if (g_never_persist.count(id))                              return false;
    if (g_force_persist.count(id) || g_always_persist.count(id)) return true;
    // [persistence] persist_physick_tears = 0: drop tear-sourced effects as a
    // group. Sits AFTER force_persist so a single tear id can still be rescued.
    if (!g_persist_physick && g_physick_effects.count(id))      return false;
    if (is_system_effect(id))                                   return false;
    // Source-category allowlist: keep only ids discovered at startup as a
    // grease / spell / consumable / weapon-skill (AoW) buff (or
    // a trusted utility like the lantern light -- see buff_discovery; chains
    // and bullet indirection included). Drops talismans, weapon-innate
    // passives, environment/state effects.
    return g_tracked_speffects.count(id) != 0;
}

void log_effect_changes(const std::vector<int>& cur_vec) {
    static std::unordered_set<int> prev;
    std::unordered_set<int> cur(cur_vec.begin(), cur_vec.end());
    // One indented line per gained (+) / lost (-) effect -- much easier for a
    // human to scan than the old single-line +[..] -[..] dump. The deltas
    // fully determine the active set; the header carries the count.
    std::string delta;
    for (int id : cur)  if (!prev.count(id)) { delta += "\n  + "; delta += named(id); }
    for (int id : prev) if (!cur.count(id))  { delta += "\n  - "; delta += named(id); }
    if (!delta.empty())
        flog("effects changed (%zu active):%s", cur.size(), delta.c_str());
    prev = std::move(cur);
}

void log_filter_changes(const std::vector<int>& cur_vec) {
    // Only drops the USER configured are worth a line: never_persist_ids,
    // persist_physick_tears = 0. Untracked effects
    // (talismans, armor passives, environment/state) and hard-blocked system
    // ids are dropped SILENTLY -- they used to reprint a 20+ line block on
    // every active-set change and drowned the log. To find the id of a buff
    // the mod isn't keeping, use the `effects changed: +/-` lines (the id is
    // there when the buff is applied) and add it to force_persist_ids.
    static std::unordered_set<int> prev_dropped;
    std::unordered_set<int> dropped;
    std::string lines;
    for (int id : cur_vec) {
        if (is_persistable(id)) continue;
        const char* why =
            g_never_persist.count(id)                              ? "user-blocked"
          : (!g_persist_physick && g_physick_effects.count(id))    ? "physick-off"
                                                                   : nullptr;
        if (!why) continue; // not-tracked / system: silent
        dropped.insert(id);
        lines += "\n  "; lines += named(id);
        lines += " ("; lines += why; lines += ")";
    }
    if (dropped != prev_dropped && !dropped.empty())
        flog("filter: NOT persisting %zu effect(s):%s", dropped.size(), lines.c_str());
    prev_dropped = std::move(dropped);

    // Opt-in ([general] log_untracked): surface buffs the mod is silently NOT
    // keeping so a user can diagnose "my buff isn't persisting / isn't in the
    // state file". Logged ONCE per id for the whole session (not per active-set
    // change -- that spam is why the always-on version was removed), and only
    // for ids that aren't hard-blocked engine-state (system ids are never buffs,
    // so listing them is just noise). The remedy is printed inline.
    if (g_log_untracked) {
        static std::unordered_set<int> logged_untracked;
        for (int id : cur_vec) {
            if (is_persistable(id)) continue;
            if (is_system_effect(id)) continue;              // never a buff
            if (g_never_persist.count(id)) continue;         // already reported above
            if (!g_persist_physick && g_physick_effects.count(id)) continue; // ditto
            if (!logged_untracked.insert(id).second) continue; // once per session
            flog("untracked: %s is active but NOT persisted (not a tracked "
                 "grease/spell/consumable/AoW buff). To keep it through death/"
                 "fast travel, add %d to force_persist_ids.", named(id).c_str(), id);
        }
    }
}

} // namespace pb
