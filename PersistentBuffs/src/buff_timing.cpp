#include "buff_timing.hpp"
#include "buff_filters.hpp"
#include "config.hpp"
#include "game_access.hpp"
#include "offsets.hpp"
#include "sp_entry.hpp"
#include "utils.hpp"

#include <MinHook.h>

#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace pb {

namespace {

struct TimingRec {
    float  total_s;           // effectEndurance at application; <= 0 => untracked
    double elapsed_s;         // advances only while present & player valid
    int    past_expiry_ticks; // refresh self-heal counter (see timing_tick)
};

// Guards all timing state below: the ApplySpEffect detour runs on the GAME's
// thread(s) concurrently with the poll thread (see timing_install_hook).
std::mutex g_timing_mutex;

std::unordered_map<int, TimingRec> g_timing;
std::unordered_set<int>            g_present_prev;    // persistable ids last tick
std::unordered_map<int, int>       g_expect_reappear; // our re-applies, TTL ticks
bool g_prev_valid = false;
std::chrono::steady_clock::time_point g_prev_tick;

// Set once a live entry's duration field matches its row's effectEndurance --
// i.e. the SpecialEffectEntry layout (offsets.hpp +0x40/+0x48) is right for
// this game build. While true, the engine's own removal_timer is the clock
// authority and re-applies write it directly; while false everything degrades
// to the wall-clock + effectEndurance patch-around this mod shipped with.
bool g_entry_layout_ok = false;

TimingRec fresh_record(int id) {
    const auto* r = sp_row(id);
    const float total = (r && r->effectEndurance > 0.0f) ? r->effectEndurance : -1.0f;
    return TimingRec{ total, 0.0, 0 };
}

// Original ApplySpEffect once hooked. Our own re-applies (apply_persisted) call
// THIS, bypassing the detour -- both so we don't misread our own patched-
// duration applies as player recasts, and because apply_persisted holds
// g_timing_mutex across the call (going through the hooked entry would
// re-enter the detour on the same thread and deadlock).
ApplySpEffect_t g_apply_trampoline = nullptr;

// Runs on the game's thread for every SpEffect the engine applies to any
// character. A persistable id landing on the player is a (re)application:
// restart its clock at the row's full duration whether or not the id was
// already active -- the presence-based tracking in timing_tick can't see a
// mid-duration recast (the id never disappears), which is why re-greasing at
// 300s left didn't go back to the full duration. Engine-initiated applies
// (cycleOccurrenceSpEffectId top-ups etc.) also land here; resetting those
// clocks mirrors what the engine itself does and fails safe (over-estimates
// remaining, never wrongly vetoes). See CLAUDE.md "Buff timing & expiry".
void detour_apply_speffect(void* chr_ins, int sp_effect_id, char unk) {
    const uintptr_t player = get_player_ins();
    if (player && chr_ins == reinterpret_cast<void*>(player) &&
        is_persistable(sp_effect_id)) {
        {
            std::lock_guard<std::mutex> lock(g_timing_mutex);
            g_timing[sp_effect_id] = fresh_record(sp_effect_id);
            g_expect_reappear.erase(sp_effect_id); // a real apply supersedes our mark
        }
        if (g_log_effects)
            flog("timing: %s (re)applied by the game -> timer reset to full",
                 named(sp_effect_id).c_str());
    }
    g_apply_trampoline(chr_ins, sp_effect_id, unk);
}

// Lock-free internals (callers hold g_timing_mutex) -- the public
// timing_remaining/timing_is_expired wrappers lock, and functions that need
// both under ONE critical section (prune, apply_persisted) call these to
// avoid recursive locking.
double remaining_unlocked(int id) {
    const auto it = g_timing.find(id);
    if (it == g_timing.end() || it->second.total_s <= 0.0f)
        return std::numeric_limits<double>::infinity();
    return static_cast<double>(it->second.total_s) - it->second.elapsed_s;
}

bool is_expired_unlocked(int id) {
    const auto it = g_timing.find(id);
    if (it == g_timing.end() || it->second.total_s <= 0.0f) return false;
    if (g_present_prev.count(id)) return false; // live => the engine decides
    return static_cast<double>(it->second.total_s) - it->second.elapsed_s
           <= kExpiryMarginS;
}

} // namespace

