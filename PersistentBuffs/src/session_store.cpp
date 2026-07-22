#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "session_store.hpp"

#include "buff_filters.hpp"
#include "buff_timing.hpp"
#include "config.hpp"
#include "game_access.hpp"
#include "ini.hpp"
#include "sp_entry.hpp"
#include "utils.hpp"
#include "weapon_memory.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace pb {

namespace {

// On-disk format version. Bump if the [char_*] layout changes incompatibly; a
// mismatch makes session_startup_load ignore the file (start fresh).
constexpr int kSessionFormatVersion = 1;

// In-memory mirror of the state file: one entry per [char_<key>] section, keyed
// by the FULL section name. Ordered so the rewritten file is deterministic and
// so OTHER characters' sections survive a save untouched.
struct SessionEntry {
    long long   saved_unix = 0;
    std::string buffs;         // raw "id:rem id:rem ..." (-1 rem == infinite)
    std::string weapon_buffs;  // raw "id:weapon:rem ..." -- weapon-memory bindings
                               // ([weapon_memory] remember_per_weapon), same -1
                               // convention. Absent key in old files -> "".
    std::string payloads;      // compact "id:idx=hex,.. ..." -- only the cast-time
                               // payload slots carrying a scaled magnitude, so a
                               // seal/staff-cast weapon buff restores catalyst-
                               // scaled, not param base. Absent key -> "".
};
std::map<std::string, SessionEntry> g_mirror;

std::string utf16_to_utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                       nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
    return s;
}

// Parse "id:rem id:rem ..." into (id, remaining) pairs; malformed tokens skipped.
std::vector<std::pair<int, double>> parse_buffs(const std::string& s) {
    std::vector<std::pair<int, double>> out;
    std::istringstream ss(s);
    std::string tok;
    while (ss >> tok) {
        const size_t colon = tok.find(':');
        if (colon == std::string::npos) continue;
        try {
            const int    id  = std::stoi(tok.substr(0, colon));
            const double rem = std::stod(tok.substr(colon + 1));
            out.emplace_back(id, rem);
        } catch (...) { /* skip malformed pair */ }
    }
    return out;
}

// One parsed weapon-memory binding: buff id, owning weapon item id, remaining s.
struct WeaponBuff { int id; int weapon; double remaining; };

// Parse "id:weapon:rem id:weapon:rem ..." triples; malformed tokens skipped.
std::vector<WeaponBuff> parse_weapon_buffs(const std::string& s) {
    std::vector<WeaponBuff> out;
    std::istringstream ss(s);
    std::string tok;
    while (ss >> tok) {
        const size_t c1 = tok.find(':');
        if (c1 == std::string::npos) continue;
        const size_t c2 = tok.find(':', c1 + 1);
        if (c2 == std::string::npos) continue;
        try {
            const int    id     = std::stoi(tok.substr(0, c1));
            const int    weapon = std::stoi(tok.substr(c1 + 1, c2 - c1 - 1));
            const double rem    = std::stod(tok.substr(c2 + 1));
            out.push_back(WeaponBuff{ id, weapon, rem });
        } catch (...) { /* skip malformed triple */ }
    }
    return out;
}

// One parsed cast-time payload: buff id + its 10 entry dwords (+0x50..0x78),
// with every non-persisted slot left at the skip sentinel.
struct BuffPayload { int id; std::array<uint32_t, 10> dwords; };

// Parse the compact "id:idx=hex,idx=hex ... id:..." form: only the payload slots
// that carry a scaled magnitude are stored (index-tagged); the rest default to
// the skip sentinel so sp_entry_restore leaves them alone. Malformed fields are
// skipped; a token with no valid slot is dropped.
std::vector<BuffPayload> parse_payloads(const std::string& s) {
    std::vector<BuffPayload> out;
    std::istringstream ss(s);
    std::string tok;
    while (ss >> tok) {
        const size_t colon = tok.find(':');
        if (colon == std::string::npos) continue;
        try {
            const int id = std::stoi(tok.substr(0, colon));
            std::array<uint32_t, 10> p;
            p.fill(kPayloadSkip); // every unlisted slot: skip on restore
            std::istringstream ps(tok.substr(colon + 1));
            std::string field;
            bool any = false;
            while (std::getline(ps, field, ',')) {
                const size_t eq = field.find('=');
                if (eq == std::string::npos) continue;
                const int idx = std::stoi(field.substr(0, eq));
                if (idx < 0 || idx > 9) continue;
                p[idx] = static_cast<uint32_t>(std::stoul(field.substr(eq + 1), nullptr, 16));
                any = true;
            }
            if (!any) continue; // no usable slot -> nothing to restore
            out.push_back(BuffPayload{ id, p });
        } catch (...) { /* skip malformed token */ }
    }
    return out;
}

