// ============================================================
//  Adjustable Spell Cost - a libER param-patcher DLL
//
//  Makes sorceries and incantations cheaper to cast and easier to
//  meet the stat requirements for, all in memory -- no regulation.bin.
//
//    Magic.mp / mp_charge              = FP cost      -> divided (floored)
//    Magic.requirementIntellect        = INT req      -> divided (min 1)
//    Magic.requirementFaith            = Faith req    -> divided (min 1)
//    Magic.requirementLuck             = Arcane req   -> divided (min 1)
//
//    EquipParamWeapon.proper*          = catalyst stat requirements
//                                         (staffs/seals only) -> divided (min 1)
//
//  In Elden Ring the `Magic` param contains ONLY sorceries and
//  incantations, so every row with a cost / requirement is a spell --
//  there's no extra filtering to do (see README). Catalysts (staffs and
//  sacred seals) are identified the same way OmniCaster does it: a weapon
//  row with exactly one of enableMagic/enableMiracle set (see
//  `is_catalyst`). Run OFFLINE only (EasyAntiCheat).
// ============================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iostream>
#include <string>

// libER: wait_for_params
#include <coresystem/cs_param.hpp>
// libER: from::param::Magic
#include <param/param.hpp>

#include "ini.hpp"

namespace {

HINSTANCE g_hinst = nullptr;
bool      g_debug = false;

// ---- paths: config next to the DLL, log in a logs/ subfolder ----
std::wstring module_path() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(g_hinst, buf, MAX_PATH);
    return std::wstring(buf);
}
std::wstring dir_of(const std::wstring& p) {
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}
std::wstring stem_of(const std::wstring& p) {
    const size_t slash = p.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? p : p.substr(slash + 1);
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? name : name.substr(0, dot);
}
std::wstring config_path() {
    const std::wstring m = module_path();
    return dir_of(m) + L"\\" + stem_of(m) + L".ini";
}
std::wstring log_path() {
    const std::wstring m = module_path();
    return dir_of(m) + L"\\logs\\" + stem_of(m) + L".log";
}

// ---- logging: ALWAYS writes logs/<DllName>.log near the DLL --
void log_line(const std::string& msg, bool truncate = false) {
    const std::wstring path = log_path();
    CreateDirectoryW(dir_of(path).c_str(), nullptr); // ensure logs/ exists
    std::ofstream f(path,
                    std::ios::out | (truncate ? std::ios::trunc : std::ios::app));
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char ts[24];
        std::snprintf(ts, sizeof(ts), "[%02d:%02d:%02d.%03d] ",
                      st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        f << ts << msg << '\n';
    }
    if (g_debug)
        std::cout << "[AdjustableSpellCost] " << msg << std::endl;
}

void flog(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    log_line(buf);
}

// Divide an FP cost by `divisor`, rounded, with a floor of `minCost`.
// If the vanilla cost is already at or below the floor, it's left as-is
// (we never RAISE a cheap spell up to the floor).
short divide_fp(short cost, float divisor, int minCost) {
    if (cost <= minCost) return cost;           // already cheap -> keep vanilla
    int v = static_cast<int>(std::lround(cost / divisor));
    if (v < minCost) v = minCost;               // don't drop below the floor
    if (v > 32767)   v = 32767;
    return static_cast<short>(v);
}

// Divide a stat requirement by `divisor`, rounded, floored at 1.
// A requirement of 0 means "this stat isn't required" (e.g. an incantation
// needs no INT) and is left at 0 -- we never invent a new requirement.
// The floor of 1 is hardcoded (no .ini knob), same as the spell requirement
// divider above.
unsigned char divide_req(unsigned char req, float divisor) {
    if (req <= 1) return req;                    // 0 stays 0, 1 stays 1
    int v = static_cast<int>(std::lround(req / divisor));
    if (v < 1)   v = 1;                          // spells that need a stat keep >=1
    if (v > 255) v = 255;
    return static_cast<unsigned char>(v);
}

