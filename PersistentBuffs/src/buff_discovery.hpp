#pragma once

#include <unordered_set>

namespace pb {

// ---- source-category buff allowlist (buff_discovery.cpp) -----------------
// Built once at startup (after params load): every SpEffect id reachable from
// a buff SOURCE the player can trigger --
//   * greases            (EquipParamGoods flags: isEnhance/isShieldEnchant/70)
//   * spells             (Magic refId1..10: direct SpEffects AND projectile/
//                         bullet-applied AOE buffs like Golden Vow)
//   * consumables        (ALL EquipParamGoods rows, field-gated)
//   * weapon skills/AoW  (BehaviorParam_PC/BehaviorParam refs, field-gated --
//                         covers every ash of war incl. overhaul-added ones)
// -- with replace/cycle chains and bullet indirection fully walked, so the id
// that's actually ACTIVE on the player is tracked, not just the id the source
// references first. Everything is derived from the loaded params, so overhaul
// mods' new items/spells/skills are discovered automatically.
// is_persistable() (buff_filters.cpp) is the consumer of this set.
extern std::unordered_set<int> g_tracked_speffects;

// Subset of the consumable-sourced ids that came from a Flask of Wondrous
// Physick crystal tear (EquipParamGoods.goodsType == 10). Lets [persistence]
// persist_physick_tears drop tears as a group without touching other
// consumables; force_persist_ids can still rescue an individual id.
extern std::unordered_set<int> g_physick_effects;

// Populate the sets. Requires params to be loaded; call once from the worker
// after wait_for_params succeeds. If params never loaded, the set stays empty
// and only force/always-persist ids remain persistable.
void build_tracked_speffects();

// True if `id` is a weapon/shield enchant (belongs to the weapon, not the
// character). Three signals, any one suffices:
//  * discovered from a grease (definitionally an enchant);
//  * its discovery chain carries a weapon-enchant VFX row
//    (SpEffectVfxParam.enchantStartDmyId_* / soulParamIdForWepEnchant) --
//    the robust signal: sibling mods (IWB stacking) zero spCategory and
//    overhaul regulations renumber it, but nothing touches the enchant vfx;
//  * its spCategory matches one learned from the grease-sourced enchants
//    (vanilla-regulation belt, kept alongside the vfx check).
// Used to decide weapon-bound vs character-wide without a hand-kept id list.
bool is_enchant_effect(int id);

// True if `id` should survive ANY weapon swap (character-wide semantics in
// weapon-memory step 3): the hard-coded g_always_persist seed set, plus every
// discovered spell/skill (AoW) buff that is NOT an enchant-category effect.
// Grease-sourced ids are never character-wide.
bool is_character_wide_buff(int id);

} // namespace pb
