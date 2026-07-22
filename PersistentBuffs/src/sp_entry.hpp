#pragma once

#include <cstdint>
#include <vector>

namespace pb {

// ---- live SpEffect entry access (sp_entry.cpp) ---------------------------
// The engine keeps one SpecialEffectEntry node per active effect (the same
// linked list enumerate_speffects walks). Beyond the id (+0x8) and next ptr
// (+0x30) the entry carries the ENGINE'S OWN per-effect state:
//   +0x40 removal_timer  (f32)  -- seconds until the effect removes itself;
//                                  THE authoritative countdown (pauses, recasts,
//                                  IWB patches: everything is reflected here)
//   +0x44 secondary timer (f32)
//   +0x48 duration        (f32) -- total duration copied from effectEndurance
//                                  at application (-1/0 = infinite)
//   +0x4C interval_timer  (f32)
//   +0x50..+0x78 payload  (10 dwords, layout unknown) -- among other things
//                 this is where the CAST-TIME MAGNITUDE of scaling buffs lives
//                 (weapon buffs cast from a seal/staff store their catalyst-
//                 scaled power in the instance, NOT in SpEffectParam -- that's
//                 why re-applying the bare id gives the grease-level base
//                 values: the "+75 instead of +200" holster bug).
// Layout source: vswarte/fromsoftware-rs cs::SpecialEffectEntry (id/next
// offsets match the TGA CT values this mod already uses).
//
// This module snapshots the timer block + payload of every persistable live
// effect each poll tick, and restores it onto the freshly-created entry right
// after one of OUR re-applies -- giving back both the remaining time and the
// catalyst-scaled magnitude. All calls happen on the poll thread.

// Canonical "do not restore this slot" marker: a quiet NaN. It reads as
// non-finite, so the plausible-float gate in sp_entry_restore skips it -- used
// both for payload dwords that aren't restorable and (by session_store) for the
// slots omitted from the compact on-disk form.
constexpr uint32_t kPayloadSkip = 0x7FC00000u;

struct EntrySnapshot {
    float    removal_timer = -1.f; // engine countdown at capture time
    float    unk44         = 0.f;
    float    duration      = -1.f; // engine total (-1/0 = infinite)
    float    interval      = 0.f;
    uint32_t payload[10]   = {};   // +0x50..+0x78 raw dwords
    bool     valid         = false;
};

// Address of the live entry for `id` on the player's effect list (0 if absent).
uintptr_t sp_entry_find(uintptr_t player, int id);

// Snapshot the timer block + payload of every id in `ids` that is live on the
// player. Called once per poll tick with the persistable set; absent ids keep
// their last snapshot (frozen state through death strip / stow / loading).
void sp_entry_capture(uintptr_t player, const std::vector<int>& ids);

// Last captured snapshot for `id` (valid == false if never seen live).
EntrySnapshot sp_entry_last(int id);

// Engine remaining seconds for `id` from its LAST capture: >= 0 tracked,
// negative = infinite/unknown (duration <= 0 or never captured).
double sp_entry_last_remaining(int id);

// After OUR re-apply of `id`: locate the fresh entry and restore state from
// the last snapshot.
//  * remaining_s >= 0 -> write it into removal_timer (engine countdown), so
//    the buff resumes mid-duration without the effectEndurance patch-around.
//  * restore_payload -> copy back payload dwords that changed, gated to values
//    that read as plausible floats on BOTH sides (magnitude, rates, timers);
//    pointer/handle-looking dwords are never touched. Each restored dword is
//    logged with its offset so in-game logs pin the real magnitude field.
// Returns true if the entry was found and the removal_timer write happened.
bool sp_entry_restore(uintptr_t player, int id, double remaining_s,
                      bool restore_payload);

// Seed a payload snapshot for `id` from stored cast-time dwords (cross-session
// restore): marks it valid so a following apply_persisted -> sp_entry_restore
// gives back the catalyst-scaled magnitude, exactly like an in-session weapon
// swap does -- now across a game restart. Only the payload is seeded; the timers
// stay at their infinite defaults (the timing module owns remaining, and
// timing_tick's engine-sync branch needs duration > 0, so it ignores the seed
// until a real capture replaces it).
void sp_entry_seed(int id, const uint32_t payload[10]);

// Fill `out` with id's last-captured payload for persistence to disk, replacing
// any dword that is NOT a restorable plausible float with a non-plausible
// sentinel -- so no stale pointer/handle is written to the state file, and that
// slot is skipped on restore (same gate sp_entry_restore applies). Returns false
// if `id` was never captured live (nothing worth saving).
bool sp_entry_payload_snapshot(int id, uint32_t out[10]);

// Wipe all snapshots (character switch -- never leak entry state across).
void sp_entry_clear();

// Drop one id's snapshot (its buff expired for good).
void sp_entry_forget(int id);

} // namespace pb
