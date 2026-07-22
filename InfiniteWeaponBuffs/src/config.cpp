#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "config.hpp"
#include "utils.hpp"

#include <cstdio>
#include <sstream>

namespace iwb {

HINSTANCE g_hinst = nullptr;
bool      g_debug = false;

// ---- parse "130, 2003170" into a set of ints ----------------
void parse_int_list(const std::string& spec, std::unordered_set<int>& out) {
    std::stringstream ss(spec);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        const size_t a = tok.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        const size_t b = tok.find_last_not_of(" \t");
        try { out.insert(std::stoi(tok.substr(a, b - a + 1))); }
        catch (...) {}
    }
}

// ---- parse "821:823, 1821:1823" into right:left pairs -------
void parse_pair_list(const std::string& spec, std::vector<HandPair>& out) {
    std::stringstream ss(spec);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        const size_t colon = tok.find(':');
        if (colon == std::string::npos) continue;
        try {
            const int r = std::stoi(tok.substr(0, colon));
            const int l = std::stoi(tok.substr(colon + 1));
            out.push_back({ r, l });
        } catch (...) {}
    }
}

Config load_config() {
    Config cfg;
    const std::wstring cfgPath = config_path();
    const bool loaded = cfg.ini.load(cfgPath);

    g_debug = cfg.ini.get_bool("logging", "debug_console", false);
    if (g_debug) {
        AllocConsole();
        FILE* out = nullptr;
        freopen_s(&out, "CONOUT$", "w", stdout);
    }

    flog(loaded ? "config loaded"
                : "[WARN] .ini not found next to the DLL; using defaults");

    // Built-in id lists unioned with the user's allowlists.
    //   extraGoods : extra buff consumables.  horseGoods : protected horses.
    //   ashIds     : Ash-of-War buff SpEffect ids.
    for (int id : kExtraConsumableGoodsBuiltin)   cfg.extraGoods.insert(id);
    for (int id : kHorseSummonGoodsBuiltin)       cfg.horseGoods.insert(id);
    for (int id : kAshOfWarBuffSpEffectsBuiltin)  cfg.ashIds.insert(id);
    parse_int_list(cfg.ini.get_string("general", "extra_goods", ""), cfg.extraGoods);
    parse_int_list(cfg.ini.get_string("ashes_of_war", "speffect_ids", ""), cfg.ashIds);
    // Raw SpEffect ids to extend directly (NOT goods ids). See Config::extraSpeffects.
    parse_int_list(cfg.ini.get_string("consumables", "extra_speffect_ids", ""),
                   cfg.extraSpeffects);

    // Dual-wield off-hand mirror: built-in weapon-art pairs + .ini extra_pairs.
    // (Greases are paired dynamically at apply time, so none are listed here.)
    for (const auto& p : kDualWieldArtPairsBuiltin) cfg.artPairs.push_back(p);
    parse_pair_list(cfg.ini.get_string("dual_wield", "extra_pairs", ""), cfg.artPairs);

    return cfg;
}

} // namespace iwb
