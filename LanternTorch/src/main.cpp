// ============================================================
//  Lantern Torch - a libER param-patcher DLL
//
//  Makes the Lantern (Goods 2070) behave like a torch: the SpEffect the
//  lantern applies while it is equipped is overwritten in memory with the
//  resident SpEffect of the torch you pick in the .ini, so the lantern
//  gains that torch's passive behaviour (Sentry's Torch revealing invisible
//  enemies, Beast-Repellent Torch driving beasts off, the plain Torch's
//  fire aura, ...).
//
//  Everything is a live param edit -- no regulation.bin, no FXR patching:
//
//    EquipParamWeapon[<torch>].residentSpEffectId{,1,2}
//        -> the torch's "while held" SpEffect(s)
//    SpEffectParam[<lantern effect>]  = SpEffectParam[<torch effect>]
//        -> whole-row mirror, with the lantern's own identity fields put
//           back afterwards (see kept fields below)
//  ...and one memory patch for the light itself (see THE LIGHT below).
//
//  Fields always kept from the lantern row after the mirror, so the lantern
//  goes on behaving exactly like a lantern:
//    effectEndurance  } lifecycle -- the lantern effect is managed by the
//    motionInterval   } equip/unequip code and must keep its own timing
//    stateInfo        the engine's handle on "this is the lantern effect"
//    iconId           the buff icon shown in the HUD
//    vfxId0..7        the lantern's own light, left completely alone
//
//  THE LIGHT: a torch's flame can't be copied -- it hangs off the weapon
//  model, and no param carries it (every torch's SpEffect dumps vfx = -1).
//  So the lantern keeps its own light, and instead of swapping it we make it
//  stronger: the light's radius and intensity live inside the FXR the game
//  has loaded in memory (f000302421.fxr, the lantern light), and they are
//  written straight into it. Same approach CustomLantern (0-F) uses, and its
//  signature/offset research is what made it possible:
//      https://github.com/0-F/CustomLantern
//
//  IMPORTANT: only *resident* (passive, while-held) torch effects can be
//  mirrored. Torch traits that live on the attack -- St. Trina's sleep,
//  Steel-Wire's bleed, Ghostflame's frost -- are on the weapon's attack
//  data, and a lantern has no attack, so those torches may have nothing to
//  mirror. The log says so explicitly when that happens.
//
//  Set [debugging] dump = 1 to write the lantern + every torch's raw rows
//  to the log without changing anything. Run OFFLINE only (EasyAntiCheat).
// ============================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdarg>
#include <cstdio>
#include <exception>
#include <fstream>
#include <string>
#include <vector>

// libER: wait_for_params
#include <coresystem/cs_param.hpp>
// libER: from::param::*
#include <param/param.hpp>

#include "ini.hpp"

namespace {

HINSTANCE g_hinst = nullptr;

// ---- game data ----------------------------------------------
// Goods row of the Lantern (Paramdex: EquipParamGoods 2070 "Lantern").
constexpr int kLanternGoods = 2070;

// SpEffects the lantern applies while equipped (Paramdex SpEffectParam
// 3245 "[Item] Lantern - Right", 3246 "[Item] Lantern - Left"). Used as the
// fallback when the goods row's own refs don't resolve.
constexpr int kLanternSpEffects[] = { 3245, 3246 };

// The lantern light's FXR (sfx\effect\f000302421.fxr), found in memory by its
// header: "FXR\0" + version 5, then \1 + the effect id 302421 (0x00049D55).
constexpr unsigned long long kFxrSig0 = 0x0005000000525846ull;
constexpr unsigned long long kFxrSig1 = 0x00049D5500000001ull;

// Vanilla values inside that FXR, for the .ini comments and the log.
constexpr float kVanillaRadius    = 16.0f;
constexpr float kVanillaIntensity = 1.25f;

struct Torch {
    int         id;
    const char* name;
};
// EquipParamWeapon base rows (+0). Torchpole is a halberd, not a torch, but
// it burns and people ask for it, so it's selectable too.
constexpr Torch kTorches[] = {
    { 24000000, "Torch"                  },
    { 24020000, "Steel-Wire Torch"       },
    { 24040000, "St. Trina's Torch"      },
    { 24050000, "Ghostflame Torch"       },
    { 24060000, "Beast-Repellent Torch"  },
    { 24070000, "Sentry's Torch"         },
    { 24500000, "Nanaya's Torch"         },
    { 16080000, "Torchpole"              },
};

const char* torch_name(int id) {
    for (const auto& t : kTorches)
        if (t.id == id) return t.name;
    return "(not in the known-torch list)";
}

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
}

