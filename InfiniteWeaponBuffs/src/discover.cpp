#include "discover.hpp"
#include "speffect_lookup.hpp"
#include "buff_filters.hpp"
#include "horse_protection.hpp"
#include "dual_wield.hpp"
#include "utils.hpp"

#include <string>

#include <param/param.hpp>

namespace iwb {

void dump_candidates(const std::unordered_set<int>& extraGoods,
                     const std::unordered_set<int>& horseGoods,
                     const std::unordered_set<int>& ashIds,
                     const std::vector<HandPair>& artPairs) {
    flog("discover: tgt flags S=self P=player F=friend O=oppose/enemy; "
         "DBF=debuff FOE=enemy-only (both dropped for consumables/spells), PROT=protected");

    // --- protected (horse-summon) effects -------------------
    std::unordered_set<int> protectedSp;
    build_protected_set(horseGoods, protectedSp);
    {
        std::string ids;
        for (int id : protectedSp) ids += std::to_string(id) + " ";
        flog("discover: ---- PROTECTED SpEffects (horse-summon, never patched) ----");
        flog("discover: %zu protected: %s", protectedSp.size(), ids.c_str());
    }

    auto tgt = [](const from::paramdef::SP_EFFECT_PARAM_ST* r) {
        std::string t;
        if (r && (r->effectTargetSelf   || r->effectTargetSelfTarget))      t += 'S';
        if (r && r->effectTargetPlayer)                                     t += 'P';
        if (r && (r->effectTargetFriend || r->effectTargetFriendlyTarget))  t += 'F';
        if (r && (r->effectTargetEnemy  || r->effectTargetOpposeTarget))    t += 'O';
        return t.empty() ? std::string("-") : t;
    };
    auto resolved = [&](const std::vector<int>& entries, bool followChain) {
        std::string s;
        for (int e : entries) {
            std::vector<int> timed;
            if (followChain) {
                std::unordered_set<int> v;
                collect_timed_chain(e, timed, v);
            } else {
                auto* r = sp_row(e);
                if (r && r->effectEndurance > 0.0f) timed.push_back(e);
            }
            for (int t : timed) {
                auto* r = sp_row(t);
                char buf[112];
                // DBF/FOE = would be dropped by the consumable/spell filter.
                std::snprintf(buf, sizeof(buf), "%d(dur=%.1f tgt=%s%s%s%s) ", t,
                              r ? r->effectEndurance : 0.0f, tgt(r).c_str(),
                              protectedSp.count(t) ? " PROT" : "",
                              is_debuff(r)   ? " DBF" : "",
                              is_foe_only(r) ? " FOE" : "");
                s += buf;
            }
        }
        return s.empty() ? std::string("-") : s;
    };

    // --- greases --------------------------------------------
    flog("discover: ---- GREASES (isEnhance/isShieldEnchant/sortGroup 70) ----");
    int gn = 0;
    for (auto [id, row] : from::param::EquipParamGoods) {
        if (!is_grease(row)) continue;
        std::vector<int> entries = { row.refId_default, row.refId_1 };
        flog("grease goods=%d sortGrp=%d enh=%d/%d -> %s",
             static_cast<int>(id), static_cast<int>(row.sortGroupId),
             row.isEnhance ? 1 : 0, row.isShieldEnchant ? 1 : 0,
             resolved(entries, false).c_str());
        ++gn;
    }
    flog("discover: %d grease goods", gn);

    // --- consumables in scope -------------------------------
    flog("discover: ---- CONSUMABLES (sortGroup 20 + extra_goods allowlist) ----");
    int cn = 0;
    for (auto [id, row] : from::param::EquipParamGoods) {
        const bool inScope =
            static_cast<int>(row.sortGroupId) == kSortGroupConsumable ||
            extraGoods.count(static_cast<int>(id));
        if (!inScope) continue;
        const bool horse =
            row.isSummonHorse || horseGoods.count(static_cast<int>(id));
        std::vector<int> entries;
        gather_goods_entry_speffects(row, entries);
        flog("consumable goods=%d type=%d sortGrp=%d refCat=%d refs=[%d,%d] behav=%d%s -> %s",
             static_cast<int>(id), static_cast<int>(row.goodsType),
             static_cast<int>(row.sortGroupId), static_cast<int>(row.refCategory),
             row.refId_default, row.refId_1, row.behaviorId,
             horse ? " HORSE(skipped)" : "",
             resolved(entries, true).c_str());
        ++cn;
    }
    flog("discover: %d consumable goods in scope", cn);

    // --- potential misses -----------------------------------
    // Goods NOT in any category that still reach a self/player timed buff:
    // candidates we currently skip. Only effects lasting >= kMissDurFloor are
    // shown, to skip the flood of 0.1s instant cures / craft / summon triggers.
    constexpr float kMissDurFloor = 5.0f;
    flog("discover: ---- POTENTIAL MISSES (uncategorized self buffs, dur>=%.0fs) ----",
         kMissDurFloor);
    int mn = 0;
    for (auto [id, row] : from::param::EquipParamGoods) {
        if (is_grease(row)) continue;
        if (static_cast<int>(row.sortGroupId) == kSortGroupConsumable) continue;
        if (extraGoods.count(static_cast<int>(id))) continue;
        if (row.isSummonHorse || horseGoods.count(static_cast<int>(id))) continue;
        std::vector<int> entries;
        gather_goods_entry_speffects(row, entries);
        std::vector<int> hits;
        for (int e : entries) {
            std::vector<int> timed; std::unordered_set<int> v;
            collect_timed_chain(e, timed, v);
            for (int t : timed) {
                auto* r = sp_row(t);
                if (!protectedSp.count(t) && is_self_buff(r) &&
                    r && r->effectEndurance >= kMissDurFloor)
                    hits.push_back(t);
            }
        }
        if (hits.empty()) continue;
        std::string s;
        for (int t : hits) {
            auto* r = sp_row(t);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%d(dur=%.1f) ", t,
                          r ? r->effectEndurance : 0.0f);
            s += buf;
        }
        flog("miss? goods=%d type=%d sortGrp=%d -> %s",
             static_cast<int>(id), static_cast<int>(row.goodsType),
             static_cast<int>(row.sortGroupId), s.c_str());
        ++mn;
    }
    flog("discover: %d potential-miss goods (review for coverage)", mn);