// Rewrite the whole mirror to <state>.tmp, then atomically replace the real file
// (MoveFileEx WRITE_THROUGH) so a crash mid-write can't corrupt the state.
void write_state_file() {
    const std::wstring final_path = state_path();
    const std::wstring tmp_path   = final_path + L".tmp";
    {
        std::ofstream f(tmp_path, std::ios::out | std::ios::trunc);
        if (!f) {
            flog("[WARN] session: cannot open state tmp file for write");
            return;
        }
        f << "; PersistentBuffs cross-session state -- machine-written; safe to edit or delete.\n";
        f << "; buffs = space-separated id:remaining_seconds pairs; -1 = infinite.\n";
        f << "; weapon_buffs = space-separated id:weapon:remaining_seconds triples\n";
        f << ";   (weapon-memory bindings -- which weapon each grease/blade buff is on).\n";
        f << "; buff_payloads = space-separated id:idx=hex,.. -- only the entry payload\n";
        f << ";   slots that carry a scaled magnitude, so buffs restore scaled, not base.\n";
        f << "[state]\n";
        f << "version = " << kSessionFormatVersion << "\n\n";
        for (const auto& [section, e] : g_mirror) {
            f << '[' << section << "]\n";
            f << "saved_unix = " << e.saved_unix << "\n";
            f << "buffs = " << e.buffs << "\n";
            f << "weapon_buffs = " << e.weapon_buffs << "\n";
            f << "buff_payloads = " << e.payloads << "\n\n";
        }
    } // flush + close before the move
    if (!MoveFileExW(tmp_path.c_str(), final_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        flog("[WARN] session: atomic state replace failed (err %lu)", GetLastError());
    }
}

} // namespace

std::string session_char_key(const std::wstring& name) {
    if (name.empty()) return "default";
    const std::string utf8 = utf16_to_utf8(name);
    std::string san;
    for (char c : utf8) {
        if (san.size() >= 24) break;
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        san += ok ? c : '_';
    }
    if (san.empty()) san = "char"; // name sanitized to nothing (all non-ASCII)
    // FNV-1a (32-bit) over the RAW UTF-16 bytes -> 4 hex digits, so two names
    // that sanitize identically still get distinct keys.
    uint32_t h = 0x811c9dc5u;
    const auto* bytes = reinterpret_cast<const unsigned char*>(name.data());
    const size_t nbytes = name.size() * sizeof(wchar_t);
    for (size_t i = 0; i < nbytes; ++i) { h ^= bytes[i]; h *= 0x01000193u; }
    char tag[8];
    std::snprintf(tag, sizeof(tag), "_%04x", static_cast<unsigned>(h & 0xFFFFu));
    return san + tag;
}

std::string session_current_key(const std::string& prev_key, bool* name_ok) {
    const std::wstring name = get_character_name();
    if (name_ok) *name_ok = !name.empty();
    if (name.empty())
        return prev_key.empty() ? std::string("default") : prev_key;
    return session_char_key(name);
}

void session_startup_load() {
    g_mirror.clear();
    Ini ini;
    if (!ini.load(state_path())) {
        flog("session: no state file yet -- starting fresh");
        return;
    }
    const int ver = ini.get_int("state", "version", 0);
    if (ver != kSessionFormatVersion) {
        flog("[WARN] session: state file version %d != %d -- ignoring (will overwrite)",
             ver, kSessionFormatVersion);
        return;
    }
    size_t n = 0;
    for (const std::string& sec : ini.section_names()) {
        if (sec.rfind("char_", 0) != 0) continue; // only per-character sections
        SessionEntry e;
        try { e.saved_unix = std::stoll(ini.get_string(sec, "saved_unix", "0")); }
        catch (...) { e.saved_unix = 0; }
        e.buffs        = ini.get_string(sec, "buffs", "");
        e.weapon_buffs = ini.get_string(sec, "weapon_buffs", "");
        e.payloads     = ini.get_string(sec, "buff_payloads", "");
        g_mirror[sec] = std::move(e);
        ++n;
    }
    flog("session: loaded %zu character state entr%s from disk", n, n == 1 ? "y" : "ies");
}