void flog(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    log_line(buf);
}

// ---- param row accessors (binary search by id) --------------
from::paramdef::SP_EFFECT_PARAM_ST* sp_row(int id) {
    if (id < 0) return nullptr;
    auto [row, ok] = from::param::SpEffectParam[id];
    return ok ? &row : nullptr;
}
from::paramdef::SP_EFFECT_VFX_PARAM_ST* vfx_row(int id) {
    if (id < 0) return nullptr;
    auto [row, ok] = from::param::SpEffectVfxParam[id];
    return ok ? &row : nullptr;
}
from::paramdef::EQUIP_PARAM_WEAPON_ST* weapon_row(int id) {
    if (id < 0) return nullptr;
    auto [row, ok] = from::param::EquipParamWeapon[id];
    return ok ? &row : nullptr;
}
from::paramdef::EQUIP_PARAM_GOODS_ST* goods_row(int id) {
    if (id < 0) return nullptr;
    auto [row, ok] = from::param::EquipParamGoods[id];
    return ok ? &row : nullptr;
}

// ---- .ini helpers -------------------------------------------
int get_int(const Ini& ini, const std::string& sec, const std::string& key, int def) {
    const std::string v = ini.get_string(sec, key);
    if (v.empty()) return def;
    try { return std::stoi(v, nullptr, 0); } catch (...) { return def; }
}

void push_unique(std::vector<int>& v, int id) {
    for (int x : v) if (x == id) return;
    v.push_back(id);
}

// ---- the lantern's SpEffects --------------------------------
// Prefer whatever the goods row actually points at (so an overhaul mod that
// moved the lantern effect still works); fall back to the vanilla pair.
std::vector<int> lantern_speffects() {
    std::vector<int> found;
    if (auto* g = goods_row(kLanternGoods)) {
        // refCategory 1 = the ref ids are bullets; anything else (the vanilla
        // lantern is 2) points them at SpEffects.
        if (g->refCategory != 1) {
            for (int r : { g->refId_default, g->refId_1 })
                if (sp_row(r)) push_unique(found, r);
        }
    } else {
        flog("[WARN] EquipParamGoods %d (Lantern) not found", kLanternGoods);
    }

    for (int id : kLanternSpEffects)
        if (sp_row(id)) push_unique(found, id);

    return found;
}

// ---- dump mode ----------------------------------------------
void dump_speffect(const char* tag, int id) {
    auto* sp = sp_row(id);
    if (!sp) { flog("  %s SpEffect %d: <missing>", tag, id); return; }
    flog("  %s SpEffect %d: endurance=%.2f interval=%.2f stateInfo=%u icon=%d "
         "targetSelf=%d",
         tag, id, sp->effectEndurance, sp->motionInterval,
         static_cast<unsigned>(sp->stateInfo), sp->iconId,
         sp->effectTargetSelf ? 1 : 0);
    flog("      vfx = %d %d %d %d %d %d %d %d",
         sp->vfxId, sp->vfxId1, sp->vfxId2, sp->vfxId3,
         sp->vfxId4, sp->vfxId5, sp->vfxId6, sp->vfxId7);
    flog("      replace=%d cycle=%d atkOccurrence=%d",
         sp->replaceSpEffectId, sp->cycleOccurrenceSpEffectId,
         sp->atkOccurrenceSpEffectId);
    const int vfxIds[] = { sp->vfxId, sp->vfxId1, sp->vfxId2, sp->vfxId3 };
    for (int v : vfxIds) {
        auto* vf = vfx_row(v);
        if (!vf) continue;
        flog("      vfx row %d: midstSfx=%d midstDmy=%d initSfx=%d finishSfx=%d "
             "effectType=%u",
             v, vf->midstSfxId, static_cast<int>(vf->midstDmyId),
             vf->initSfxId, vf->finishSfxId,
             static_cast<unsigned>(vf->effectType));
    }
}

