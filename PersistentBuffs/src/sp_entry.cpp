#include "sp_entry.hpp"

#include "config.hpp"
#include "game_access.hpp"
#include "offsets.hpp"
#include "scan.hpp"
#include "utils.hpp"

#include <cmath>
#include <cstring>
#include <unordered_map>

namespace pb {

namespace {

std::unordered_map<int, EntrySnapshot> g_snap; // poll-thread only

// The raw block we read/write in one go: +0x40 .. +0x78 of an entry.
struct EntryBlock {
    float    removal_timer; // +0x40
    float    unk44;         // +0x44
    float    duration;      // +0x48
    float    interval;      // +0x4C
    uint32_t payload[10];   // +0x50..+0x78
};
static_assert(sizeof(EntryBlock) == 0x38, "entry block layout");

bool read_block(uintptr_t entry, EntryBlock& out) {
    return mem::safe_read(entry + kSpEffectTimerOffset, out);
}

// A dword we're willing to restore must decode to a "plain data" float on both
// sides: exactly zero, or a finite normal value in a sane range. Pointer
// halves, handles and packed ints decode to NaN/denormals/huge values and are
// rejected -- so a layout drift can corrupt nothing.
bool plausible_float(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    if (bits == 0) return true;
    if (!std::isfinite(f)) return false;
    const float a = std::fabs(f);
    return a >= 1e-6f && a <= 1e9f;
}

} // namespace

uintptr_t sp_entry_find(uintptr_t player, int id) {
    if (!player) return 0;
    const uintptr_t manager = mem::deref(player + kSpEffectManagerOffset);
    if (!manager) return 0;
    uintptr_t slot = mem::deref(manager + kSpEffectFirstSlotOffset);
    int guard = 0;
    while (slot && guard++ < 512) {
        int cur = -1;
        if (mem::safe_read(slot + kSpEffectIdOffset, cur) && cur == id)
            return slot;
        slot = mem::deref(slot + kSpEffectNextOffset);
    }
    return 0;
}

void sp_entry_capture(uintptr_t player, const std::vector<int>& ids) {
    if (!player) return;
    // One list walk for all ids (not one walk per id).
    const uintptr_t manager = mem::deref(player + kSpEffectManagerOffset);
    if (!manager) return;
    uintptr_t slot = mem::deref(manager + kSpEffectFirstSlotOffset);
    int guard = 0;
    while (slot && guard++ < 512) {
        int cur = -1;
        if (mem::safe_read(slot + kSpEffectIdOffset, cur) && cur > 0) {
            for (int id : ids) {
                if (id != cur) continue;
                EntryBlock b{};
                if (read_block(slot, b)) {
                    EntrySnapshot& s = g_snap[id];
                    s.removal_timer = b.removal_timer;
                    s.unk44         = b.unk44;
                    s.duration      = b.duration;
                    s.interval      = b.interval;
                    std::memcpy(s.payload, b.payload, sizeof(s.payload));
                    s.valid = true;
                }
                break;
            }
        }
        slot = mem::deref(slot + kSpEffectNextOffset);
    }
}

EntrySnapshot sp_entry_last(int id) {
    const auto it = g_snap.find(id);
    return it == g_snap.end() ? EntrySnapshot{} : it->second;
}

double sp_entry_last_remaining(int id) {
    const auto it = g_snap.find(id);
    if (it == g_snap.end() || !it->second.valid) return -1.0;
    if (it->second.duration <= 0.0f) return -1.0; // engine says infinite
    return it->second.removal_timer >= 0.0f
               ? static_cast<double>(it->second.removal_timer)
               : -1.0;
}

bool sp_entry_restore(uintptr_t player, int id, double remaining_s,
                      bool restore_payload) {
    const uintptr_t entry = sp_entry_find(player, id);
    if (!entry) return false;

    EntryBlock now{};
    if (!read_block(entry, now)) return false;

    const auto it = g_snap.find(id);
    const EntrySnapshot* snap =
        (it != g_snap.end() && it->second.valid) ? &it->second : nullptr;

    // Sanity gate before ANY write: the fresh entry must look like an entry
    // (its duration field is a plausible float / -1). Guards layout drift on
    // a future game patch -- if this trips, we write nothing.
    if (!plausible_float(*reinterpret_cast<uint32_t*>(&now.duration)) &&
        now.duration != -1.0f) {
        flog("[WARN] entry restore: %s entry layout unexpected -- skipping "
             "(duration dword %08X)", named(id).c_str(),
             *reinterpret_cast<uint32_t*>(&now.duration));
        return false;
    }

    bool wrote_timer = false;
    if (remaining_s >= 0.0 && now.duration > 0.0f) {
        // Resume mid-countdown: the engine ticks removal_timer down itself.
        const float rem = static_cast<float>(
            remaining_s > kExpiryMarginS ? remaining_s : kExpiryMarginS);
        mem::safe_write(entry + kSpEffectTimerOffset, rem);
        wrote_timer = true;
    }

    // Give back the cast-time state (catalyst-scaled magnitude etc.) that the
    // bare re-apply reset to param defaults.
    if (restore_payload && snap) {
        std::string restored;
        for (int i = 0; i < 10; ++i) {
            const uint32_t oldv = snap->payload[i];
            const uint32_t newv = now.payload[i];
            if (oldv == newv) continue;
            if (!plausible_float(oldv) || !plausible_float(newv)) continue;
            mem::safe_write(entry + kSpEffectPayloadOffset +
                                static_cast<uintptr_t>(i) * 4, oldv);
            if (g_log_effects) {
                float of, nf;
                std::memcpy(&of, &oldv, 4);
                std::memcpy(&nf, &newv, 4);
                char buf[64];
                std::snprintf(buf, sizeof(buf), "+0x%02X %.4g->%.4g ",
                              static_cast<unsigned>(kSpEffectPayloadOffset + i * 4),
                              nf, of);
                restored += buf;
            }
        }
        if (!restored.empty())
            flog("entry restore: %s payload restored [ %s]",
                 named(id).c_str(), restored.c_str());
    }
    return wrote_timer;
}

void sp_entry_seed(int id, const uint32_t payload[10]) {
    if (id <= 0 || !payload) return;
    EntrySnapshot& s = g_snap[id]; // fresh: timers stay at infinite defaults
    std::memcpy(s.payload, payload, sizeof(s.payload));
    s.valid = true;
}

bool sp_entry_payload_snapshot(int id, uint32_t out[10]) {
    const auto it = g_snap.find(id);
    if (it == g_snap.end() || !it->second.valid) return false;
    for (int i = 0; i < 10; ++i) {
        const uint32_t v = it->second.payload[i];
        // A non-restorable slot is stored as the skip sentinel (a quiet NaN):
        // plausible_float rejects it on restore, so no run-specific pointer
        // leaks to disk and the slot is left untouched.
        out[i] = plausible_float(v) ? v : kPayloadSkip;
    }
    return true;
}

void sp_entry_clear()      { g_snap.clear(); }
void sp_entry_forget(int id) { g_snap.erase(id); }

} // namespace pb
