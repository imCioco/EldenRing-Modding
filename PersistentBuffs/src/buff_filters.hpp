#pragma once

#include <string>
#include <vector>

#include <param/param.hpp>

namespace pb {

// EquipParamGoods.sortGroupId categories treated as buff sources by
// buff_discovery (see the full category table there). 20 = buff/heal
// consumables, 70 = greases.
constexpr int kSortGroupGrease     = 70;
constexpr int kSortGroupConsumable = 20;
// Guard against pathological / cyclic SpEffect replace/cycle chains during
// consumable discovery.
constexpr int kChainMaxDepth       = 8;

// SpEffectParam row by id (binary search), or nullptr if it doesn't exist /
// params not loaded yet. Mirrors InfiniteWeaponBuffs' sp_row.
from::paramdef::SP_EFFECT_PARAM_ST* sp_row(int id);

// True if the effect targets the caster (self / player). Used by buff_discovery
// to keep only self-buffs when scanning consumables. Ported from IWB.
bool is_self_buff(const from::paramdef::SP_EFFECT_PARAM_ST* r);

// True if the goods row is a weapon/shield grease (isEnhance / isShieldEnchant,
// or sortGroupId == kSortGroupGrease). Used by buff_discovery. Ported from IWB.
bool is_grease(const from::paramdef::EQUIP_PARAM_GOODS_ST& row);

// Engine state / animation / environment effects that must never be persisted --
// the engine applies & clears them itself, so re-applying sticks the player in
// that state. Matched by id only (robust even if SpEffectParam field offsets
// drift across game versions). See CLAUDE.md "Buff filtering" for the full list
// of confirmed ids and why each one is here.
bool is_system_effect(int id);

// Does this SpEffect actually improve a combat/vitality stat? Positive-stat
// whitelist, copied verbatim from InfiniteWeaponBuffs. As of 2026-07-22 the
// discovery gate no longer uses this (it switched to the is_debuff/is_foe_only
// blacklist below so self+ally buffs with no positive stat on their timer row
// are kept); retained as a public helper / for reference.
bool is_beneficial_buff(const from::paramdef::SP_EFFECT_PARAM_ST* r);

// Does this SpEffect HARM the bearer (drain/reduce HP-FP-stam, weaken atk/def,
// raise damage taken, lower status resist / stat corrections)? The blacklist
// gate for the wide discovery sources: keep everything EXCEPT a debuff, so
// self+ally buffs whose timer row carries no positive stat still track. Ported
// verbatim from InfiniteWeaponBuffs' is_debuff. See buff_discovery.
bool is_debuff(const from::paramdef::SP_EFFECT_PARAM_ST* r);

// Does this SpEffect target ONLY foes (enemy/oppose-target, no self/player/
// friend flag)? An AoE buff's enemy-debuff bullet node must not be tracked as
// one of our persistable buffs. Ported verbatim from InfiniteWeaponBuffs.
bool is_foe_only(const from::paramdef::SP_EFFECT_PARAM_ST* r);

// Decide whether an active SpEffect should be snapshotted & re-applied after a
// wipe. Precedence:
//   1. never_persist_ids -> drop  (user kill switch, beats everything)
//   2. force/always      -> keep  (trusted allowlist; ashes live in g_always_persist)
//   3. physick tears     -> drop when [persistence] persist_physick_tears = 0
//                           (group gate; sits after force so one id can be rescued)
//   4. is_system_effect  -> drop  (known engine-state ids; fast, drift-proof)
//   5. source allowlist  -> keep iff in g_tracked_speffects (a grease/spell/
//                           consumable/utility buff discovered at
//                           startup; see buff_discovery)
// Anything else (talismans, weapon-innate passives, environment/state
// effects) -> dropped.
bool is_persistable(int id);

// Diagnostic: log which effect ids appeared/disappeared since last call (raw,
// unfiltered) so system effects can be discovered from the log. No-op unless
// g_log_effects is on.
void log_effect_changes(const std::vector<int>& cur_vec);

// Diagnostic (behind g_log_effects): log active effects dropped by an EXPLICIT
// user setting (never_persist_ids / persist_physick_tears),
// de-duped on the reported set. Untracked and system drops are silent since
// v1.33 (they reprinted a 20+ line talisman block on every change); find a
// wanted-but-dropped id via the `effects changed` +/- lines instead.
void log_filter_changes(const std::vector<int>& cur_vec);

} // namespace pb