void dump() {
    flog("---- DUMP: lantern ----");
    if (auto* g = goods_row(kLanternGoods)) {
        flog("  EquipParamGoods %d: refId_default=%d refId_1=%d refCategory=%u "
             "goodsType=%u behaviorId=%d",
             kLanternGoods, g->refId_default, g->refId_1,
             static_cast<unsigned>(g->refCategory),
             static_cast<unsigned>(g->goodsType), g->behaviorId);
    }
    for (int id : lantern_speffects())
        dump_speffect("lantern", id);

    flog("---- DUMP: torches ----");
    for (const auto& t : kTorches) {
        auto* w = weapon_row(t.id);
        if (!w) { flog("  %d %s: <row missing>", t.id, t.name); continue; }
        flog("  %d %s: resident=%d,%d,%d  behavior=%d,%d,%d",
             t.id, t.name, w->residentSpEffectId, w->residentSpEffectId1,
             w->residentSpEffectId2, w->spEffectBehaviorId0,
             w->spEffectBehaviorId1, w->spEffectBehaviorId2);
        for (int s : { w->residentSpEffectId, w->residentSpEffectId1,
                       w->residentSpEffectId2 })
            if (s >= 0) dump_speffect("  torch", s);
    }
    flog("---- DUMP end (no edits applied) ----");
}

// ---- the mirror ---------------------------------------------
// Fields that describe *the lantern* rather than *the effect*. They are read
// off the lantern row before the copy and always written back after it, so
// nothing about how the lantern itself works changes -- only what it does.
struct Kept {
    int            vfx[8];
    float          effectEndurance;
    float          motionInterval;
    unsigned short stateInfo;
    int            iconId;
};

Kept capture(const from::paramdef::SP_EFFECT_PARAM_ST& r) {
    Kept k{};
    k.vfx[0] = r.vfxId;  k.vfx[1] = r.vfxId1; k.vfx[2] = r.vfxId2;
    k.vfx[3] = r.vfxId3; k.vfx[4] = r.vfxId4; k.vfx[5] = r.vfxId5;
    k.vfx[6] = r.vfxId6; k.vfx[7] = r.vfxId7;
    k.effectEndurance = r.effectEndurance;
    k.motionInterval  = r.motionInterval;
    k.stateInfo       = r.stateInfo;
    k.iconId          = r.iconId;
    return k;
}

void restore_vfx(from::paramdef::SP_EFFECT_PARAM_ST& r, const Kept& k) {
    r.vfxId  = k.vfx[0]; r.vfxId1 = k.vfx[1]; r.vfxId2 = k.vfx[2];
    r.vfxId3 = k.vfx[3]; r.vfxId4 = k.vfx[4]; r.vfxId5 = k.vfx[5];
    r.vfxId6 = k.vfx[6]; r.vfxId7 = k.vfx[7];
}

