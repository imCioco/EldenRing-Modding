// ============================================================
//  ER Infinite Weapon Buffs - a libER param-patcher DLL
//
//  Everything is done by rewriting param tables in memory after
//  the game loads them -- no regulation.bin edits. Two passes:
//
//    1. EquipParamWeapon.isEnhance = true on every weapon
//       => any grease / spell buff can be applied to any weapon.
//
//    2. SpEffectParam.effectEndurance = <configured value> for the
//       buff SpEffects discovered per category
//       => greases / spell buffs / consumable buffs last as long
//          as you want (-1 = permanent).
//
//  Categories are derived live from the params, code-driven (no
//  goodsType lists to maintain):
//    * greases     = goods that enchant a weapon/shield (isEnhance /
//                    isShieldEnchant) or sit in sort group 70.
//    * spell buffs = SpEffects referenced by the Magic param.
//    * consumables = buff/heal goods (sort group 20) + an explicit
//                    allowlist (e.g. the DLC Golden Vow pot). The buff
//                    SpEffect is found by following the item's
//                    behavior -> bullet -> shooter chain and the
//                    SpEffect replace/cycle chain, then keeping only
//                    self/player-targeted timed effects.
//
//  Horse-summon items (Spectral Steed Whistle) are protected: every
//  SpEffect they can reach is collected up-front and never patched, so
//  Torrent's "active" state can never get stuck.
//
//  Writes a log next to the DLL (logs/InfiniteWeaponBuffs.log) every
//  launch so you can see exactly what happened. Run OFFLINE only.
//
//  See CLAUDE.md for the per-category discovery rules. The code itself
//  is split across src/*.cpp by feature -- config.cpp owns the .ini
//  options, apply.cpp is the main duration-patching pass, discover.cpp
//  is the diagnostic dump.
// ============================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <exception>

// libER: SoloParamRepository::wait_for_params
#include <coresystem/cs_param.hpp>

#include "config.hpp"
#include "utils.hpp"
#include "names.hpp"
#include "apply.hpp"
#include "discover.hpp"

namespace iwb {

// ---- worker thread (param load blocks; never do that in DllMain)
DWORD WINAPI run(LPVOID) {
    Config cfg = load_config();
    // Readable logs: parse the embedded Paramdex names (+ optional loose
    // overrides next to the DLL) before we start logging affected effects.
    load_names();

    try {
        flog("waiting for params...");
        from::CS::SoloParamRepository::wait_for_params(-1);
        flog("params ready -- applying edits");
        if (cfg.ini.get_bool("logging", "dump", false)) {
            flog("DISCOVER MODE ON -- dumping candidates (durations not applied)");
            dump_candidates(cfg.extraGoods, cfg.horseGoods, cfg.ashIds, cfg.artPairs);
        }
        apply(cfg.ini, cfg.extraGoods, cfg.horseGoods, cfg.ashIds,
              cfg.extraSpeffects, cfg.artPairs);
        flog("done.");
    } catch (const std::exception& e) {
        flog("[ERROR] exception: %s", e.what());
    } catch (...) {
        flog("[ERROR] unknown exception while applying edits");
    }
    return 0;
}

} // namespace iwb

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        iwb::g_hinst = hinst;
        DisableThreadLibraryCalls(hinst);
        // Fresh log each launch + hard proof the DLL actually loaded.
        iwb::log_line("==== InfiniteWeaponBuffs loaded (DllMain attach) ====",
                 /*truncate=*/true);
        CreateThread(nullptr, 0, iwb::run, nullptr, 0, nullptr);
    }
    return TRUE;
}
