#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <string>
#include <unordered_set>
#include <vector>

#include "ini.hpp"

namespace iwb {

extern HINSTANCE g_hinst;
extern bool      g_debug;

// ---- known vanilla item ids --------------------------------
// Used in addition to the live param flags, so categorization stays
// correct even if a regulation doesn't set the expected flag.
//   130     = Spectral Steed Whistle (Torrent) -- always protected.
//   2003170 = DLC "Golden Vow" thrown pot -- buffs the thrower via a
//             bullet, so it isn't a plain sort-group-20 consumable.
//   1290    = Starlight Shards -- sorts in group 10 (stat/FP items), so the
//             sort-group-20 consumable scan misses it; allowlisted here so its
//             buff (e.g. an overhaul FP-regen effect, default ~60s) is extended.
//             Vanilla Starlight Shards restore FP instantly (no timer) -> the
//             "no timer" check safely leaves them unchanged (logged as such).
inline const int kHorseSummonGoodsBuiltin[]      = { 130 };
inline const int kExtraConsumableGoodsBuiltin[]  = { 2003170, 1290 };

// Built-in SpEffect ids for Ash-of-War buffs. Ashes apply their buff through a
// weapon-skill behavior that can't be reached cleanly from the gem param
// (SwordArtsParam has no SpEffect/bullet ref -- verified), so they're driven by
// this curated allowlist; extend with [ashes_of_war] speffect_ids. Ids are the
// buff-carrying rows (the "- ... Buff" effects, incl. No-FP variants) from
// soulsmods/Paramdex ER SpEffectParam names. This now aims to cover EVERY
// buff-granting Ash of War, including the element weapon-enchant arts (Cragblade,
// Flaming Strike, infusions, etc.) -- previously excluded by choice. Intentionally
// NOT listed: the bare trigger/FP-consume rows, "- VFX"/"- Icon"/"- Bullet" rows,
// the on-hit status-application rows ("- Frost"/"- Poison"/"- Bleed" without
// "Buff"), and the *self-cost* rows (Seppuku "- Self Bleed", Vyke "- Self
// Madness", "- HP Burn", Determination/RKR "- Critical Damage Debuff") -- those
// aren't buffs. `dump=1`'s "ASH-OF-WAR ALLOWLIST CHECK" verifies each id exists +
// is timed on the live regulation.
inline const std::vector<int> kAshOfWarBuffSpEffectsBuiltin = {
    801, 803, 806, 809, 811, 814, // Barricade Shield (hardness/guard buff)
    821, 823,                     // Sacred Blade (holy enchant)
    826, 828,                     // Chilling Mist (frost enchant)
    831, 833,                     // Poisonous Mist (poison enchant)
    841, 843, 846, 848,           // Roar
    1511, 1513,                   // Stormhawk Axe (lightning enchant)
    1540,                         // Raptor of the Mists (protection)
    1586, 1588,                   // Jellyfish Shield
    1627, 1629,                   // Mohgwyn's Sacred Spear (fire enchant)
    1640, 1641,                   // Holy Ground (defence + HP regen)
    1650, 1651, 1655, 1656,       // Endure (poise)
    1676, 1678,                   // Lightning Slash (lightning enchant)
    1681, 1683, 1686, 1688,       // Barbaric/Milos Roar
    1691, 1693, 1696, 1698,       // Determination
    1701, 1703, 1706, 1708,       // Royal Knight's Resolve
    1716, 1718,                   // Golden Tempering (holy enchant)
    1721, 1723,                   // Dark Moon Greatsword (frost/magic enchant)
    1730, 1732,                   // Golden Vow / Golden Great Arrow
    1755, 1758,                   // Seppuku (damage/bleed buff)
    1765, 1766, 1767,             // Assassin's Gambit
    1776, 1778,                   // Flaming Strike (fire enchant)
    1791, 1793,                   // Eclipse Shotel (holy/blight enchant)
    1806, 1808,                   // Ruinous Ghostflame (frost/magic enchant)
    1811, 1813, 1816, 1818,       // War Cry
    1821, 1823, 1826, 1828,       // Cragblade
    1835, 1836,                   // Last Rites (self + allies)
    1841, 1843, 1846, 1848,       // Sacred Order
    1850, 1851, 1852,             // Oath of Vengeance (stat + poise)
    1861, 1863, 1866, 1868,       // Braggart's Roar
    1870, 1871,                   // Shared Order (self + allies)
    1881, 1883,                   // Sword of St. Trina (sleep enchant)
    1891, 1893,                   // Ice Lightning Sword (frost/lightning enchant)
};

// ---- dual-wield off-hand mirror: weapon-art enchant pairs ----
// A weapon buff is two SpEffect rows: Right (wepParamChange==1) enchants the
// right weapon, Left (==2) the left. Greases are paired dynamically from the
// live params (see build_dualwield_mirror); weapon-skill (Ash of War) enchants
// aren't goods, so their Right->Left pairs are listed here (ids from the
// soulsmods/Paramdex-based reference set). Extend via [dual_wield] extra_pairs.
// NB: these are hand-targeted *weapon enchant* arts -- a different set from
// kAshOfWarBuffSpEffectsBuiltin above, which are character self-buffs (Roar,
// Determination, ...) that don't target a hand and need no mirroring.
struct HandPair { int right; int left; };
inline const HandPair kDualWieldArtPairsBuiltin[] = {
    { 821, 823 },              // Sacred Blade
    { 826, 828 },              // Chilling Mist
    { 831, 833 },              // Poison Mist
    { 1676, 1678 },            // Lightning Slash
    { 1721, 1723 },            // Moonlight Greatsword
    { 1755, 1758 },            // Seppuku
    { 1776, 1778 },            // Flaming Strike
    { 1806, 1808 },            // Ruinous Ghostflame
    { 1821, 1823 },            // Cragblade
    { 1891, 1893 },            // Ice Lightning Sword
    { 20000891, 20000896 },    // Flame Skewer
    { 20000961, 20000966 },    // Flame Spear
};

// Sort groups (EquipParamGoods.sortGroupId) we treat as categories.
constexpr int kSortGroupGrease     = 70; // greases (vanilla + DLC)
constexpr int kSortGroupConsumable = 20; // buff/heal foods, livers, boluses...

// EquipParamGoods.goodsType == 7 = SPIRIT SUMMON ashes (Crystalian Ashes, ...).
// They sort in group 20 alongside real consumables, so the consumable scan
// picks them up -- but their refId resolves to a "[Spirit Summon] X Ashes"
// SpEffect, i.e. transient SUMMON STATE, not a player buff. Extending those to
// 600s is wrong (it messes with the summon, and was the "why are spirit ashes
// affected?" report). Verified in the vanilla regulation: all 561 goodsType-7
// rows are spirit summons, none are buff consumables -> safe to exclude wholesale.
constexpr int kGoodsTypeSpiritSummon = 7;

// Guard against pathological / cyclic SpEffect chains.
constexpr int kChainMaxDepth = 8;

// ---- parse "130, 2003170" into a set of ints ----------------
void parse_int_list(const std::string& spec, std::unordered_set<int>& out);
// ---- parse "821:823, 1821:1823" into right:left pairs -------
void parse_pair_list(const std::string& spec, std::vector<HandPair>& out);

// Everything derived from config.ini + the built-in lists above, ready to
// hand to apply() / dump_candidates().
struct Config {
    Ini ini;
    std::unordered_set<int> extraGoods, horseGoods, ashIds;
    // Raw SpEffect ids to extend directly (from [consumables] extra_speffect_ids),
    // for buffs whose id you know but automatic discovery doesn't reach. Distinct
    // from extraGoods (which are EquipParamGoods ITEM ids).
    std::unordered_set<int> extraSpeffects;
    std::vector<HandPair> artPairs;
};

// Loads config.ini (or falls back to built-in defaults if it's missing),
// sets up the debug console, and resolves the built-in + .ini-configured id
// lists. Call once from the worker thread (blocking file I/O).
Config load_config();

} // namespace iwb