// Copy `srcId`'s whole SpEffect row onto `dstId`, then put the kept fields
// back. The lantern's VFX ids are always among them -- its light is never
// touched here, because no torch carries a flame on its SpEffect anyway
// (they all dump vfx = -1; the flame is on the weapon model).
// Returns false only if a row is missing.
bool mirror(int dstId, int srcId) {
    auto* dst = sp_row(dstId);
    auto* src = sp_row(srcId);
    if (!dst) { flog("[WARN] lantern SpEffect %d not found -- skipped", dstId); return false; }
    if (!src) { flog("[WARN] torch SpEffect %d not found -- skipped", srcId); return false; }

    const Kept k = capture(*dst);

    *dst = *src;                       // <- the mirror

    restore_vfx(*dst, k);

    dst->effectEndurance = k.effectEndurance;
    dst->motionInterval  = k.motionInterval;
    dst->stateInfo       = k.stateInfo;
    dst->iconId          = k.iconId;

    flog("  mirrored SpEffect %d -> lantern SpEffect %d (vfx now %d, "
         "endurance %.2f, stateInfo %u)",
         srcId, dstId, dst->vfxId, dst->effectEndurance,
         static_cast<unsigned>(dst->stateInfo));
    return true;
}

// ---- the light: radius + intensity, written into the FXR -----
// The lantern light's FXR is a plain block of data in the game's memory once
// it's loaded, and its radius / intensity are two floats at a fixed distance
// from the light's colour block. Find the FXR by its header, work out where
// that colour block sits, and write.

// Walk the process's committed private read/write memory looking for the FXR
// header. Returns null until the game has actually loaded the effect.
unsigned char* find_lantern_fxr() {
    MEMORY_BASIC_INFORMATION mbi{};
    uintptr_t addr = 0;

    while (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t next = base + mbi.RegionSize;

        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            mbi.Protect == PAGE_READWRITE && mbi.RegionSize >= 16) {
            auto* p   = reinterpret_cast<unsigned long long*>(base);
            auto* end = reinterpret_cast<unsigned long long*>(next - 16);
            for (; p < end; ++p)
                if (p[0] == kFxrSig0 && p[1] == kFxrSig1)
                    return reinterpret_cast<unsigned char*>(p);
        }

        if (next <= addr) break;    // no progress / wrapped -> stop
        addr = next;
    }
    return nullptr;
}

// Distance from the FXR's start to its light colour block. The FXR is laid
// out differently once a tool like SoulsFormats has rewritten it, so pick the
// layout by looking for the effect id at the position each one implies.
int colour_offset(const unsigned char* fxr) {
    constexpr unsigned int kEffectId = 0x00049D55;   // 302421
    if (*reinterpret_cast<const unsigned int*>(fxr + 0x1D84) == kEffectId)
        return 0x1CDC;                               // as the game ships it
    if (*reinterpret_cast<const unsigned int*>(fxr + 0x1C0C) == kEffectId)
        return 0x1D50;                               // repacked by another mod
    flog("[WARN] the lantern FXR doesn't match either known layout -- "
         "assuming the vanilla one (another mod may have rewritten it)");
    return 0x1CDC;
}

// Wait for the effect to be loaded, then write radius / intensity into it.
// Both are skipped when left at 0, so an .ini that only sets one doesn't
// disturb the other.
void apply_light(const Ini& ini) {
    const float radius    = ini.get_float("light", "radius", 0.0f);
    const float intensity = ini.get_float("light", "intensity", 0.0f);

    if (radius <= 0.0f && intensity <= 0.0f) {
        flog("light: radius and intensity both unset -> light left vanilla");
        return;
    }

    // The effect isn't in memory at startup, so keep looking for a while.
    constexpr int kTries       = 40;
    constexpr int kIntervalMs  = 3000;
    unsigned char* fxr = nullptr;
    for (int i = 0; i < kTries && !fxr; ++i) {
        fxr = find_lantern_fxr();
        if (!fxr) Sleep(kIntervalMs);
    }
    if (!fxr) {
        flog("[WARN] lantern light effect not found in memory after %d s -- "
             "radius / intensity not applied",
             kTries * kIntervalMs / 1000);
        return;
    }
    flog("light: lantern FXR found at 0x%p", fxr);

    const int colour = colour_offset(fxr);
    auto* pRadius    = reinterpret_cast<float*>(fxr + colour + 0x20);
    auto* pIntensity = reinterpret_cast<float*>(fxr + colour + 0x30);

    if (radius > 0.0f) {
        flog("light: radius %.2f -> %.2f (vanilla %.2f)",
             *pRadius, radius, kVanillaRadius);
        *pRadius = radius;
    }
    if (intensity > 0.0f) {
        flog("light: intensity %.2f -> %.2f (vanilla %.2f)",
             *pIntensity, intensity, kVanillaIntensity);
        *pIntensity = intensity;
    }
}

