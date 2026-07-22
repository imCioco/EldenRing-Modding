#pragma once

#include <param/param.hpp>

namespace iwb {

bool is_self_buff(const from::paramdef::SP_EFFECT_PARAM_ST* r);

// Engine state / animation / environment effects that must never be made
// permanent (re-using PersistentBuffs' Paramdex-sourced blocklist). These are
// matched by id only -- robust even if struct field offsets drift across game
// versions. 9621 = Roundtable "Disallow Hostile Actions" (the no-combat lock).
bool is_system_effect(int id);

// Does this SpEffect actually improve a combat/vitality stat? A *positive*
// whitelist: true only if at least one combat/vitality stat is improved. Still
// used by the `[discover] dump` diagnostics for labelling; the apply pass uses
// the `is_debuff` blacklist below instead (see its note).
bool is_beneficial_buff(const from::paramdef::SP_EFFECT_PARAM_ST* r);

// Does this SpEffect *harm* the bearer? A blacklist -- true if it lowers your
// max/current HP/FP/stamina, drains them over time, weakens attack or defense,
// raises damage taken, or lowers status resistance. This is the gate the apply
// pass uses for consumables and spell buffs: "extend everything on a timer
// EXCEPT debuffs" (user request), rather than the narrower "must be a positive
// buff" whitelist -- which was dropping ally-and-self buffs (Golden Vow spell,
// Barrier of Gold, Rallying Standard) whose timer row carries no stat field of
// its own. Neutral/utility effects are NOT debuffs, so they pass; genuine
// engine states are still stopped by `is_system_effect` + the protected set.
bool is_debuff(const from::paramdef::SP_EFFECT_PARAM_ST* r);

// Does this SpEffect target *only* enemies (hostile/opposing) with no self/
// player/ally flag? Those are offensive spell payloads (e.g. a sorcery bullet's
// hit effect), not buffs you or your allies receive -- excluded so following
// spell bullets doesn't sweep in enemy-only effects. Effects with no target
// flags at all are NOT foe-only (many buff rows leave targeting to the bullet),
// so they still pass.
bool is_foe_only(const from::paramdef::SP_EFFECT_PARAM_ST* r);

bool is_grease(const from::paramdef::EQUIP_PARAM_GOODS_ST& row);

} // namespace iwb
