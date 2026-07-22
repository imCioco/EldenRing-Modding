#include "buff_discovery.hpp"

#include "buff_filters.hpp"   // sp_row, is_self_buff, is_debuff, is_foe_only, is_grease, kChainMaxDepth
#include "config.hpp"         // g_always_persist
#include "utils.hpp"          // flog

#include <unordered_map>
#include <vector>

#include <param/param.hpp>

namespace pb {

std::unordered_set<int> g_tracked_speffects;
std::unordered_set<int> g_physick_effects;

namespace {

// Which source(s) an id was discovered from -- drives the weapon-bound vs
// character-wide classification (is_character_wide_buff).
enum SourceBits : unsigned {
    kSrcGrease     = 1u << 0,
    kSrcSpell      = 1u << 1,
    kSrcConsumable = 1u << 2,
    kSrcBehavior   = 1u << 3, // weapon skills / ashes of war
    kSrcUtility    = 1u << 5, // lantern & co -- trusted non-buff conveniences
    // Not a source: the id's chain carries a weapon-enchant VFX (see
    // has_enchant_vfx). Tagged per CHAIN at discovery so cycled/replace nodes
    // without their own vfx (e.g. "Scholar's Shield (Cycled)") still classify
    // weapon-bound like their root.
    kSrcEnchantVfx = 1u << 6,
};
std::unordered_map<int, unsigned> g_source_of;

// EquipParamGoods.goodsType for Flask of Wondrous Physick crystal tears
// (Smithbox ER GOODS_TYPE enum: 9 = Wondrous Physick, 10 = Wondrous Physick
// Tear). goodsType is how the mixing menu itself finds tears, so overhaul-
// added tears carry it too -- more robust than the 11000..11031 / 2011000..
// 2011070 goods id ranges.
constexpr unsigned char kGoodsTypePhysickTear = 10;

// EquipParamGoods.goodsType == 7 = SPIRIT SUMMON ashes (Crystalian Ashes, ...).
// They sort in group 20 but resolve to a "[Spirit Summon] X Ashes" SpEffect --
// transient summon STATE, never a player buff. Persisting/re-applying one on
// respawn is the same class of bug as the 9540 / 20200000 spirit-summon states
// this filter already blocks (blocks re-summoning). Verified in the vanilla
// regulation: all 561 goodsType-7 rows are spirit summons, none are buffs.
constexpr unsigned char kGoodsTypeSpiritSummon = 7;

// The Lantern (EquipParamGoods 2070). Its light is SpEffect 3245/3246
// ("[Item] Lantern - Right/Left"), which the death wipe removes like any
// other effect -- but the beneficial-stat gate drops it (a light buffs no
// stat), so the mod never restored it and the player respawned in the dark.
// Tracked as a TRUSTED utility source: the goods row's refIds are walked
// (overhaul-proof), with the two vanilla ids as a fallback.
constexpr int kLanternGoodsId = 2070;
constexpr int kLanternSpEffects[] = { 3245, 3246 };

// Weapon-enchant spCategory values, learned from the grease-sourced effects.
// The engine uses spCategory for mutual exclusivity (a new enchant replaces
// the old one), so blade spells share these categories with greases (verified
// against the vanilla regulation: greases + blade spells are 162 right / 163
// left) -- which is exactly the "belongs to the weapon" class. spCategory 0
// (uncategorized) is never treated as an enchant category.
//
// ⚠ spCategory is NOT reliable on its own: InfiniteWeaponBuffs' stacking
// option zeroes it on every patched buff (both mods run in one process on the
// same loaded params, in either order), and overhaul regulations can renumber
// it. That's how Order's Blade got classified character-wide and re-applied
// onto EVERY weapon (the "same buff on all weapons" bug). The robust,
// mod-proof signal is the VFX row: a weapon enchant's SpEffectVfxParam
// carries the on-blade attachment (enchantStartDmyId_* dummy-poly chain
// and/or soulParamIdForWepEnchant) -- greases, blade spells, and AoW enchant
// arts all have it; body buffs (Golden Vow, FGMS, consumables) don't; and no
// duration/stacking mod touches SpEffectVfxParam. Verified against the
// vanilla regulation: the vfx check selects exactly the 11 armament/shield
// enchant spells + the enchant arts (Cragblade &c). Determination / Royal
// Knight's Resolve carry the enchant vfx too (the weapon glow) but stay
// character-wide via the g_always_persist seed, which wins first.
std::unordered_set<int> g_enchant_categories;

bool vfx_row_is_enchant(int vfx_id) {
    if (vfx_id < 0) return false;
    auto [row, ok] = from::param::SpEffectVfxParam[vfx_id];
    if (!ok) return false;
    return row.enchantStartDmyId_0 >= 0 || row.soulParamIdForWepEnchant > 0;
}

bool has_enchant_vfx(const from::paramdef::SP_EFFECT_PARAM_ST* r) {
    if (!r) return false;
    const int vids[] = { r->vfxId,  r->vfxId1, r->vfxId2, r->vfxId3,
                         r->vfxId4, r->vfxId5, r->vfxId6, r->vfxId7 };
    for (int v : vids)
        if (vfx_row_is_enchant(v)) return true;
    return false;
}

// Built-in exclusions:
//   130 = Spectral Steed Whistle (Torrent) -- horse summon, never a buff.
const std::unordered_set<int> kHorseSummonGoods = { 130 };

// Magic/Behavior/Goods refCategory / refType values (same enum everywhere):
constexpr int kRefBullet   = 1;
constexpr int kRefSpEffect = 2;

// Cap for HitBulletID recursion (bullets spawning bullets).
constexpr int kBulletMaxDepth = 3;

from::paramdef::BULLET_PARAM_ST* bullet_row(int id) {
    if (id < 0) return nullptr;
    auto [row, ok] = from::param::Bullet[id];
    return ok ? &row : nullptr;
}
from::paramdef::BEHAVIOR_PARAM_ST* behavior_row(int id) {
    if (id < 0) return nullptr;
    { auto [row, ok] = from::param::BehaviorParam_PC[id]; if (ok) return &row; }
    { auto [row, ok] = from::param::BehaviorParam[id];    if (ok) return &row; }
    return nullptr;
}

// Every SpEffect a bullet can apply: the shooter effect plus the hit effects,
// following spawn-on-hit child bullets a few levels deep. This is how AOE/ally
// buffs (Golden Vow, blessings, buff pots) actually deliver their effect.
void add_bullet_speffects(int bulletId, std::vector<int>& out, int depth = 0) {
    if (depth > kBulletMaxDepth) return;
    auto* bl = bullet_row(bulletId);
    if (!bl) return;
    const int ids[] = { bl->spEffectIDForShooter, bl->spEffectId0,
                        bl->spEffectId1, bl->spEffectId2,
                        bl->spEffectId3, bl->spEffectId4 };
    for (int s : ids) if (s >= 0) out.push_back(s);
    if (bl->HitBulletID >= 0 && bl->HitBulletID != bulletId)
        add_bullet_speffects(bl->HitBulletID, out, depth + 1);
}

// Resolve the SpEffect(s) a goods row can apply: its refId(s) as a SpEffect, as
// a Bullet when refCategory==1 (projectile), and the behaviorId -> BehaviorParam
// path. Mirrors IWB's gather_goods_entry_speffects.
void gather_goods_entry_speffects(const from::paramdef::EQUIP_PARAM_GOODS_ST& row,
                                  std::vector<int>& out) {
    const int refs[2] = { row.refId_default, row.refId_1 };
    for (int r : refs) {
        if (r < 0) continue;
        if (sp_row(r)) out.push_back(r);                        // refId as SpEffect
        if (row.refCategory == kRefBullet) add_bullet_speffects(r, out);
    }
    if (row.behaviorId > 0) {
        if (auto* b = behavior_row(row.behaviorId)) {
            if (b->refType == kRefSpEffect && b->refId >= 0) out.push_back(b->refId);
            else if (b->refType == kRefBullet) add_bullet_speffects(b->refId, out);
        }
    }
}

// Walk the SpEffect replace/cycle chain, collecting EVERY reachable id (any
// duration -- PersistentBuffs must keep buffs an infinite-duration mod like IWB
// may already have patched to -1).
void collect_all_chain(int startId, std::unordered_set<int>& out, int depth = 0) {
    if (startId < 0 || depth > kChainMaxDepth) return;
    if (!out.insert(startId).second) return; // already visited
    auto* r = sp_row(startId);
    if (!r) return;
    collect_all_chain(r->replaceSpEffectId,         out, depth + 1);
    collect_all_chain(r->cycleOccurrenceSpEffectId, out, depth + 1);
}

// Track every chain node reachable from `entries`, tagged with `src`.
//  * trusted == true  -> every node with a row is kept (grease refs, spell
//    direct refs -- narrow sources whose whole chain IS the buff).
//  * trusted == false -> a node is kept unless it HARMS the bearer (is_debuff)
//    or targets foes only (is_foe_only) -- the "accept everything except a
//    debuff" gate for the wide sources (consumables, bullets, weapon-skill
//    behaviors). This is the InfiniteWeaponBuffs model: the old positive gate
//    (is_beneficial_buff, keep only rows with a recognised stat up) dropped
//    self+ally buffs whose timer row carries no stat of its own (Golden Vow
//    spell, Rallying Standard, Blessing of the Erdtree). Now those track; only
//    self-debuffs and enemy-only nodes are excluded, so they can't be
//    re-applied. (is_system_effect still hard-blocks known engine-state ids
//    downstream in is_persistable.)
int track_entries(const std::vector<int>& entries, unsigned src, bool trusted) {
    // Enchant-vfx tagging is per ENTRY chain: if any node reachable from one
    // entry carries the weapon-enchant vfx, the whole chain is an enchant --
    // cycled/replace helper nodes usually have no vfx of their own.
    for (int e : entries) {
        std::unordered_set<int> one;
        collect_all_chain(e, one);
        bool enchant = false;
        for (int node : one)
            if (has_enchant_vfx(sp_row(node))) { enchant = true; break; }
        if (enchant)
            for (int node : one)
                if (sp_row(node)) g_source_of[node] |= kSrcEnchantVfx;
    }
    std::unordered_set<int> chain;
    for (int e : entries) collect_all_chain(e, chain);
    int added = 0;
    for (int node : chain) {
        const auto* r = sp_row(node);
        if (!r) continue;
        if (!trusted && (is_debuff(r) || is_foe_only(r))) continue;
        g_source_of[node] |= src;
        if (g_tracked_speffects.insert(node).second) ++added;
    }
    return added;
}

} // namespace

void build_tracked_speffects() {
    g_tracked_speffects.clear();
    g_physick_effects.clear();
    g_source_of.clear();
    g_enchant_categories.clear();
    int n_grease = 0, n_spell = 0, n_consum = 0, n_skill = 0;
    int n_physick = 0, n_utility = 0;

    // 1. Greases (weapon/shield buffs) -- trusted narrow source, chain-walked.
    //    Their spCategory values seed the enchant-category set used to tell
    //    weapon-bound enchants apart from character-wide buffs.
    for (auto [id, row] : from::param::EquipParamGoods) {
        if (!is_grease(row)) continue;
        std::vector<int> entries;
        for (int r : { row.refId_default, row.refId_1 })
            if (r >= 0 && sp_row(r)) entries.push_back(r);
        n_grease += track_entries(entries, kSrcGrease, /*trusted=*/true);
    }
    for (const auto& [id, src] : g_source_of) {
        if (!(src & kSrcGrease)) continue;
        const auto* r = sp_row(id);
        // Only nodes that carry the enchant vfx seed a category: grease chains
        // also contain plain helper nodes whose spCategory (20 = generic item
        // buff) is shared by unrelated character-wide buffs -- seeding those
        // would misclassify them as weapon-bound.
        if (r && r->spCategory > 0 && has_enchant_vfx(r))
            g_enchant_categories.insert(static_cast<int>(r->spCategory));
    }

    // 2. Spells (sorceries / incantations). Direct SpEffect refs are trusted
    //    (self-buffs like Flame Grant Me Strength). Projectile refs are walked
    //    through the Bullet table with the not-a-debuff gate -- this is how AOE /
    //    ally incantations (Golden Vow 6600 -> bullet -> 1660000...) deliver
    //    their buff; missing them was the "incant buffs lost on death" bug.
    for (auto [id, row] : from::param::Magic) {
        const int  refs[] = { row.refId1, row.refId2, row.refId3, row.refId4,
                              row.refId5, row.refId6, row.refId7, row.refId8,
                              row.refId9, row.refId10 };
        const int  cats[] = { row.refCategory1, row.refCategory2, row.refCategory3,
                              row.refCategory4, row.refCategory5, row.refCategory6,
                              row.refCategory7, row.refCategory8, row.refCategory9,
                              row.refCategory10 };
        std::vector<int> direct, from_bullets;
        for (int i = 0; i < 10; ++i) {
            const int r = refs[i];
            if (r < 0) continue;
            if (cats[i] == kRefBullet)      add_bullet_speffects(r, from_bullets);
            else if (sp_row(r))             direct.push_back(r);
        }
        n_spell += track_entries(direct,       kSrcSpell, /*trusted=*/true);
        n_spell += track_entries(from_bullets, kSrcSpell, /*trusted=*/false);
    }

    // 3. Consumables -- ALL goods rows (any sortGroupId, so overhaul items are
    //    picked up wherever they sort), gated per chain node to self-targeted
    //    non-debuffs (drops enemy items and self-harming riders; keeps neutral
    //    utility buffs the old positive gate missed), following the bullet/
    //    behavior indirection so projectile pots (DLC Golden Vow 2003170) count.
    for (auto [id, row] : from::param::EquipParamGoods) {
        if (is_grease(row)) continue; // already tracked as source 1
        if (row.isSummonHorse || kHorseSummonGoods.count(static_cast<int>(id))) continue;
        if (row.goodsType == kGoodsTypeSpiritSummon) continue; // spirit ashes: summon state, not a buff
        // Physick crystal tears are tagged so [persistence] persist_physick_tears
        // can gate them as a group (see is_persistable). They still go through
        // the same not-a-debuff/not-foe-only gate as any consumable, so a harmful
        // tear (Ruptured's explosion etc.) is never tracked either way.
        const bool is_tear = row.goodsType == kGoodsTypePhysickTear;
        std::vector<int> entries;
        gather_goods_entry_speffects(row, entries);
        std::unordered_set<int> chain;
        for (int e : entries) collect_all_chain(e, chain);
        for (int node : chain) {
            const auto* r = sp_row(node);
            // Gate = InfiniteWeaponBuffs' "accept everything except a debuff"
            // (is_debuff || is_foe_only), NOT the old is_self_buff requirement.
            // A self-applied consumable can carry NO target flag at all (the item
            // use implies self) -- e.g. Starlight Shards' FP-regen SpEffect
            // (501290) has effectTargetSelf/Player both 0, so is_self_buff dropped
            // it and it was never persisted (user report). is_foe_only still keeps
            // enemy-only item effects out; over-tracking a neutral node that never
            // lands on the player is inert (PB only re-applies ACTIVE ids).
            if (!r || is_debuff(r) || is_foe_only(r)) continue;
            g_source_of[node] |= kSrcConsumable;
            if (is_tear && g_physick_effects.insert(node).second) ++n_physick;
            if (g_tracked_speffects.insert(node).second) ++n_consum;
        }
    }

    // 4. Weapon skills / ashes of war -- every behavior a player action can
    //    trigger, not-a-debuff gated. This discovers ALL AoW self-buffs (Endure,
    //    roars, Golden Vow, overhaul-added skills...) instead of the old
    //    hard-coded 32-id list (kept as a seed in g_always_persist). Attack
    //    behaviors reference AtkParam (refType 0) and are skipped naturally.
    auto scan_behaviors = [&](auto& table) {
        for (auto [id, row] : table) {
            std::vector<int> entries;
            if (row.refType == kRefSpEffect && row.refId >= 0)
                entries.push_back(row.refId);
            else if (row.refType == kRefBullet)
                add_bullet_speffects(row.refId, entries);
            if (!entries.empty())
                n_skill += track_entries(entries, kSrcBehavior, /*trusted=*/false);
        }
    };
    scan_behaviors(from::param::BehaviorParam_PC);
    scan_behaviors(from::param::BehaviorParam);

    // 5. Utility conveniences the death wipe also strips: the Lantern light.
    //    TRUSTED (no field gate -- a light buffs no stat, which is exactly why
    //    the consumable scan drops it and the player respawned in the dark).
    //    Walk the goods row's refIds (overhaul-proof), then make sure the two
    //    vanilla ids are in even if the row resolves nothing (the toggle may
    //    be HKS-applied rather than refId-applied).
    {
        std::vector<int> entries;
        { auto [row, ok] = from::param::EquipParamGoods[kLanternGoodsId];
          if (ok) gather_goods_entry_speffects(row, entries); }
        for (int id : kLanternSpEffects) if (sp_row(id)) entries.push_back(id);
        n_utility += track_entries(entries, kSrcUtility, /*trusted=*/true);
    }

    size_t n_enchant_vfx = 0;
    for (const auto& [id, src] : g_source_of)
        if (src & kSrcEnchantVfx) ++n_enchant_vfx;
    flog("buff filter: tracked %zu SpEffect(s) [greases %d, spells %d, "
         "consumables %d (physick tears %d), skills/AoW %d, "
         "utility %d] -- param-driven source allowlist (%zu enchant categor%s, "
         "%zu enchant-vfx effect(s))",
         g_tracked_speffects.size(), n_grease, n_spell, n_consum, n_physick,
         n_skill, n_utility, g_enchant_categories.size(),
         g_enchant_categories.size() == 1 ? "y" : "ies", n_enchant_vfx);
}

bool is_enchant_effect(int id) {
    const auto it = g_source_of.find(id);
    if (it != g_source_of.end() &&
        (it->second & (kSrcGrease | kSrcEnchantVfx))) return true;
    const auto* r = sp_row(id);
    if (!r) return false;
    // spCategory belt (vanilla regulations) ...
    if (r->spCategory > 0 &&
        g_enchant_categories.count(static_cast<int>(r->spCategory)) != 0)
        return true;
    // ... and the vfx suspenders for ids outside the discovery scan
    // (force_persist_ids etc.). See the note above vfx_row_is_enchant.
    return has_enchant_vfx(r);
}

bool is_character_wide_buff(int id) {
    if (g_always_persist.count(id)) return true; // hand-verified seed set
    const auto it = g_source_of.find(id);
    if (it == g_source_of.end()) return false;   // not discovered -> no opinion
    if (it->second & kSrcGrease) return false;   // greases are weapon-bound
    if (!(it->second & (kSrcSpell | kSrcBehavior | kSrcConsumable))) return false;
    return !is_enchant_effect(id);
}

} // namespace pb
