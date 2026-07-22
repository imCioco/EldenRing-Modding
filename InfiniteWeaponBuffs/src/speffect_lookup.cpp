#include "speffect_lookup.hpp"
#include "config.hpp"

namespace iwb {

// ---- param row accessors (binary search by id) --------------
from::paramdef::SP_EFFECT_PARAM_ST* sp_row(int id) {
    if (id < 0) return nullptr;
    auto [row, ok] = from::param::SpEffectParam[id];
    return ok ? &row : nullptr;
}
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

void add_bullet_speffects(int bulletId, std::vector<int>& out) {
    auto* bl = bullet_row(bulletId);
    if (!bl) return;
    const int ids[] = { bl->spEffectIDForShooter, bl->spEffectId0,
                        bl->spEffectId1, bl->spEffectId2,
                        bl->spEffectId3, bl->spEffectId4 };
    for (int s : ids) if (s >= 0) out.push_back(s);
}

void gather_goods_entry_speffects(const from::paramdef::EQUIP_PARAM_GOODS_ST& row,
                                  std::vector<int>& out) {
    const int refs[2] = { row.refId_default, row.refId_1 };
    for (int r : refs) {
        if (r < 0) continue;
        if (sp_row(r)) out.push_back(r);            // refId as SpEffect
        if (row.refCategory == 1) add_bullet_speffects(r, out); // refId as Bullet
    }

    if (row.behaviorId > 0) {
        if (auto* b = behavior_row(row.behaviorId)) {
            if (b->refType == 2 && b->refId >= 0) out.push_back(b->refId);
            else if (b->refType == 1)             add_bullet_speffects(b->refId, out);
        }
    }
}

void collect_all_chain(int startId, std::unordered_set<int>& out, int depth) {
    if (startId < 0 || depth > kChainMaxDepth) return;
    if (!out.insert(startId).second) return; // already visited
    auto* r = sp_row(startId);
    if (!r) return;
    collect_all_chain(r->replaceSpEffectId,         out, depth + 1);
    collect_all_chain(r->cycleOccurrenceSpEffectId, out, depth + 1);
}

void collect_timed_chain(int startId, std::vector<int>& out,
                         std::unordered_set<int>& visited, int depth) {
    if (startId < 0 || depth > kChainMaxDepth) return;
    if (!visited.insert(startId).second) return;
    auto* r = sp_row(startId);
    if (!r) return;
    if (r->effectEndurance > 0.0f) out.push_back(startId);
    collect_timed_chain(r->replaceSpEffectId,         out, visited, depth + 1);
    collect_timed_chain(r->cycleOccurrenceSpEffectId, out, visited, depth + 1);
}

void gather_magic_entries(const from::paramdef::MAGIC_PARAM_ST& row,
                          std::vector<int>& out) {
    // A spell reaches its buff SpEffect the same way a goods item does, so this
    // mirrors gather_goods_entry_speffects. Each Magic ref is (refCategoryN,
    // refIdN): category 1 = Projectile (Bullet), 2 = SpEffect. Self-only buffs
    // (Flame Grant Me Strength, Magic Fortification) put the buff SpEffect
    // directly in refId (category 2) -- those already worked. Self+ally buffs
    // (Golden Vow, Barrier of Gold, Rallying Standard, Blessing of the Erdtree)
    // deliver the buff through an AoE Bullet (category 1): the real effect lives
    // on that bullet's shooter/hit SpEffects, so refId alone misses it. We now
    // follow the bullet as well, then let the caller walk the replace/cycle
    // chain and filter (self/ally, non-debuff, timed).
    const int refs[] = {
        row.refId1, row.refId2, row.refId3, row.refId4,  row.refId5,
        row.refId6, row.refId7, row.refId8, row.refId9,  row.refId10,
    };
    const int cats[] = {
        row.refCategory1, row.refCategory2, row.refCategory3, row.refCategory4,
        row.refCategory5, row.refCategory6, row.refCategory7, row.refCategory8,
        row.refCategory9, row.refCategory10,
    };
    for (int i = 0; i < 10; ++i) {
        const int r = refs[i];
        if (r < 0) continue;
        if (sp_row(r)) out.push_back(r);                // refId as SpEffect
        if (cats[i] == 1) add_bullet_speffects(r, out); // refId as Bullet (AoE)
    }

    if (row.behaviorId > 0) {
        if (auto* b = behavior_row(row.behaviorId)) {
            if (b->refType == 2 && b->refId >= 0) out.push_back(b->refId);
            else if (b->refType == 1)             add_bullet_speffects(b->refId, out);
        }
    }
}

} // namespace iwb
