#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "scan.hpp"

namespace pb {

// ---- game access -------------------------------------------------------
extern mem::Module g_mod;

// Resolve the local player ChrIns via WorldChrMan. Returns 0 if not in-game.
uintptr_t get_player_ins();

// Current HP of the player ChrIns (modules -> data module -> hp), or -1 if
// unreadable/implausible. hp == 0 while the ChrIns is valid == the player is
// DEAD -- the authoritative death signal for the freeze (the buff-drop
// heuristic stays as fallback when this returns -1).
int get_player_hp(uintptr_t player);

// Resolve PlayerGameData via GameDataMan. Returns 0 if unavailable.
uintptr_t get_player_game_data();

// Read the local character's name (PlayerGameData + kCharNameOffset, UTF-16,
// <=16 chars). Empty string on failure / implausible data (e.g. a control char,
// which flags a wrong offset). ⚠ UNVERIFIED offset -- see offsets.hpp.
std::wstring get_character_name();

// Item id of the weapon in the currently-active hand slot, or -1 if none.
// `slot_off` selects the active slot index field; `prim_off` is the slot-0 id.
int get_active_weapon_id(uintptr_t slot_off, uintptr_t prim_off);
int get_active_right_weapon_id();
int get_active_left_weapon_id();
int get_arm_style();

// Walk the player's active SpEffect linked list, collecting effect ids.
//   player + 0x178      -> SpEffect manager
//   manager + 0x8       -> first slot (the list head is one indirection below)
//   slot + 0x8 (int)    -> effect id
//   slot + 0x30 (ptr)   -> next slot
void enumerate_speffects(uintptr_t player, std::vector<int>& out);

} // namespace pb