    // --- spell buffs ----------------------------------------
    // Follows the same resolution the apply pass uses: refId as SpEffect + (for
    // AoE self+ally buffs) refId as Bullet + behavior, then the replace/cycle
    // chain. Entries tagged FOE/DBF would be dropped (offensive payloads/debuffs).
    flog("discover: ---- Magic -> SpEffect (spell-buff candidates, chain-followed) ----");
    int sn = 0;
    for (auto [id, row] : from::param::Magic) {
        std::vector<int> entries;
        gather_magic_entries(row, entries);
        std::string s = resolved(entries, true);
        if (s == "-") continue;
        flog("magic=%d -> %s", static_cast<int>(id), s.c_str());
        ++sn;
    }
    flog("discover: %d magic rows with timed buffs", sn);

    // --- ash-of-war allowlist check -------------------------
    // The Ash-of-War category is id-driven (built-in + [ashes_of_war]
    // speffect_ids), because the activated buff can't be reached from the gem
    // param. Verify each allowlisted id against THIS regulation: present + timed
    // means it'll be extended; missing means a different id (e.g. on a mod) --
    // look it up in soulsmods/Paramdex SpEffectParam names and add it.
    flog("discover: ---- ASH-OF-WAR ALLOWLIST CHECK (%zu id(s)) ----", ashIds.size());
    int aok = 0;
    for (int id : ashIds) {
        auto* r = sp_row(id);
        const bool willExtend = r && r->effectEndurance > 0.0f &&
                                !protectedSp.count(id) && !is_system_effect(id);
        const char* status =
            !r                          ? "MISSING (not in SpEffectParam)" :
            protectedSp.count(id)       ? "skipped (protected)" :
            is_system_effect(id)        ? "skipped (system effect)" :
            (r->effectEndurance > 0.0f) ? "OK (timed -> will extend)" :
                                          "skipped (not on a timer)";
        flog("ash id=%d dur=%.1f %s", id, r ? r->effectEndurance : 0.0f, status);
        if (willExtend) ++aok;
    }
    flog("discover: %d/%zu ash allowlist id(s) will be extended", aok, ashIds.size());

    // --- dual-wield off-hand mirror -------------------------
    // What [dual_wield] mirror_to_offhand=1 would wire: each grease/weapon-art
    // Right -> its Left (off-hand) enchant. "curCyc" is the right row's current
    // cycleOccurrenceSpEffectId -- if it's not -1, a vanilla periodic effect
    // would be overwritten (review before enabling).
    flog("discover: ---- DUAL-WIELD PAIRS (right -> left offhand mirror) ----");
    std::unordered_map<int, int> dwMirror;
    build_dualwield_mirror(artPairs, protectedSp, dwMirror, /*logDetail*/ true);
    flog("discover: %zu dual-wield mirror pair(s) would be wired", dwMirror.size());

    flog("discover: review the sections above, then set dump=0");
}

} // namespace iwb
