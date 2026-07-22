// ============================================================
//  OmniCaster - cast any spell with any catalyst, unified scaling
//
//  A libER param-patcher DLL for ELDEN RING (in-memory, no regulation.bin):
//    * staffs cast incantations, seals cast sorceries (cast_anything)
//    * spell power follows the catalyst you're holding: each staff's holy
//      side is mirrored from its magic side and vice-versa, across
//      EquipParamWeapon + AttackElementCorrectParam + ReinforceParamWeapon
//      (scaling_mode = equipped)
//    * or: every catalyst scales both spell types off max(INT, FAI),
//      tracked live via a 1 s poll of PlayerGameData
//      (scaling_mode = highest_stat)
//    * optionally, each spell's INT/FAI requirement is checked against that
//      same higher stat -- same number, other stat (requirement_swap)
//
//  Run OFFLINE only (EasyAntiCheat).
// ============================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdio>
#include <exception>
#include <string>

// libER: wait_for_params
#include <coresystem/cs_param.hpp>

#include "ini.hpp"
#include "names.hpp"
#include "player_stats.hpp"
#include "scaling.hpp"
#include "utils.hpp"

namespace omni {
namespace {

ScalingMode parse_mode(const std::string& s) {
    if (s == "off") return ScalingMode::Off;
    if (s == "highest_stat" || s == "highest") return ScalingMode::HighestStat;
    if (s != "equipped")
        flog("[WARN] unknown scaling_mode '%s' -- using 'equipped'", s.c_str());
    return ScalingMode::Equipped;
}

DWORD WINAPI run(LPVOID) {
    Ini ini;
    const bool loaded = ini.load(config_path());

    g_debug = ini.get_bool("debugging", "debug_console", false);
    if (g_debug) {
        AllocConsole();
        FILE* out = nullptr;
        freopen_s(&out, "CONOUT$", "w", stdout);
    }

    Config cfg;
    cfg.cast_anything    = ini.get_bool("general", "cast_anything", true);
    cfg.mode             = parse_mode(ini.get_string("general", "scaling_mode", "equipped"));
    cfg.requirement_swap = ini.get_bool("general", "requirement_swap", false);
    cfg.dump             = ini.get_bool("debugging", "dump", false);

    flog(loaded ? "config loaded" : "[WARN] .ini not found next to the DLL; using defaults");
    flog("cast_anything=%d scaling_mode=%s requirement_swap=%d dump=%d",
         cfg.cast_anything ? 1 : 0,
         cfg.mode == ScalingMode::Off        ? "off"
         : cfg.mode == ScalingMode::Equipped ? "equipped"
                                             : "highest_stat",
         cfg.requirement_swap ? 1 : 0,
         cfg.dump ? 1 : 0);

    names_init();

    try {
        flog("waiting for params...");
        from::CS::SoloParamRepository::wait_for_params(-1);
        flog("params ready -- applying edits");
        apply_all(cfg);
        flog("param pass done.");
    } catch (const std::exception& e) {
        flog("[ERROR] exception: %s", e.what());
        return 0;
    } catch (...) {
        flog("[ERROR] unknown exception while applying edits");
        return 0;
    }

    // The poll is shared: scaling flips and requirement swaps both key off the
    // player's higher casting stat, so either feature alone needs it running.
    if (cfg.mode == ScalingMode::HighestStat || cfg.requirement_swap) {
        if (!player_stats_init())
            flog("[WARN] player stats unreadable -- catalysts stay on their "
                 "post-mirror (equipped-style) scaling and spell requirements "
                 "stay vanilla");
        for (;;) {
            highest_stat_tick(cfg);
            Sleep(1000);
        }
    }
    return 0;
}

} // namespace
} // namespace omni

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        omni::g_hinst = hinst;
        DisableThreadLibraryCalls(hinst);
        omni::log_line("==== OmniCaster loaded (DllMain attach) ====", /*truncate=*/true);
        CreateThread(nullptr, 0, omni::run, nullptr, 0, nullptr);
    }
    return TRUE;
}
