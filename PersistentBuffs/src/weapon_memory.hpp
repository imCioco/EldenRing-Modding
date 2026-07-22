#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pb {

// ---- per-weapon buff memory --------------------------------------------
// Greases / blade buffs bind to the active weapon (in practice the right hand),
// so the engine drops them on a weapon swap AND on a "loadout change" such as
// bringing a left-hand weapon into play (dual wield) or two-handing -- even when
// the right weapon itself doesn't change. We restore them per weapon: see
// CLAUDE.md "Per-weapon buff memory" for the full ownership/confirm/restore
// state machine. Corresponds to [weapon_memory] remember_per_weapon in the .ini.

void weapon_memory_reset(); // call across transitions
// suppress_forget: pass true while the player is DEAD (hp == 0 / death freeze)
// -- the killing-blow strip must not be read as natural expiry, or every death
// would wipe the per-weapon bindings.
void weapon_memory_tick(uintptr_t player, const std::vector<int>& current,
                        bool suppress_forget = false);

// ---- cross-session persistence support ---------------------------------
// Read-only view of confirmed weapon-bound ownership (buff id -> set of weapon
// item ids) so session_store can persist it to the state file.
const std::unordered_map<int, std::unordered_set<int>>& weapon_memory_owners();

// Restore one (buff id, weapon id) ownership pair from a loaded session, so a
// swap back to that weapon after a game restart re-applies the buff.
void weapon_memory_seed_owner(int id, int weapon_id);

// Wipe all ownership tracking (confirmed + tentative). Called on a real
// character switch, mirroring timing_clear(), so a previous character's
// weapon bindings can't leak into the new one.
void weapon_memory_clear_owners();

} // namespace pb