void session_save(const std::string& key, const std::vector<int>& remembered) {
    // Written when EITHER feature is on: session persistence needs the flat
    // list; weapon memory needs its bindings persisted so a game restart
    // doesn't erase buffs parked on a stowed weapon. (Restore stays gated on
    // g_session_persist -- see session_restore's caller in main.cpp.)
    if (!g_session_persist && !g_weapon_memory) return;
    const std::string section = "char_" + key;

    std::string buffs;
    std::vector<int> payload_ids; // ids we persisted -> save their cast-time payload
    size_t count = 0;
    for (int id : remembered) {
        if (count >= kSessionMaxBuffs) {
            flog("session: truncated save for '%s' at %zu buff(s)", key.c_str(),
                 kSessionMaxBuffs);
            break;
        }
        if (timing_is_expired(id)) continue; // don't persist an already-dead buff
        const double rem = timing_remaining(id);
        char pair[32];
        if (std::isfinite(rem))
            std::snprintf(pair, sizeof(pair), "%d:%.1f", id, rem < 0.0 ? 0.0 : rem);
        else
            std::snprintf(pair, sizeof(pair), "%d:-1", id); // infinite (IWB etc.)
        if (!buffs.empty()) buffs += ' ';
        buffs += pair;
        payload_ids.push_back(id);
        ++count;
    }

    // Weapon-memory bindings: one id:weapon:remaining triple per (buff, owning
    // weapon). A parked buff's clock is frozen while it's absent from the live
    // list (timing_tick only advances present ids), so timing_remaining is its
    // true remaining time. Expired ids are skipped like the flat list above.
    std::string weapon_buffs;
    if (g_weapon_memory) {
        size_t wcount = 0;
        for (const auto& [id, weapons] : weapon_memory_owners()) {
            if (timing_is_expired(id)) continue;
            const double rem = timing_remaining(id);
            for (int weapon : weapons) {
                if (wcount >= kSessionMaxBuffs) break;
                char triple[48];
                if (std::isfinite(rem))
                    std::snprintf(triple, sizeof(triple), "%d:%d:%.1f",
                                  id, weapon, rem < 0.0 ? 0.0 : rem);
                else
                    std::snprintf(triple, sizeof(triple), "%d:%d:-1", id, weapon);
                if (!weapon_buffs.empty()) weapon_buffs += ' ';
                weapon_buffs += triple;
                ++wcount;
            }
            payload_ids.push_back(id); // parked buff's payload is still frozen in g_snap
            if (wcount >= kSessionMaxBuffs) {
                flog("session: truncated weapon-memory save for '%s' at %zu binding(s)",
                     key.c_str(), kSessionMaxBuffs);
                break;
            }
        }
    }

    // Cast-time payloads for every id we persisted (flat + weapon-parked), so a
    // scaling weapon buff (seal/staff-cast) restores its catalyst-scaled
    // magnitude across a restart, not the param base -- the same state the
    // in-session weapon-swap restore already gives back. Only ids captured live
    // this session yield a snapshot; the rest are silently skipped.
    //
    // Written COMPACTLY: only slots that carry an actual scaled value are stored
    // (index-tagged "idx=hex"). Skipped are the non-restorable sentinel slots and
    // the neutral defaults 0.0 / 1.0 (an unscaled multiplier the bare re-apply
    // already reproduces -- restoring it is a no-op). A buff with no such slot
    // (a flat, non-scaling buff whose payload is all 1.0) is omitted entirely
    // rather than writing ten dead dwords.
    std::string payloads;
    {
        std::unordered_set<int> done;
        for (int id : payload_ids) {
            if (!done.insert(id).second) continue; // one payload per id
            uint32_t p[10];
            if (!sp_entry_payload_snapshot(id, p)) continue;
            std::string fields;
            for (int i = 0; i < 10; ++i) {
                const uint32_t v = p[i];
                if (v == kPayloadSkip) continue;              // not restorable
                if (v == 0x00000000u || v == 0x3F800000u) continue; // neutral 0.0 / 1.0
                char f[20];
                std::snprintf(f, sizeof(f), "%s%d=%08x",
                              fields.empty() ? "" : ",", i, v);
                fields += f;
            }
            if (fields.empty()) continue; // no scaled magnitude worth persisting
            char head[16];
            std::snprintf(head, sizeof(head), "%d:", id);
            if (!payloads.empty()) payloads += ' ';
            payloads += head;
            payloads += fields;
        }
    }

    // Avoid churning the file for a character standing around with no buffs.
    auto it = g_mirror.find(section);
    if (buffs.empty() && weapon_buffs.empty() &&
        (it == g_mirror.end() ||
         (it->second.buffs.empty() && it->second.weapon_buffs.empty())))
        return;

    SessionEntry& e = g_mirror[section];
    e.saved_unix   = static_cast<long long>(std::time(nullptr));
    e.buffs        = std::move(buffs);
    e.weapon_buffs = std::move(weapon_buffs);
    e.payloads     = std::move(payloads);
    write_state_file();
}