void timing_tick(bool player_valid, const std::vector<int>& persistable_now) {
    std::lock_guard<std::mutex> lock(g_timing_mutex);
    const auto now = std::chrono::steady_clock::now();
    // dt counts only between two consecutive VALID ticks (never across a
    // loading screen), capped so a debugger break / process suspend can't eat
    // a buff's whole remaining time in one tick.
    double dt = 0.0;
    if (g_prev_valid && player_valid) {
        dt = std::chrono::duration<double>(now - g_prev_tick).count();
        if (dt < 0.0)         dt = 0.0;
        if (dt > kMaxTickDtS) dt = kMaxTickDtS;
    }
    g_prev_tick  = now;
    g_prev_valid = player_valid;

    if (!player_valid) {
        // Loading screen / menu: clocks freeze. Clearing the presence set
        // makes every post-transition id a (re)appearance, classified below
        // as ours (g_expect_reappear) or a fresh application.
        g_present_prev.clear();
        return;
    }

    for (int id : persistable_now) {
        const bool was_present = g_present_prev.count(id) != 0;
        auto it = g_timing.find(id);

        if (!was_present) {
            // (Re)appeared this tick. Ours (marked by apply_persisted) keeps
            // its clock -- the engine was given the remaining, not the full,
            // duration. Unmarked => the player (re)applied it: restart with
            // the row's duration as of NOW (sees param patches).
            const auto ex = g_expect_reappear.find(id);
            if (ex != g_expect_reappear.end() && it != g_timing.end()) {
                g_expect_reappear.erase(ex);
                it->second.past_expiry_ticks = 0;
            } else {
                g_timing[id] = fresh_record(id);
            }
            continue;
        }

        if (it == g_timing.end()) {
            // Present but untracked: the DLL was injected mid-session. A
            // full-duration record over-estimates remaining -- benign.
            g_timing[id] = fresh_record(id);
            continue;
        }

        TimingRec& rec = it->second;
        if (rec.total_s <= 0.0f) continue; // untracked/permanent

        rec.elapsed_s += dt;

        // Engine sync: while the buff is LIVE, its entry carries the engine's
        // own countdown (removal_timer, captured this tick by sp_entry_capture).
        // When the entry's total matches our row-based total, that countdown is
        // the truth -- recasts, pauses, engine top-ups all reflected -- so it
        // overrides the wall-clock. A mismatch (layout drift on a new game
        // build, or our own mid-duration restore) falls through to the
        // wall-clock + self-heal below, i.e. the pre-entry behavior.
        const EntrySnapshot snap = sp_entry_last(id);
        if (snap.valid && snap.duration > 0.0f && snap.removal_timer >= 0.0f &&
            std::fabs(snap.duration - rec.total_s) <=
                0.5f + rec.total_s * 0.01f) {
            if (!g_entry_layout_ok) {
                g_entry_layout_ok = true;
                flog("buff timing: SpEffect entry layout verified -- engine "
                     "countdown is now the clock authority");
            }
            double el = static_cast<double>(rec.total_s) -
                        static_cast<double>(snap.removal_timer);
            rec.elapsed_s = el < 0.0 ? 0.0 : el;
            rec.past_expiry_ticks = 0;
            continue;
        }

        // Refresh self-heal (fallback path): the engine still shows the buff
        // well past our computed expiry => the player refreshed it mid-duration
        // (a recast resets the engine's timer without the id ever disappearing,
        // which presence-based tracking can't observe). Trust the engine and
        // restart our clock. Fails safe: worst case we over-estimate
        // remaining; we never wrongly veto a live buff.
        if (rec.total_s - rec.elapsed_s <= kExpiryMarginS) {
            if (++rec.past_expiry_ticks >= kRefreshGraceTicks) {
                flog("timing: %s still active past expected expiry -> "
                     "assuming refresh, timer restarted", named(id).c_str());
                rec.elapsed_s = 0.0;
                rec.past_expiry_ticks = 0;
            }
        } else {
            rec.past_expiry_ticks = 0;
        }
    }

    // Age out reappear marks for re-applies the engine swallowed.
    for (auto it = g_expect_reappear.begin(); it != g_expect_reappear.end();) {
        if (--it->second <= 0) it = g_expect_reappear.erase(it);
        else                   ++it;
    }

    g_present_prev.clear();
    g_present_prev.insert(persistable_now.begin(), persistable_now.end());
}

double timing_remaining(int id) {
    std::lock_guard<std::mutex> lock(g_timing_mutex);
    return remaining_unlocked(id);
}

bool timing_is_expired(int id) {
    std::lock_guard<std::mutex> lock(g_timing_mutex);
    return is_expired_unlocked(id);
}

void timing_prune_expired(std::vector<int>& remembered) {
    std::lock_guard<std::mutex> lock(g_timing_mutex);
    for (size_t i = 0; i < remembered.size();) {
        const int id = remembered[i];
        if (is_expired_unlocked(id)) {
            flog("timing: pruned expired buff %s (remaining %.1fs)",
                 named(id).c_str(), remaining_unlocked(id));
            remembered.erase(remembered.begin() + i);
        } else {
            ++i;
        }
    }
}

