#include "weapon_memory.hpp"
#include "config.hpp"
#include "buff_discovery.hpp"
#include "buff_filters.hpp"
#include "buff_timing.hpp"
#include "game_access.hpp"
#include "offsets.hpp"
#include "utils.hpp"

#include <vector>

#include <unordered_map>
#include <unordered_set>

namespace pb {

// "Loadout" = (active right wep id, active left wep id, ArmStyle).
//  * Ownership is learned at APPLICATION: when a buff first appears, tag it with
//    the active weapon (right hand preferred, else left -- matching how greases /
//    blade spells target the right armament).
//  * A buff is treated as weapon-bound only once it's observed to DROP within a
//    short window after a loadout change. Body buffs (Golden Vow, consumables)
//    don't drop on a loadout change, so they're never confirmed -> never touched.
//  * Confirmed buffs are re-applied while their owner weapon is in hand. A buff
//    that vanishes with NO recent loadout change = natural expiry -> forgotten
//    (so we don't resurrect an expired grease, matching the prior right-hand feel).
constexpr int kReapplyWindowTicks = 3;  // ~600ms at the 200ms poll; covers swap lag

std::unordered_map<int,int> g_applied_owner; // buff id -> weapon active when it appeared
// confirmed weapon-bound buff -> the SET of weapons that have it. A set (not a
// single weapon) so the SAME grease applied to two weapons is remembered for
// both -- otherwise switching between them would drop it from one.
std::unordered_map<int, std::unordered_set<int>> g_buff_owner;
std::unordered_set<int>     g_prev_buffs;
std::unordered_set<int>     g_restored_event; // buffs already restored since last change
int  g_prev_right = -1, g_prev_left = -1, g_prev_arm = -2;
bool g_loadout_valid = false;
int  g_ticks_since_change = 1 << 20;

void weapon_memory_reset() { g_loadout_valid = false; }

const std::unordered_map<int, std::unordered_set<int>>& weapon_memory_owners() {
    return g_buff_owner;
}

void weapon_memory_seed_owner(int id, int weapon_id) {
    if (id <= 0 || weapon_id <= 0) return;
    g_buff_owner[id].insert(weapon_id);
}

void weapon_memory_clear_owners() {
    g_buff_owner.clear();
    g_applied_owner.clear();
    g_prev_buffs.clear();
    g_restored_event.clear();
}

void weapon_memory_tick(uintptr_t player, const std::vector<int>& current,
                        bool suppress_forget) {
    if (!g_weapon_memory) return;
    const int right = get_active_right_weapon_id();
    const int left  = get_active_left_weapon_id();
    const int arm   = get_arm_style();
    // Only track real buffs -- never engine state effects (so we don't bind/
    // re-apply Roundtable's no-combat block etc. through weapon swaps).
    std::unordered_set<int> cur;
    for (int id : current) if (is_persistable(id)) cur.insert(id);
    const int owner_now = (right > 0) ? right : left;

    if (!g_loadout_valid) {
        // Seed: tag already-active buffs, but don't confirm any as weapon-bound.
        if (owner_now > 0) for (int id : cur) g_applied_owner[id] = owner_now;
        g_prev_buffs = cur;
        g_prev_right = right; g_prev_left = left; g_prev_arm = arm;
        g_ticks_since_change = 1 << 20;
        g_loadout_valid = true;
        return;
    }

    const bool changed = (right != g_prev_right) || (left != g_prev_left) || (arm != g_prev_arm);
    if (changed) { g_ticks_since_change = 0; g_restored_event.clear(); }
    else if (g_ticks_since_change < (1 << 20)) ++g_ticks_since_change;
    const bool recent = g_ticks_since_change <= kReapplyWindowTicks;

    // 1) Learn tentative ownership for newly-applied buffs. Skip ids WE just
    //    restored (g_restored_event): ownership is learned from the PLAYER
    //    applying a buff, and re-tagging our own restore with whatever weapon
    //    is now in hand made every character-wide restore accumulate a bogus
    //    binding for that weapon (then confirmed on the next swap -- the
    //    "same buff bound to every weapon" growth in the state file).
    if (owner_now > 0)
        for (int id : cur)
            if (!g_prev_buffs.count(id) && !g_restored_event.count(id))
                g_applied_owner[id] = owner_now;

    // 2) Handle buffs that vanished this tick.
    for (int id : g_prev_buffs) {
        if (cur.count(id)) continue;
        if (recent) {
            // Dropped together with a loadout change -> weapon-bound. Add the
            // weapon it was applied on to this buff's owner set (a buff can live
            // on several weapons, e.g. the same grease on two of them).
            auto a = g_applied_owner.find(id);
            if (a != g_applied_owner.end() && a->second > 0) {
                if (g_buff_owner[id].insert(a->second).second)
                    flog("weapon-memory: %s bound to %s",
                         named(id).c_str(), named_weapon(a->second).c_str());
            }
        } else if (!suppress_forget) {
            // Vanished with no recent loadout change -> natural expiry. Drop the
            // in-hand weapon(s) from this buff's owners (their copy ran out);
            // forget the buff entirely once no weapon owns it / if character-wide.
            // suppress_forget (the player is DEAD): the killing-blow strip is
            // neither an expiry nor a loadout change -- keep the bindings so a
            // grease parked on a stowed weapon survives the respawn.
            auto it = g_buff_owner.find(id);
            if (it != g_buff_owner.end()) {
                if (is_character_wide_buff(id)) {
                    g_buff_owner.erase(it);
                } else {
                    it->second.erase(right);
                    it->second.erase(left);
                    if (it->second.empty()) g_buff_owner.erase(it);
                }
            }
            g_applied_owner.erase(id);
        }
    }

    // 3) Within the post-change window, restore dropped confirmed buffs. A normal
    //    weapon buff (grease / blade spell) is restored only while its owner
    //    weapon is back in hand (owner not in hand => we swapped away on purpose
    //    => leave dropped). A CHARACTER-WIDE buff (AoW self-buff -- classified
    //    param-driven via is_character_wide_buff, seeded by g_always_persist) is
    //    restored regardless of which weapon is in hand, so it survives any swap
    //    like a body buff.
    //    De-duped per change event: in a dual-wield state the engine may keep our
    //    re-applied buff under a different id, so it never re-appears in `cur` --
    //    without this guard we'd re-apply (and log) every tick of the window.
    if (recent && g_apply) {
        int n = 0;
        std::vector<int> expired;
        for (const auto& kv : g_buff_owner) {
            const int id = kv.first;
            const auto& weapons = kv.second;          // set of owning weapon IDs
            bool in_hand = (weapons.find(right) != weapons.end()) ||
                        (weapons.find(left)  != weapons.end());
            bool always = is_character_wide_buff(id);
            if ((in_hand || always) && !cur.count(id) && !g_restored_event.count(id)) {
                if (!apply_persisted(player, id)) {
                    // Vetoed: its own timer ran out (e.g. it expired within the
                    // loadout-change window and got mis-confirmed as weapon-
                    // bound). Forget it instead of retrying every window.
                    expired.push_back(id);
                    continue;
                }
                g_restored_event.insert(id);
                ++n;
            }
        }
        for (int id : expired) {
            flog("weapon-memory: forgetting expired buff %s", named(id).c_str());
            g_buff_owner.erase(id);
            g_applied_owner.erase(id);
        }
        if (n) flog("weapon-memory: restored %d weapon buff(s) after loadout change", n);
    }

    g_prev_buffs = cur;
    g_prev_right = right; g_prev_left = left; g_prev_arm = arm;
}

} // namespace pb