// A staff or seal has at least one of enableMagic/enableMiracle set: staffs
// cast sorceries (enableMagic), seals cast incantations (enableMiracle), and
// natively-dual catalysts (e.g. Staff of the Great Beyond) have both. Normal
// weapons have neither.
//
// Deliberately "or", not "xor": OmniCaster's cast_anything (if that mod is
// also loaded) flips EVERY catalyst to have BOTH flags set, so an "exactly
// one" check silently finds zero catalysts whenever OmniCaster's param pass
// happens to run first (both mods load via their own independent thread --
// there's no ordering guarantee between them). "At least one" stays correct
// either way, since only catalysts ever get either flag set to begin with.
bool is_catalyst(bool enableMagic, bool enableMiracle) {
    return enableMagic || enableMiracle;
}

void apply(const Ini& ini) {
    const float fpDivisor  = ini.get_float("fp_cost", "divisor", 1.0f);
    const int   minCost    = static_cast<int>(ini.get_float("fp_cost", "min_cost", 1.0f));
    const float reqDivisor = ini.get_float("stat_requirements", "divisor", 1.0f);
    const bool  logEach    = ini.get_bool("debugging", "log_each", false);

    if (fpDivisor <= 0.0f || reqDivisor <= 0.0f) {
        flog("[WARN] divisors must be > 0 (fp=%.2f req=%.2f); nothing changed",
             fpDivisor, reqDivisor);
        return;
    }

    const bool touchFp  = std::fabs(fpDivisor  - 1.0f) >= 0.001f;
    const bool touchReq = std::fabs(reqDivisor - 1.0f) >= 0.001f;
    if (!touchFp && !touchReq) {
        flog("both divisors = 1.0 -> nothing to change");
        return;
    }

    int spells = 0, fpChanged = 0, reqChanged = 0;
    for (auto [id, row] : from::param::Magic) {
        // Every Magic row is a sorcery or incantation. Skip only truly empty
        // rows (no cost and no requirement) so the summary counts real spells.
        const bool hasReq = row.requirementIntellect || row.requirementFaith ||
                            row.requirementLuck;
        if (row.mp <= 0 && row.mp_charge <= 0 && !hasReq) continue;
        ++spells;

        const short oldMp  = row.mp;
        const short oldMpC = row.mp_charge;
        const unsigned char oldInt = row.requirementIntellect;
        const unsigned char oldFth = row.requirementFaith;
        const unsigned char oldArc = row.requirementLuck;   // Arcane == Luck internally

        bool changed = false;
        if (touchFp) {
            const short nMp  = divide_fp(oldMp,  fpDivisor, minCost);
            const short nMpC = divide_fp(oldMpC, fpDivisor, minCost);
            if (nMp  != oldMp)  { row.mp        = nMp;  changed = true; }
            if (nMpC != oldMpC) { row.mp_charge = nMpC; changed = true; }
            if (changed) ++fpChanged;
        }
        if (touchReq) {
            const unsigned char nInt = divide_req(oldInt, reqDivisor);
            const unsigned char nFth = divide_req(oldFth, reqDivisor);
            const unsigned char nArc = divide_req(oldArc, reqDivisor);
            bool rc = false;
            if (nInt != oldInt) { row.requirementIntellect = nInt; rc = true; }
            if (nFth != oldFth) { row.requirementFaith     = nFth; rc = true; }
            if (nArc != oldArc) { row.requirementLuck       = nArc; rc = true; }
            if (rc) ++reqChanged;
        }

        if (logEach)
            flog("spell %d  FP %d->%d (chg %d->%d)  req INT %d->%d FTH %d->%d ARC %d->%d",
                 static_cast<int>(id), oldMp, row.mp, oldMpC, row.mp_charge,
                 oldInt, row.requirementIntellect, oldFth, row.requirementFaith,
                 oldArc, row.requirementLuck);
    }

    flog("fp_divisor=%.2f (min %d) req_divisor=%.2f: %d spells -> FP changed on %d, requirements changed on %d",
         fpDivisor, minCost, reqDivisor, spells, fpChanged, reqChanged);
    if (spells == 0)
        flog("[WARN] no spells found in the Magic param -- possible libER/version mismatch");
}

