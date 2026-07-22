#pragma once

#include <string>
#include <vector>

namespace pb {

// ---- cross-session buff persistence ------------------------------------
// Save the character's active persistable buffs (with their REMAINING seconds)
// to PersistentBuffs.state.ini next to the DLL, and restore them on the next
// game launch for the same character. Time does NOT pass while the game is
// closed -- we store remaining seconds, not absolute expiry timestamps -- and
// infinite buffs (effectEndurance == -1) stay infinite. Gated by
// [session] remember_across_sessions (g_session_persist). See
// docs/SESSION_PERSISTENCE_DESIGN.md for the full design + verification steps.

// Derive a stable, ini/filesystem-safe key from a character name: sanitized
// UTF-8 (<=24 chars, [A-Za-z0-9_-], others -> '_') + '_' + 4 hex digits of
// FNV-1a over the raw UTF-16 bytes (so names that sanitize identically still get
// distinct keys). Byte-identical names still collide -- documented limitation.
std::string session_char_key(const std::wstring& name);

// Resolve the current character's key. On a failed name read: KEEP `prev_key` if
// we have one (a transient bad read must never look like a character switch and
// wipe state); "default" only if no name was ever read. `*name_ok`, if given,
// reports whether a real name was read this call.
std::string session_current_key(const std::string& prev_key, bool* name_ok);

// Parse PersistentBuffs.state.ini into the in-memory mirror. Call once at
// startup (after the GameDataMan block). A missing file or a version mismatch
// starts fresh (the old file is ignored, then overwritten on the next save).
void session_startup_load();

// Persist `remembered` under character `key`, atomically rewriting the whole
// state file (tmp + MoveFileEx). Expired ids are skipped; remaining seconds come
// from the timing module (infinite -> -1). Capped at kSessionMaxBuffs. When
// [weapon_memory] is on, the weapon-memory bindings (buff id -> owning weapon,
// from weapon_memory_owners()) are saved too, as `weapon_buffs` id:weapon:rem
// triples -- so a restart can't erase buffs parked on a stowed weapon. Writes
// happen when EITHER remember_across_sessions or remember_per_weapon is on.
// Each persisted id also carries its cast-time entry payload (`buff_payloads`
// id:h0,..,h9) so a scaling weapon buff restores its catalyst-scaled magnitude
// after a restart, not the param base.
void session_save(const std::string& key, const std::vector<int>& remembered);

// Restore character `key`'s saved buffs into `remembered` (replacing it) and
// seed the timing module from their stored remaining times. Always clears the
// timing records + weapon-memory bindings + `remembered` first, so a character
// with no saved entry ends up with an empty set. When [weapon_memory] is on,
// also re-seeds the weapon bindings (weapon_memory_seed_owner) so swapping to
// the owning weapon re-applies its parked buffs. Also re-seeds each saved id's
// cast-time payload into g_snap (sp_entry_seed) and clears g_snap first, so the
// re-apply restores scaled magnitude (not param base) across a restart. Returns
// true iff a saved entry existed for `key`. Only called when
// remember_across_sessions is on.
bool session_restore(const std::string& key, std::vector<int>& remembered);

} // namespace pb