bool session_restore(const std::string& key, std::vector<int>& remembered) {
    // A character switch (or first load): never inherit the previous character's
    // clocks, buffs, weapon bindings, or cast-time payloads, whether or not a
    // saved entry exists. (Clearing g_snap here -- rather than in the caller after
    // restore -- lets the payload seeding below survive.)
    timing_clear();
    weapon_memory_clear_owners();
    sp_entry_clear();
    remembered.clear();

    const auto it = g_mirror.find("char_" + key);
    if (it == g_mirror.end()) return false;

    const auto pairs = parse_buffs(it->second.buffs);
    std::string logline;
    std::unordered_set<int> seeded; // ids whose clock came from the flat list
    for (const auto& [id, rem] : pairs) {
        if (!is_persistable(id)) continue; // filter drifted / edited-in junk
        // Remaining-time sanity: a buff already at/below the expiry margin has
        // effectively run out -- don't resurrect it. (rem < 0 == infinite, keep.)
        if (rem >= 0.0 && rem <= static_cast<double>(kExpiryMarginS)) continue;

        timing_seed(id, rem);
        seeded.insert(id);
        remembered.push_back(id);

        // Log the outcome, incl. the two IWB-driven special cases.
        const auto* r = sp_row(id);
        const bool row_finite = (r && r->effectEndurance > 0.0f);
        char tail[24];
        if (!row_finite)        std::snprintf(tail, sizeof(tail), "(inf) ");  // row untracked now
        else if (rem < 0.0)     std::snprintf(tail, sizeof(tail), "(full) "); // stored inf, row finite now
        else                    std::snprintf(tail, sizeof(tail), "(%.1fs) ", rem);
        logline += named(id); logline += tail;
    }
    flog("session: restored %zu buff(s) for '%s': [ %s]",
         remembered.size(), key.c_str(), logline.c_str());

    // Weapon-memory bindings: seed g_buff_owner so swapping to the owning
    // weapon re-applies the buff (weapon_memory_tick step 3), and seed the
    // clock for buffs that were PARKED at save time (absent from the flat
    // list). The flat entry, saved from the live buff, stays authoritative --
    // never overwrite a clock it already seeded.
    if (g_weapon_memory && !it->second.weapon_buffs.empty()) {
        size_t nbind = 0;
        for (const WeaponBuff& wb : parse_weapon_buffs(it->second.weapon_buffs)) {
            if (!is_persistable(wb.id)) continue;
            if (wb.remaining >= 0.0 &&
                wb.remaining <= static_cast<double>(kExpiryMarginS)) continue;
            if (!seeded.count(wb.id)) {
                timing_seed(wb.id, wb.remaining);
                seeded.insert(wb.id);
            }
            weapon_memory_seed_owner(wb.id, wb.weapon);
            ++nbind;
        }
        flog("session: restored %zu weapon binding(s) for '%s'", nbind, key.c_str());
    }

    // Cast-time payload restore: re-seed each saved id's entry payload into
    // g_snap so the upcoming apply_persisted -> sp_entry_restore gives back the
    // catalyst-scaled magnitude (e.g. a seal-cast weapon buff's full "+200"),
    // not the param base -- the same fix the in-session weapon swap already had,
    // now across a game restart. Seeded for BOTH flat and weapon-parked ids
    // (payloads are keyed by buff id, and weapon step-3 re-apply reads g_snap
    // too). g_snap was cleared above, so no stale payload leaks in.
    if (!it->second.payloads.empty()) {
        size_t np = 0;
        for (const BuffPayload& bp : parse_payloads(it->second.payloads)) {
            if (!is_persistable(bp.id)) continue; // filter drifted / edited-in junk
            sp_entry_seed(bp.id, bp.dwords.data());
            ++np;
        }
        flog("session: restored %zu buff payload(s) for '%s'", np, key.c_str());
    }
    return true;
}

} // namespace pb
