#pragma once

#include <cstdint>
#include <vector>

namespace pb {

// ---- buff timing memory (wall-clock) ------------------------------------
// Tracks how long each persistable buff has been active so the mod knows its
// remaining duration at all times. A buff's clock advances ONLY while its id
// is present on the player and the player is valid -- while a buff is absent
// (death pre-strip, loading screen) its remaining time is preserved, which is
// exactly what the death-freeze restore wants. Total duration is read from
// SpEffectParam.effectEndurance AT APPLICATION TIME, so it picks up
// InfiniteWeaponBuffs' startup param patches. No row / endurance <= 0 =>
// untracked (treated as permanent: never expire-vetoed -- preserves the old
// behavior for force/always-persist ids without usable rows). See CLAUDE.md
// "Buff timing & expiry".

// Feed the tracker once per poll iteration. `persistable_now` = this tick's
// is_persistable-filtered active ids; pass player_valid=false (empty vector)
// during loading screens so the clocks freeze.
void timing_tick(bool player_valid, const std::vector<int>& persistable_now);

// Seconds left for `id`; +infinity if untracked or permanent.
double timing_remaining(int id);

// True iff `id` is tracked, finite, at/past its expiry margin AND absent from
// the live list. A still-present buff is NEVER expired -- the engine is the
// authority while it's alive (see the refresh self-heal in timing_tick).
bool timing_is_expired(int id);

// Remove expired ids from `remembered` (one log line each) so a stale
// death-freeze snapshot can't resurrect them. Timing records are kept: a
// weapon-memory entry for the same id must still see it as expired.
void timing_prune_expired(std::vector<int>& remembered);

// The one way to re-apply a persisted buff: vetoes expired ids (returns
// false), otherwise applies via g_apply with the buff's REMAINING time --
// written straight into the engine countdown when the entry layout is
// verified, else via the SpEffectParam patch-around -- and restores the
// cast-time payload (buff power), then registers the id so its reappearance
// keeps its clock. Remaining-time + power restore are core behavior (the old
// .ini toggles are gone). Returns true if applied.
bool apply_persisted(uintptr_t player, int id);

// MinHook ApplySpEffect so a recast of a STILL-ACTIVE buff resets its clock
// to the full duration (presence-based tracking can't see it -- the id never
// disappears; the "reapplied grease at 300s left stayed at 300s" bug). Call
// once from run() after g_apply is resolved; a failed hook just logs and
// falls back to presence-based detection. Our own re-applies go through the
// trampoline, so they never read as player recasts.
void timing_install_hook();

// ---- cross-session persistence support ---------------------------------
// Wipe all timing records + presence/reappear bookkeeping. Called on a
// character switch and before seeding a restored session so a previous
// character's clocks can't leak into the new one.
void timing_clear();

// Seed a record for `id` from a stored REMAINING time (cross-session restore).
// total_s = the row's effectEndurance read NOW; elapsed = clamp(total - remaining,
// 0, total). A row with endurance <= 0 (e.g. InfiniteWeaponBuffs installed) =>
// untracked/infinite regardless of the stored value; a stored remaining < 0
// (infinite) but a now-finite row => fresh full duration (elapsed 0).
void timing_seed(int id, double remaining_s);

} // namespace pb