// Divide every catalyst's (staff/seal) five stat requirements -- Strength,
// Agility (Dex), Magic (Int), Faith, Luck (Arcane) -- by the configured
// divisor. Floored at 1, same rule (and same helper) as the spell
// requirement divider: a stat the catalyst never needed (0) stays 0.
void apply_catalyst_requirements(const Ini& ini) {
    const float divisor = ini.get_float("catalyst_stat_requirements", "divisor", 1.0f);
    const bool  logEach = ini.get_bool("debugging", "log_each", false);

    if (divisor <= 0.0f) {
        flog("[WARN] catalyst_stat_requirements divisor must be > 0 (%.2f); nothing changed", divisor);
        return;
    }
    if (std::fabs(divisor - 1.0f) < 0.001f) {
        flog("catalyst_stat_requirements divisor = 1.0 -> nothing to change");
        return;
    }

    int catalysts = 0, changed = 0;
    for (auto [id, row] : from::param::EquipParamWeapon) {
        if (!is_catalyst(row.enableMagic, row.enableMiracle)) continue;
        ++catalysts;

        const unsigned char oldStr = row.properStrength;
        const unsigned char oldAgi = row.properAgility;
        const unsigned char oldInt = row.properMagic;
        const unsigned char oldFth = row.properFaith;
        const unsigned char oldArc = row.properLuck;

        const unsigned char nStr = divide_req(oldStr, divisor);
        const unsigned char nAgi = divide_req(oldAgi, divisor);
        const unsigned char nInt = divide_req(oldInt, divisor);
        const unsigned char nFth = divide_req(oldFth, divisor);
        const unsigned char nArc = divide_req(oldArc, divisor);

        bool rc = false;
        if (nStr != oldStr) { row.properStrength = nStr; rc = true; }
        if (nAgi != oldAgi) { row.properAgility  = nAgi; rc = true; }
        if (nInt != oldInt) { row.properMagic    = nInt; rc = true; }
        if (nFth != oldFth) { row.properFaith    = nFth; rc = true; }
        if (nArc != oldArc) { row.properLuck     = nArc; rc = true; }
        if (rc) ++changed;

        if (logEach)
            flog("catalyst %d  STR %d->%d DEX %d->%d INT %d->%d FTH %d->%d ARC %d->%d",
                 static_cast<int>(id), oldStr, row.properStrength, oldAgi, row.properAgility,
                 oldInt, row.properMagic, oldFth, row.properFaith, oldArc, row.properLuck);
    }

    flog("catalyst_requirements_divisor=%.2f: %d staffs/seals -> requirements changed on %d",
         divisor, catalysts, changed);
    if (catalysts == 0)
        flog("[WARN] no staffs/seals found in EquipParamWeapon -- possible libER/version mismatch");
}

// ---- worker thread (param load blocks; never do that in DllMain)
DWORD WINAPI run(LPVOID) {
    Ini ini;
    const bool loaded = ini.load(config_path());

    g_debug = ini.get_bool("debugging", "debug_console", false);
    if (g_debug) {
        AllocConsole();
        FILE* out = nullptr;
        freopen_s(&out, "CONOUT$", "w", stdout);
    }

    flog(loaded ? "config loaded"
                : "[WARN] .ini not found next to the DLL; using defaults");

    try {
        flog("waiting for params...");
        from::CS::SoloParamRepository::wait_for_params(-1);
        flog("params ready -- applying edits");
        apply(ini);
        apply_catalyst_requirements(ini);
        flog("done.");
    } catch (const std::exception& e) {
        flog("[ERROR] exception: %s", e.what());
    } catch (...) {
        flog("[ERROR] unknown exception while applying edits");
    }
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hinst = hinst;
        DisableThreadLibraryCalls(hinst);
        log_line("==== AdjustableSpellCost loaded (DllMain attach) ====",
                 /*truncate=*/true);
        CreateThread(nullptr, 0, run, nullptr, 0, nullptr);
    }
    return TRUE;
}