bool apply_persisted(uintptr_t player, int id) {
    if (!g_apply) return false;
    // Once hooked, our own applies go through the trampoline so the detour
    // never sees them (and can't deadlock on the mutex we hold below).
    const ApplySpEffect_t apply_fn = g_apply_trampoline ? g_apply_trampoline
                                                        : g_apply;
    double rem;
    {
        std::lock_guard<std::mutex> lock(g_timing_mutex);
        if (is_expired_unlocked(id)) return false; // the veto
        rem = remaining_unlocked(id);
    }
    // The engine call itself runs OUTSIDE the mutex: if ApplySpEffect ever
    // applies chained effects synchronously, those recurse through the hooked
    // entry into the detour, which takes the mutex -- holding it here would
    // self-deadlock.
    // Restoring the REMAINING time (not a fresh full duration) is core
    // behavior -- no longer an .ini option.
    auto* r = sp_row(id);
    const bool want_remaining =
        r && r->effectEndurance > 0.0f && std::isfinite(rem);
    if (want_remaining && !g_entry_layout_ok) {
        // Patch-around (fallback when the entry layout isn't verified for this
        // game build): the engine copies effectEndurance into the live effect
        // at application, so briefly writing the remaining time restores the
        // buff mid-countdown. Aligned 4-byte float write -- atomic on x64; the
        // microseconds-wide race vs another application of the same row is the
        // same accepted-risk class as the off-thread apply call (see CLAUDE.md).
        const float orig = r->effectEndurance;
        r->effectEndurance = static_cast<float>(
            rem > kExpiryMarginS ? rem : kExpiryMarginS);
        apply_fn(reinterpret_cast<void*>(player), id, 1); // unk=1 == "self"
        r->effectEndurance = orig;
    } else {
        apply_fn(reinterpret_cast<void*>(player), id, 1); // unk=1 == "self"
    }
    // Fix up the freshly-created entry: write the engine countdown directly
    // (verified layout only) and restore the cast-time payload -- the
    // catalyst-scaled magnitude a bare re-apply resets to param defaults (the
    // "+75 instead of +200" holster report). Both are core behavior now (the
    // old restore_buff_power option is gone). Runs right after the synchronous
    // apply, on this thread, outside the mutex.
    sp_entry_restore(player, id,
                     (want_remaining && g_entry_layout_ok) ? rem : -1.0,
                     /*restore_payload=*/true);
    {
        std::lock_guard<std::mutex> lock(g_timing_mutex);
        g_expect_reappear[id] = kExpectReappearTicks;
    }
    return true;
}

void timing_install_hook() {
    if (!g_apply) return; // apply fn not resolved -> nothing to hook
    if (MH_CreateHook(reinterpret_cast<LPVOID>(g_apply),
                      reinterpret_cast<LPVOID>(&detour_apply_speffect),
                      reinterpret_cast<LPVOID*>(&g_apply_trampoline)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<LPVOID>(g_apply)) == MH_OK) {
        flog("buff timing: reapplication hook installed on ApplySpEffect");
    } else {
        g_apply_trampoline = nullptr; // apply_persisted falls back to g_apply
        flog("[WARN] buff timing: ApplySpEffect hook failed -- a mid-duration "
             "recast won't reset its timer (presence-based detection only)");
    }
}

void timing_clear() {
    std::lock_guard<std::mutex> lock(g_timing_mutex);
    g_timing.clear();
    g_present_prev.clear();
    g_expect_reappear.clear();
}

void timing_seed(int id, double remaining_s) {
    std::lock_guard<std::mutex> lock(g_timing_mutex);
    const auto* r = sp_row(id);
    const float total = (r && r->effectEndurance > 0.0f) ? r->effectEndurance : -1.0f;
    if (total <= 0.0f) {
        // Row is untracked/infinite now (e.g. IWB patched endurance to -1) ->
        // permanent, regardless of the stored remaining.
        g_timing[id] = TimingRec{ -1.0f, 0.0, 0 };
        return;
    }
    // Row is finite now. A stored infinite (remaining < 0, e.g. IWB was removed
    // since the save) restarts at full duration.
    double elapsed = (remaining_s < 0.0) ? 0.0
                                         : static_cast<double>(total) - remaining_s;
    if (elapsed < 0.0)              elapsed = 0.0;
    if (elapsed > total)           elapsed = total;
    g_timing[id] = TimingRec{ total, elapsed, 0 };
}

} // namespace pb