void apply(const Ini& ini) {
    const int torchId = get_int(ini, "lantern", "torch", 0);
    if (torchId <= 0) {
        flog("[lantern] torch = 0 (or unset) -> no torch effect applied");
        return;
    }

    auto* torch = weapon_row(torchId);
    if (!torch) {
        flog("[ERROR] weapon %d not found in EquipParamWeapon -- check "
             "[lantern] torch in the .ini (use a base +0 id, e.g. 24070000)",
             torchId);
        return;
    }
    flog("torch = %d (%s)", torchId, torch_name(torchId));

    // The passive, "while it's in your hand" effects of that torch.
    std::vector<int> srcEffects;
    for (int s : { torch->residentSpEffectId, torch->residentSpEffectId1,
                   torch->residentSpEffectId2 })
        if (s >= 0 && sp_row(s)) push_unique(srcEffects, s);

    if (srcEffects.empty()) {
        flog("[WARN] %s has no resident SpEffect -- its traits are on its "
             "attacks (sleep / bleed / frost), which a lantern can't use. "
             "Nothing to mirror; pick another torch (Torch, Sentry's Torch "
             "and Beast-Repellent Torch all carry passive effects).",
             torch_name(torchId));
        return;
    }
    flog("resident SpEffect(s) on the torch: %zu found", srcEffects.size());

    const std::vector<int> dstEffects = lantern_speffects();
    if (dstEffects.empty()) {
        flog("[ERROR] could not resolve the lantern's SpEffect rows (expected "
             "3245 / 3246) -- possible libER or game-version mismatch");
        return;
    }

    // The lantern has a row per hand, but only one of them is real: the
    // vanilla Left row (3246) is an empty stub with no duration and no light,
    // so an effect written there would never fire. Everything therefore goes
    // on the live row -- the one with a duration.
    int live = dstEffects[0];
    for (int id : dstEffects) {
        auto* r = sp_row(id);
        if (r && r->effectEndurance != 0.0f) { live = id; break; }
    }

    // Which of the torch's effects to put there. All torches share the same
    // generic fire aura (415) and the interesting ones add their own on top,
    // so the LAST resident effect is the distinctive one -- that's the one
    // worth having, and there's only one slot for it.
    const int srcId = srcEffects.back();
    for (int s : srcEffects)
        if (s != srcId)
            flog("  note: the torch also has SpEffect %d, which the lantern "
                 "has no slot for (only its distinctive effect is applied)", s);

    if (!mirror(live, srcId)) return;
    flog("done: lantern SpEffect %d now carries %s's effect %d",
         live, torch_name(torchId), srcId);
}

// ---- worker thread (param load blocks; never do that in DllMain)
DWORD WINAPI run(LPVOID) {
    Ini ini;
    const bool loaded = ini.load(config_path());

    flog(loaded ? "config loaded"
                : "[WARN] .ini not found next to the DLL; using defaults");

    try {
        flog("waiting for params...");
        from::CS::SoloParamRepository::wait_for_params(-1);
        flog("params ready");
        if (ini.get_bool("debugging", "dump", false)) {
            flog("DUMP MODE ON -- reporting rows only, no edits");
            dump();
            return 0;
        }
        apply(ini);
        apply_light(ini);   // waits for the effect to load; can take a while
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
        log_line("==== LanternTorch loaded (DllMain attach) ====",
                 /*truncate=*/true);
        CreateThread(nullptr, 0, run, nullptr, 0, nullptr);
    }
    return TRUE;
}
