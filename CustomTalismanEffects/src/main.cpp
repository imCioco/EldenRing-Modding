// ============================================================
//  CustomTalismanEffects - apply talisman effects as passive buffs, with a
//  live in-game ImGui overlay to toggle them.
//
//  EquipParamAccessory IS the talisman table; each row's residentSpEffectId1..4
//  are the effects the game applies while the talisman is worn. This mod keeps
//  the enabled talismans' resident effects present on the player -- reproducing
//  an equipped talisman without using a talisman slot.
//
//  The worker loop diffs the desired effects (from the shared state, driven by
//  the .ini + the overlay) against the player's live SpEffect list: newly
//  desired -> ApplySpEffect; newly un-desired -> RemoveSpEffect. Family
//  exclusivity (accessoryGroup) and the global "allow stacking" toggle are
//  enforced in the state layer. Run OFFLINE only (EasyAntiCheat).
// ============================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Xinput.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// libER
#include <coresystem/cs_param.hpp>
#include <param/param.hpp>

#include "game_access.hpp"
#include "hooks.hpp"
#include "ini.hpp"
#include "log.hpp"
#include "messages.hpp"
#include "offsets.hpp"
#include "overlay.hpp"
#include "session_store.hpp"
#include "state.hpp"
#include "talisman_names.hpp"

namespace cte {
namespace {

bool g_log_each = false;
constexpr DWORD kPollMs = 200;

// ---- name matching (must mirror gen_talisman_data.py norm()) ----
std::string normalize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool in_space = true;
    for (unsigned char c : s) {
        if (std::isspace(c)) {
            if (!in_space) { out.push_back(' '); in_space = true; }
        } else {
            out.push_back(static_cast<char>(std::tolower(c)));
            in_space = false;
        }
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// ---- .ini key parsing for the overlay hotkeys ----
unsigned int parse_open_key(const std::string& raw, unsigned int def) {
    std::string k = normalize(raw);
    if (k.empty()) return def;
    if (k.size() == 1) {
        char c = static_cast<char>(std::toupper(static_cast<unsigned char>(k[0])));
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return static_cast<unsigned int>(c);
        if (k[0] == '`') return VK_OEM_3;
    }
    if (k[0] == 'f' && k.size() >= 2) { // F1..F24
        int n = std::atoi(k.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + (n - 1);
    }
    if (k == "insert" || k == "ins") return VK_INSERT;
    if (k == "delete" || k == "del") return VK_DELETE;
    if (k == "home") return VK_HOME;
    if (k == "end") return VK_END;
    if (k == "pageup" || k == "pgup" || k == "prior") return VK_PRIOR;
    if (k == "pagedown" || k == "pgdn" || k == "next") return VK_NEXT;
    if (k == "backtick" || k == "grave" || k == "tilde") return VK_OEM_3;
    if (k == "tab") return VK_TAB;
    if (k == "space") return VK_SPACE;
    if (k == "pause") return VK_PAUSE;
    if (k == "scrolllock" || k == "scroll") return VK_SCROLL;
    flog("[WARN] unrecognized open_key \"%s\"; using default", raw.c_str());
    return def;
}

unsigned short parse_pad_mask(const std::string& raw, unsigned short def) {
    const std::string norm = normalize(raw);
    if (norm.empty()) return def;
    // Explicit opt-out: "none"/"off"/"disabled"/"disable" turns the gamepad combo
    // off entirely (0 == no button can ever satisfy the combo check), instead of
    // falling back to the default combo like an empty/unrecognized value does.
    if (norm == "none" || norm == "off" || norm == "disabled" || norm == "disable")
        return 0;
    unsigned short mask = 0;
    std::stringstream ss(raw);
    std::string tok;
    while (std::getline(ss, tok, '+')) {
        std::string t = normalize(tok);
        if (t == "l3" || t == "ls" || t == "lthumb" || t == "leftthumb") mask |= XINPUT_GAMEPAD_LEFT_THUMB;
        else if (t == "r3" || t == "rs" || t == "rthumb" || t == "rightthumb") mask |= XINPUT_GAMEPAD_RIGHT_THUMB;
        else if (t == "lb" || t == "l1" || t == "leftshoulder") mask |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        else if (t == "rb" || t == "r1" || t == "rightshoulder") mask |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        else if (t == "a") mask |= XINPUT_GAMEPAD_A;
        else if (t == "b") mask |= XINPUT_GAMEPAD_B;
        else if (t == "x") mask |= XINPUT_GAMEPAD_X;
        else if (t == "y") mask |= XINPUT_GAMEPAD_Y;
        else if (t == "start") mask |= XINPUT_GAMEPAD_START;
        else if (t == "back" || t == "select") mask |= XINPUT_GAMEPAD_BACK;
        else if (t == "up" || t == "dpadup") mask |= XINPUT_GAMEPAD_DPAD_UP;
        else if (t == "down" || t == "dpaddown") mask |= XINPUT_GAMEPAD_DPAD_DOWN;
        else if (t == "left" || t == "dpadleft") mask |= XINPUT_GAMEPAD_DPAD_LEFT;
        else if (t == "right" || t == "dpadright") mask |= XINPUT_GAMEPAD_DPAD_RIGHT;
        else if (!t.empty()) flog("[WARN] unrecognized pad button \"%s\" in open_pad_mask", tok.c_str());
    }
    return mask ? mask : def;
}

// Strips a case-insensitive "HOLD_"/"HOLD "/"HOLD-" prefix from a gamepad combo
// string, e.g. "HOLD_R3" -> rest = "R3", returns true. The remainder still flows
// through parse_pad_mask's own per-token normalization, so this operates on the
// raw (un-normalized) string. A HOLD_ combo must be held ~1s to toggle the menu
// (vs an instant tap), so an in-game combo like R3 doesn't also fire the menu.
bool strip_hold_prefix(const std::string& raw, std::string& rest) {
    size_t start = raw.find_first_not_of(" \t");
    if (start == std::string::npos || raw.size() < start + 5) return false;
    std::string head = raw.substr(start, 4);
    for (auto& ch : head) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    char sep = raw[start + 4];
    if (head != "hold" || (sep != '_' && sep != ' ' && sep != '-')) return false;
    rest = raw.substr(start + 5);
    return true;
}

// Enabled set + options parsed from the .ini (before params resolve).
struct IniConfig {
    std::unordered_set<std::string> enabled; // normalized talisman names set to 1
    bool           allow_stacking = false;
    bool           progression_mode = false;
    unsigned int   open_vk = VK_INSERT;
    unsigned short open_pad_mask = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
    bool           open_pad_is_hold = false;
    std::string    open_key_label = "Insert"; // raw .ini strings, for the overlay footer
    std::string    open_pad_label = "L3+R3";
    bool show_descriptions = true;
    int  sort_mode = 0;
    float ui_scale = 0.0f;                  // 0 = auto (from the game's resolution)
    // Saved overlay panel geometry ([panel]); w/h <= 0 = nothing saved yet.
    float panel_x = -1.0f, panel_y = -1.0f, panel_w = 0.0f, panel_h = 0.0f;
};

IniConfig load_config(const Ini& ini) {
    IniConfig c;
    c.allow_stacking = ini.get_bool("overlay", "allow_stacking", false);
    c.progression_mode = ini.get_bool("overlay", "progression_mode", false);
    c.open_key_label = ini.get_string("overlay", "toggle_key", "Insert");
    c.open_pad_label = ini.get_string("overlay", "toggle_gamepad_combo", "L3+R3");
    c.open_vk = parse_open_key(c.open_key_label, VK_INSERT);
    std::string combo_rest;
    c.open_pad_is_hold = strip_hold_prefix(c.open_pad_label, combo_rest);
    c.open_pad_mask = parse_pad_mask(
        c.open_pad_is_hold ? combo_rest : c.open_pad_label,
        XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB);
    c.show_descriptions = ini.get_bool("overlay", "show_descriptions", true);
    c.sort_mode = ini.get_int("overlay", "sort_mode", 0);
    const float uis = ini.get_float("overlay", "ui_scale", 0.0f);
    c.ui_scale = uis > 0.0f ? std::clamp(uis, 0.5f, 2.0f) : 0.0f;
    // Saved panel geometry (written by the overlay on close; see save_config).
    c.panel_x = ini.get_float("panel", "x", -1.0f);
    c.panel_y = ini.get_float("panel", "y", -1.0f);
    c.panel_w = ini.get_float("panel", "width", 0.0f);
    c.panel_h = ini.get_float("panel", "height", 0.0f);
    for (const auto& kv : ini.section_items("talismans"))
        if (Ini::as_bool(kv.second))
            c.enabled.insert(normalize(kv.first));
    return c;
}

// id -> baked table entry (curated name + effect). Built once. Doubles as the
// set of "base game" ids used to split the overlay into Base vs Mod-Added.
const std::unordered_map<int, const TalismanEntry*>& baked_by_id() {
    static const std::unordered_map<int, const TalismanEntry*> m = [] {
        std::unordered_map<int, const TalismanEntry*> t;
        t.reserve(kTalismanCount);
        for (const auto& e : kTalismans) t.emplace(e.id, &e);
        return t;
    }();
    return m;
}

// EquipParamAccessory rows to never surface: cut / unused content that isn't
// obtainable in-game. 6100 "Entwining Umbilical Cord" is a cut duplicate whose
// live name resolves to a broken "[ERROR]..." string (and, being unmatched to the
// real .ini entry, otherwise gets re-appended on every save).
const std::unordered_set<int> kBlacklistedAccessories = { 6100 };

// After params load, build the shared talisman model by walking EVERY row of the
// live EquipParamAccessory -- so mod-added talismans are picked up automatically,
// not just the baked base-game set. For each row we read accessoryGroup +
// resident/refId SpEffects live, and resolve the display NAME (and, as a
// fallback, the effect text) from the game's live message repository so renamed /
// repurposed talismans (Reforged, The Convergence) show correctly. Enabled flag
// comes from the .ini (keyed by name).
void build_state(const IniConfig& cfg) {
    // Live talisman names/effects from the game's message repository (mod-aware).
    // Feed it the full live accessory id set -- it fingerprints the description
    // FMG slots by matching their resolving-string id sets against these ids.
    std::vector<int> all_ids;
    for (auto [pid, row] : from::param::EquipParamAccessory) {
        (void)row;
        all_ids.push_back(static_cast<int>(pid));
    }
    const bool live = messages::init(all_ids);

    std::lock_guard<std::mutex> lk(g_state_mutex);
    g_state.allow_stacking = cfg.allow_stacking;
    g_state.progression_mode = cfg.progression_mode;
    g_state.open_vk = cfg.open_vk;
    g_state.open_pad_mask = cfg.open_pad_mask;
    g_state.open_pad_is_hold = cfg.open_pad_is_hold;
    g_state.open_key_label = cfg.open_key_label;
    g_state.open_pad_label = cfg.open_pad_label;
    g_state.show_descriptions = cfg.show_descriptions;
    g_state.sort_mode = cfg.sort_mode;
    g_state.ui_scale = cfg.ui_scale;
    g_state.panel_x = cfg.panel_x;
    g_state.panel_y = cfg.panel_y;
    g_state.panel_w = cfg.panel_w;
    g_state.panel_h = cfg.panel_h;
    g_state.talismans.clear();

    const auto& baked = baked_by_id();

    // A talisman's effect lives in one (or both) of two places on its accessory
    // row: `refId` -- the SpEffect it grants while worn (the common case; only
    // skipped when refCategory==1, which is a Bullet, not a SpEffect) -- and
    // `residentSpEffectId1..4` (a rarer "extra resident effect" slot). We take the
    // union so every talisman that references a real SpEffect resolves.
    auto sp_exists = [](int id) -> bool {
        if (id <= 0) return false;
        auto [sr, sok] = from::param::SpEffectParam[id];
        (void)sr;
        return sok;
    };

    int noeffect = 0, noname = 0, from_ref = 0, from_resident = 0, mod_added = 0;
    for (auto [pid, row] : from::param::EquipParamAccessory) {
        Talisman t;
        t.accessory_id = static_cast<int>(pid);
        if (kBlacklistedAccessories.count(t.accessory_id)) continue; // cut content
        t.group = row.accessoryGroup;
        t.sort_id = row.sortId;

        auto add_id = [&](int id) {
            if (!sp_exists(id)) return false;
            if (std::find(t.sp_ids.begin(), t.sp_ids.end(), id) == t.sp_ids.end())
                t.sp_ids.push_back(id);
            return true;
        };

        bool ref_hit = (row.refCategory != 1) && add_id(row.refId);
        bool res_hit = false;
        for (int sp : { row.residentSpEffectId1, row.residentSpEffectId2,
                        row.residentSpEffectId3, row.residentSpEffectId4 })
            if (add_id(sp)) res_hit = true;

        if (t.sp_ids.empty()) { ++noeffect; continue; } // not a passive-buff talisman

        const auto it = baked.find(t.accessory_id);
        t.is_base = it != baked.end();

        // Name: live FMG (correct for any mod / rename) -> baked -> skip the row.
        // A row with neither a live nor a baked name is a template/system entry.
        if (live) t.name = messages::accessory_name(t.accessory_id);
        if (t.name.empty() && it != baked.end()) t.name = it->second->name;
        if (t.name.empty()) { ++noname; continue; }
        // A live name that resolves to the game's "[ERROR]" placeholder marks a
        // broken/cut row; drop it (a leading '[' would also be misread as a
        // section header when written to the .ini). Prefer the baked name if any.
        if (t.name.rfind("[ERROR]", 0) == 0) {
            if (it != baked.end() && it->second->name[0] != '\0') t.name = it->second->name;
            else { ++noname; continue; }
        }

        // Effect: curated baked text preferred; else live AccessoryInfo/Caption.
        if (it != baked.end() && it->second->effect[0] != '\0')
            t.effect = it->second->effect;
        else if (live)
            t.effect = messages::accessory_effect(t.accessory_id);

        if (ref_hit) ++from_ref;
        if (res_hit) ++from_resident;
        if (!t.is_base) ++mod_added;

        t.enabled = cfg.enabled.count(normalize(t.name)) != 0;
        g_state.talismans.push_back(std::move(t));
    }

    g_state.has_mod_added = mod_added > 0;

    // With an overhaul loaded, it typically also retunes VANILLA talismans, so
    // our baked descriptions can go stale. Read EVERY description live from the
    // game in that case (keeping the baked text only where the game has none).
    // On the plain vanilla game we keep the curated baked descriptions.
    if (live && mod_added > 0) {
        for (auto& t : g_state.talismans) {
            std::string le = messages::accessory_effect(t.accessory_id);
            if (!le.empty()) t.effect = std::move(le);
        }
    }

    if (!cfg.allow_stacking) {
        collapse_groups_locked(); // honor exclusivity for the loaded .ini selection /+ unless stacking option is enabled.
    }
    sort_talismans_locked();
    flog("built %zu talismans (%d mod-added, %d without effects, %d unnamed; "
         "%d have a refId effect, %d have resident effect(s))",
         g_state.talismans.size(), mod_added, noeffect, noname, from_ref, from_resident);
}

// Rewrite the managed GLOBAL [overlay] option values in place (show_descriptions,
// sort_mode -- the only ones editable from the overlay), preserving comments /
// formatting; only touches the value after '='. Talisman selections + the
// allow_stacking / progression_mode toggles are PER-CHARACTER now (session_store,
// CustomTalismanEffects.state.ini) and are NOT written here -- the [talismans]
// block + those two toggles stay as the human-authored default template / first-
// character seed. Also self-migrates any managed [overlay] option missing from an
// older .ini. Called (alongside presets_save) when the overlay requests a save.
void save_config() {
    const std::wstring path = config_path();
    std::ifstream in(path);
    if (!in) { flog("[WARN] save: cannot open .ini for read"); return; }

    bool show_descriptions;
    int  sort_mode;
    float ui_scale, px, py, pw, ph;
    std::string open_key_label, open_pad_label;
    {
        std::lock_guard<std::mutex> lk(g_state_mutex);
        show_descriptions = g_state.show_descriptions;
        sort_mode = g_state.sort_mode;
        ui_scale = g_state.ui_scale;
        open_key_label = g_state.open_key_label;
        open_pad_label = g_state.open_pad_label;
        px = g_state.panel_x; py = g_state.panel_y;
        pw = g_state.panel_w; ph = g_state.panel_h;
    }
    char ui_scale_val[16];
    std::snprintf(ui_scale_val, sizeof(ui_scale_val), "%.2f", ui_scale);

    // Panel geometry: only persisted once the overlay has actually been open
    // (w/h stay 0 until then -- don't write a bogus zero-sized panel).
    const bool have_panel = pw > 0.0f && ph > 0.0f;
    const char* pkeys[4] = {"x", "y", "width", "height"};
    char pvals[4][16];
    std::snprintf(pvals[0], sizeof(pvals[0]), "%.0f", px);
    std::snprintf(pvals[1], sizeof(pvals[1]), "%.0f", py);
    std::snprintf(pvals[2], sizeof(pvals[2]), "%.0f", pw);
    std::snprintf(pvals[3], sizeof(pvals[3]), "%.0f", ph);
    bool pwrote[4] = {};
    bool panel_seen = false;

    // Managed [overlay] options for self-migration (append any missing from an
    // older .ini so a new DLL version self-heals the file). allow_stacking /
    // progression_mode are seeded OFF here -- their live value is per-character,
    // so they are migrate-only defaults, never value-rewritten below.
    struct OverlayOpt { const char* key; std::string value; const char* comment; };
    const std::vector<OverlayOpt> overlay_opts = {
        {"toggle_key", open_key_label, "; key to open/close the in-game panel"},
        {"toggle_gamepad_combo", open_pad_label,
         "; controller combo to open/close (buttons joined with +, prefix HOLD_ to require a ~1s hold, or \"none\")"},
        {"allow_stacking", "0",
         "; default for NEW characters: 1 = ignore talisman families (stack anything)"},
        {"progression_mode", "0",
         "; default for NEW characters: 1 = only show/apply talismans you currently own"},
        {"show_descriptions", show_descriptions ? "1" : "0",
         "; 1 = show the effect description pane at the bottom of the overlay"},
        {"sort_mode", std::to_string(sort_mode),
         "; overlay list order: 0 = Talisman ID, 1 = Name (A-Z), 2 = In-game menu order"},
        {"ui_scale", ui_scale_val,
         "; overlay size: 0 = auto (scales with your resolution -- 1440p and up), or force 0.50 - 2.00"},
    };

    // Options this DLL version has RETIRED. Any line assigning one of these --
    // in any section -- is dropped on the next save, along with the indented
    // comment block that documents it, so an .ini carried over from an older
    // version self-heals instead of keeping a key nothing reads any more.
    const std::unordered_set<std::string> retired_keys = {normalize("focus_input")};
    bool dropped_retired = false;
    // Set while skipping the retired key's trailing ";" continuation comments.
    bool eating_retired_comment = false;

    std::vector<std::string> out;
    std::unordered_set<std::string> overlay_present; // normalized [overlay] keys in the file
    int overlay_end = -1;                            // out index just after the last [overlay] key line
    int panel_end = -1;                              // ...same, for [panel]
    std::string line, section;
    auto is_blank = [](const std::string& s) {
        return s.find_first_not_of(" \t\r") == std::string::npos;
    };
    while (std::getline(in, line)) {
        std::string trimmed = line;
        // find section headers (ignoring leading spaces / trailing comments)
        size_t a = trimmed.find_first_not_of(" \t");
        const bool is_header = a != std::string::npos && trimmed[a] == '[';
        if (is_header) {
            size_t close = trimmed.find(']', a);
            if (close != std::string::npos) {
                section = normalize(trimmed.substr(a + 1, close - a - 1));
                if (section == "panel") panel_seen = true;
            }
        }

        // A retired key's documentation block trails the key line (the format
        // this .ini uses throughout), so keep dropping indented ";" comment
        // lines until the block ends.
        if (eating_retired_comment) {
            const bool comment_line =
                a != std::string::npos && (trimmed[a] == ';' || trimmed[a] == '#');
            if (comment_line && !is_header) continue;
            eating_retired_comment = false;
            // The removed block usually leaves the blank line that separated it
            // behind; don't let deletions accumulate blank lines.
            if (is_blank(line) && !out.empty() && is_blank(out.back())) continue;
        }

        const size_t eq = line.find('=');
        std::string key_norm;
        if (eq != std::string::npos && !is_header) {
            std::string key = line.substr(0, eq);
            size_t ks = key.find_first_not_of(" \t");
            size_t ke = key.find_last_not_of(" \t");
            key_norm = (ks == std::string::npos) ? "" : normalize(key.substr(ks, ke - ks + 1));
        }
        if (!key_norm.empty() && retired_keys.count(key_norm)) {
            // Drop the assignment wherever it appears, then its comment block.
            eating_retired_comment = true;
            dropped_retired = true;
            // A blank line we already emitted directly above the removed key
            // would otherwise double up with the one following its comments.
            if (!out.empty() && is_blank(out.back())) out.pop_back();
            continue;
        }

        if (eq != std::string::npos && (section == "overlay" || section == "panel")) {
            // split key / (value + optional comment)
            std::string key = line.substr(0, eq);
            std::string rest = line.substr(eq + 1);
            size_t hash = rest.find_first_of(";#");
            std::string comment = (hash == std::string::npos) ? "" : rest.substr(hash);
            auto rewrite = [&](const std::string& v) {
                line = key + "= " + v + (comment.empty() ? "" : " " + comment);
            };
            if (section == "overlay") {
                overlay_present.insert(key_norm);
                // Only these are editable from the overlay; everything else
                // (hotkeys, the per-character seed toggles) is left as authored.
                if (key_norm == "show_descriptions") rewrite(show_descriptions ? "1" : "0");
                else if (key_norm == "sort_mode") rewrite(std::to_string(sort_mode));
                else if (key_norm == "ui_scale") rewrite(ui_scale_val);
            } else if (have_panel) { // [panel]: the overlay owns every value
                for (int i = 0; i < 4; ++i)
                    if (key_norm == pkeys[i]) { rewrite(pvals[i]); pwrote[i] = true; }
            }
        }
        out.push_back(line);
        // Remember where each managed section ends, so anything missing can be
        // inserted with it rather than at EOF. [panel] tracks EVERY line of the
        // section, not just assignments: a freshly shipped .ini has the header
        // and its comments but no keys yet, and they still have to land there.
        if (eq != std::string::npos && section == "overlay")
            overlay_end = static_cast<int>(out.size());
        if (section == "panel") panel_end = static_cast<int>(out.size());
    }
    in.close();

    // Self-migration: append any managed [overlay] option missing from the file
    // (e.g. a new option shipped in a DLL update) with its current/default value,
    // so upgrading players keep their settings and just gain the new line.
    std::vector<std::string> new_opts;
    for (const auto& o : overlay_opts)
        if (!overlay_present.count(normalize(o.key)))
            new_opts.push_back(std::string(o.key) + " = " + o.value + " " + o.comment);
    if (!new_opts.empty()) {
        if (overlay_end < 0 || overlay_end > static_cast<int>(out.size())) { // no [overlay] section
            out.push_back("");
            out.push_back("[overlay]");
            overlay_end = static_cast<int>(out.size());
        }
        out.insert(out.begin() + overlay_end, new_opts.begin(), new_opts.end());
        // That insertion shifted every later line, so a [panel] section below
        // [overlay] must have its recorded end moved with it -- otherwise the
        // panel keys below land N lines too early (inside another section's
        // block).
        if (panel_end >= overlay_end)
            panel_end += static_cast<int>(new_opts.size());
        flog("added %zu missing [overlay] option(s) to the .ini", new_opts.size());
    }

    // Panel geometry. Existing keys were rewritten in place above; fill in any
    // the file is missing, and append the whole section for .ini files from
    // before the overlay remembered its size/position.
    if (have_panel) {
        if (panel_seen) {
            std::vector<std::string> missing;
            for (int i = 0; i < 4; ++i)
                if (!pwrote[i]) missing.push_back(std::string(pkeys[i]) + " = " + pvals[i]);
            if (!missing.empty() && panel_end >= 0 &&
                panel_end <= static_cast<int>(out.size())) {
                // Insert above any blank separator, so the keys sit with their
                // section rather than glued to the next one.
                size_t at = static_cast<size_t>(panel_end);
                while (at > 0 && is_blank(out[at - 1])) --at;
                out.insert(out.begin() + static_cast<ptrdiff_t>(at),
                           missing.begin(), missing.end());
            }
        } else {
            out.push_back("");
            out.push_back("[panel]");
            out.push_back("; Overlay panel size & position (pixels, relative to the game window),");
            out.push_back("; saved automatically when the overlay closes. Delete this section to");
            out.push_back("; reset the panel to its default size and placement.");
            for (int i = 0; i < 4; ++i)
                out.push_back(std::string(pkeys[i]) + " = " + pvals[i]);
        }
    }

    if (dropped_retired)
        flog("removed retired option(s) from the .ini (focus_input)");

    std::ofstream of(path, std::ios::trunc);
    if (!of) { flog("[WARN] save: cannot open .ini for write"); return; }
    for (const auto& l : out) of << l << '\n';
    flog("saved overlay options to .ini");
}

// SEH-guarded raw byte scan over the .text section. Another mod patching
// code concurrently can leave torn bytes; the scan must never crash the game.
// `data`/`data_len` are the image span; `pat`/`pat_len` the parsed pattern
// (-1 == wildcard). POD-only so __try is legal.
uintptr_t seh_raw_scan(const uint8_t* data, size_t data_len,
                       const int* pat, size_t pat_len,
                       uintptr_t base, bool* multiple) {
    if (multiple) *multiple = false;
    __try {
        uintptr_t first = 0;
        for (size_t i = 0; i + pat_len <= data_len; ++i) {
            bool ok = true;
            for (size_t j = 0; j < pat_len; ++j)
                if (pat[j] != -1 && data[i + j] != pat[j]) { ok = false; break; }
            if (!ok) continue;
            if (!first) { first = base + i; }
            else { if (multiple) *multiple = true; break; }
        }
        return first;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Parse an AOB string and run the SEH-guarded scan.
uintptr_t safe_aob_scan(const mem::Module& m, const char* aob, bool* multiple) {
    if (!m.base || !m.size) return 0;
    std::vector<int> bytes;
    std::istringstream ss(aob);
    std::string tok;
    while (ss >> tok) {
        if (tok == "??" || tok == "?") bytes.push_back(-1);
        else bytes.push_back(static_cast<int>(std::stoul(tok, nullptr, 16)));
    }
    if (bytes.empty()) return 0;
    return seh_raw_scan(reinterpret_cast<const uint8_t*>(m.base), m.size,
                        bytes.data(), bytes.size(), m.base, multiple);
}

// Resolve a game function by AOB. Returns the entry (hit - backset) or 0.
uintptr_t resolve_fn(const char* aob, uintptr_t backset, const char* name) {
    bool multiple = false;
    const uintptr_t hit = safe_aob_scan(g_mod, aob, &multiple);
    if (hit && !multiple) {
        flog("%s resolved: entry=%p", name, reinterpret_cast<void*>(hit - backset));
        return hit - backset;
    }
    if (multiple) flog("[ERROR] %s AOB matched MULTIPLE sites -- refusing to use it", name);
    else          flog("[ERROR] %s AOB not found -- game version mismatch?", name);
    return 0;
}

// Resolve a global POINTER VARIABLE referenced by a `mov reg,[rip+disp32]` (disp32
// at instruction offset 3, instruction length 7). Unlike resolve_fn this returns
// the address of the variable, not a function entry. Returns 0 if not found/unique.
uintptr_t resolve_global_ptr(const char* aob, const char* name) {
    bool multiple = false;
    const uintptr_t hit = safe_aob_scan(g_mod, aob, &multiple);
    if (!hit || multiple) {
        flog("[WARN] %s AOB %s", name, multiple ? "matched MULTIPLE sites" : "not found");
        return 0;
    }
    const uintptr_t var = mem::rip_relative(hit, 3, 7);
    flog("%s resolved: var=%p", name, reinterpret_cast<void*>(var));
    return var;
}

// Called when the resolved character key changes (including the first load of a
// session). Applies that character's preset: load an existing one; seed the very
// first character ever from the current .ini selections; or, for a later new
// character, blank the selections and raise the import prompt.
void on_character_changed(const std::string& key, const std::wstring& name) {
    std::lock_guard<std::mutex> lk(g_state_mutex);
    if (presets_has(key)) {
        presets_load_into(key);
        g_state.import_prompt_active = false;
        g_state.import_candidates.clear();
        flog("presets: character '%s' -- loaded saved preset", key.c_str());
    } else if (presets_any()) {
        presets_clear_state(); // all off; the player picks via the import banner
        g_state.import_prompt_active = true;
        g_state.import_candidates = presets_list();
        flog("presets: character '%s' -- no preset; import prompt (%zu candidate(s))",
             key.c_str(), g_state.import_candidates.size());
    } else {
        // Very first character ever: adopt the current .ini selections as its
        // preset and persist now, so the NEXT character sees a non-empty store.
        presets_save(key, name);
        g_state.import_prompt_active = false;
        g_state.import_candidates.clear();
        flog("presets: character '%s' -- first character; seeded from .ini", key.c_str());
    }
}

// The apply/remove diff loop.
void run_loop() {
    std::vector<int> active;
    std::unordered_set<int> active_set;
    std::unordered_set<int> applied;   // effects WE applied (ours to remove)
    bool warned_no_remove = false;

    // Per-character preset tracking. last_char_key is the currently-loaded
    // character's key (empty = none resolved yet / name-read failed); last_char_name
    // is its raw display name (for saves). A failed name read keeps the previous
    // key so a transient bad read never looks like a character switch.
    std::string  last_char_key;
    std::wstring last_char_name;

    // Tug-of-war guard. Some mods force-remove SpEffects they consider theirs
    // (erdGameTools strips e.g. 330600/360400 every ~150ms while those features
    // are toggled off in ITS menu). Re-applying forever makes the buff icon
    // flicker endlessly, so keep a leaky score per effect: +1 each tick an
    // effect we applied (and still want) went missing, -1 each tick it
    // survived. Past the limit we stop re-applying it for this session.
    std::unordered_map<int, int> contested; // id -> leaky tug-of-war score
    std::unordered_set<int> abandoned;      // ids we gave up re-applying
    constexpr int kContestedLimit = 8;

    for (;;) {
        // ── per-character presets: detect a character load / switch ──
        // get_character_name() returns "" at the main menu / on a failed read, so
        // this only fires once a character is actually loaded. On a real key change
        // (incl. the first load) we load/seed the preset or raise the import prompt.
        {
            const std::wstring name = get_character_name();
            const bool name_ok = !name.empty();
            const std::string key = name_ok ? char_key(name) : last_char_key;
            if (name_ok && key != last_char_key) {
                flog("characters: key='%s' (character loaded)", key.c_str());
                on_character_changed(key, name);
                last_char_key = key;
                last_char_name = name;
            }
        }

        // Progression Mode: refresh the possessed-talisman set before diffing so
        // the gate below reflects the current inventory. A failed read keeps the
        // previous set (and possessed_valid), so a transient miss can't wrongly
        // wipe out every effect.
        bool progression = false;
        {
            std::lock_guard<std::mutex> lk(g_state_mutex);
            progression = g_state.progression_mode;
        }
        if (progression) {
            std::vector<int> owned;
            if (enumerate_inventory_accessories(owned)) {
                std::lock_guard<std::mutex> lk(g_state_mutex);
                g_state.possessed_accessories.clear();
                g_state.possessed_accessories.insert(owned.begin(), owned.end());
                g_state.possessed_valid = true;
            }
        }

        // Snapshot desired effects + drain save/import requests under the lock. In
        // Progression Mode, suppress talismans the player doesn't currently own
        // (gate fails open until the first good inventory read).
        std::unordered_set<int> desired;
        bool do_save = false;
        bool do_import = false;
        std::string import_src;
        {
            std::lock_guard<std::mutex> lk(g_state_mutex);
            const bool gate = g_state.progression_mode && g_state.possessed_valid;
            for (const auto& t : g_state.talismans) {
                if (!t.enabled) continue;
                if (gate && !g_state.possessed_accessories.count(t.accessory_id)) continue;
                for (int sp : t.sp_ids) desired.insert(sp);
            }
            if (g_state.save_requested) { g_state.save_requested = false; do_save = true; }
            if (g_state.import_requested) {
                g_state.import_requested = false;
                do_import = true;
                import_src = g_state.import_from_key;
                g_state.import_from_key.clear();
            }
        }

        // Import banner resolution ("Import from X" / "Start fresh"): apply the
        // chosen preset (empty src == start fresh, keep the all-off state) and
        // persist it as THIS character's preset so the prompt doesn't recur.
        if (do_import) {
            std::lock_guard<std::mutex> lk(g_state_mutex);
            const bool imported = !import_src.empty() && presets_import_into(import_src);
            g_state.import_prompt_active = false;
            g_state.import_candidates.clear();
            presets_save(last_char_key, last_char_name);
            if (imported)
                flog("presets: imported '%s' into character '%s'",
                     import_src.c_str(), last_char_key.c_str());
            else
                flog("presets: character '%s' started fresh", last_char_key.c_str());
            // Re-snapshot desired now that selections changed, so the import
            // takes effect this tick instead of next.
            desired.clear();
            const bool gate = g_state.progression_mode && g_state.possessed_valid;
            for (const auto& t : g_state.talismans) {
                if (!t.enabled) continue;
                if (gate && !g_state.possessed_accessories.count(t.accessory_id)) continue;
                for (int sp : t.sp_ids) desired.insert(sp);
            }
        }

        // Save request (Save button / progression toggle / menu close): persist
        // this character's preset (enabled ids + the two toggles) + the global
        // [overlay] options.
        if (do_save) {
            {
                std::lock_guard<std::mutex> lk(g_state_mutex);
                bool any_on = false;
                for (const auto& t : g_state.talismans) if (t.enabled) { any_on = true; break; }
                // Don't commit an all-off section for a character still showing the
                // import prompt (the menu was closed without choosing) -- let the
                // prompt recur next load. Any enabled talisman commits + resolves it.
                if (!(g_state.import_prompt_active && !any_on)) {
                    presets_save(last_char_key, last_char_name);
                    g_state.import_prompt_active = false;
                    g_state.import_candidates.clear();
                }
            }
            save_config(); // global [overlay] options (locks g_state itself)
        }

        const uintptr_t player = get_player_ins();
        if (!player) { Sleep(kPollMs); continue; }

        enumerate_speffects(player, active);
        active_set.clear();
        active_set.insert(active.begin(), active.end());

        // Score the tug-of-war: an effect we applied and still want that is
        // no longer on the player was removed EXTERNALLY (we only remove
        // un-desired ones). One-off vanishes (area transitions, deaths) decay
        // back to zero; only a mod stripping the effect faster than our poll
        // accumulates to the limit.
        for (int id : applied) {
            if (!desired.count(id) || abandoned.count(id)) continue;
            auto it = contested.find(id);
            if (active_set.count(id)) {
                if (it != contested.end() && --it->second <= 0) contested.erase(it);
                continue;
            }
            const int score = (it == contested.end()) ? (contested[id] = 1) : ++it->second;
            if (score >= kContestedLimit) {
                abandoned.insert(id);
                flog("[WARN] SpEffect %d keeps being removed by another mod -- "
                     "giving up on re-applying it this session", id);
            }
        }

        // Apply: desired effects not currently on the player.
        if (g_apply) {
            for (int id : desired) {
                if (active_set.count(id) || abandoned.count(id)) continue;
                g_apply(reinterpret_cast<void*>(player), id, 1); // unk=1 == self
                if (applied.insert(id).second && g_log_each)
                    flog("applied SpEffect %d", id);
            }
        }

        // Remove: effects we applied that are no longer desired.
        const uintptr_t manager = mem::deref(player + kSpEffectManagerOffset);
        for (auto it = applied.begin(); it != applied.end();) {
            const int id = *it;
            if (desired.count(id)) { ++it; continue; } // still wanted
            if (active_set.count(id)) {
                if (g_remove && manager) {
                    g_remove(reinterpret_cast<void*>(manager), id);
                    if (g_log_each) flog("removed SpEffect %d", id);
                } else if (!warned_no_remove) {
                    flog("[WARN] no remove function -- disabled effects clear on next "
                         "area transition instead of instantly");
                    warned_no_remove = true;
                }
            }
            it = applied.erase(it);
        }

        // Re-read the live effect list AFTER our apply/remove so the "equipped"
        // set reflects the CURRENT state, not the stale pre-removal snapshot (which
        // wrongly flagged an effect we just removed on a family switch). Anything
        // active that we didn't apply comes from an actually-equipped talisman.
        enumerate_speffects(player, active);
        {
            std::lock_guard<std::mutex> lk(g_state_mutex);
            g_state.external_active.clear();
            for (int id : active)
                if (!applied.count(id))
                    g_state.external_active.insert(id);
        }

        Sleep(kPollMs);
    }
}

// ---- worker thread ----
DWORD WINAPI run(LPVOID) {
    // FIRST, before anything slow. The overlay backend composites into the
    // game's own D3D12 backbuffer, and the only safe way to learn the exact
    // command queue that presents it is to observe the game's CreateSwapChain*
    // call. That happens early in startup, so these (lightweight, fail-open)
    // discovery hooks must be installed before config parsing, the compat wait
    // below, or the param wait -- missing the transaction leaves the overlay
    // unavailable for the whole session. It installs hooks only; no COM, no
    // D3D, no device is created here.
    if (!overlay::bootstrap())
        flog("[WARN] graphics interception unavailable; the overlay will not be shown");

    Ini ini;
    const bool loaded = ini.load(config_path());
    g_debug    = ini.get_bool("debugging", "debug_console", false);
    g_log_each = ini.get_bool("debugging", "log_each", false);
    if (g_debug) {
        AllocConsole();
        FILE* out = nullptr;
        freopen_s(&out, "CONOUT$", "w", stdout);
    }
    flog(loaded ? "config loaded"
                : "[WARN] .ini not found next to the DLL; using defaults");

    const IniConfig cfg = load_config(ini);

    // Compatibility mode for mods with fragile startup phases (notably
    // erdGameTools: it patches game code and polls half-initialized game
    // singletons during boot -- Windows crash logs show it AV-ing in its own
    // module around the time params load, and any timing perturbation from
    // another mod can tip it over). When such a mod is present we (a) delay the
    // rest of our init so our input detours land after its DX12 hook setup, and
    // (b) later defer the heavy param/FMG work until the game world is loaded
    // (see the wait below), vacating the boot window entirely. The DXGI
    // discovery hooks above are deliberately NOT delayed: they are pure
    // observation, chain whatever is already installed, and must see the
    // swapchain creation that happens during this very window.
    // The mod loader (me3) loads DLLs in directory order, so erdGameTools may
    // load a beat AFTER us -- a single GetModuleHandle check right at startup
    // misses it. Poll for a short window to catch it regardless of load order.
    const char* compat_mod = nullptr;
    {
        constexpr DWORD kDetectPollMs    = 250;
        constexpr DWORD kDetectWindowMs  = 6000;  // catch a late-loading peer
        constexpr DWORD kSettleMs        = 12000; // let its DX12 setup finish
        constexpr const char* kCompatDlls[] = {
            "erdGameTools.dll",
        };
        for (DWORD waited = 0; waited < kDetectWindowMs && !compat_mod;
             waited += kDetectPollMs) {
            for (const char* name : kCompatDlls)
                if (GetModuleHandleA(name)) { compat_mod = name; break; }
            if (!compat_mod) Sleep(kDetectPollMs);
        }
        if (compat_mod) {
            flog("detected %s -- delaying init %lums so its DX12/patch setup settles",
                 compat_mod, static_cast<unsigned long>(kSettleMs));
            Sleep(kSettleMs);
            flog("compatibility wait done");
        }
    }

    try {
        g_mod = mem::main_module();

        // Resolve the apply / remove game functions.
        g_apply  = reinterpret_cast<ApplySpEffect_t>(
            resolve_fn(kApplySpEffectAob, kApplySpEffectFuncBackset, "ApplySpEffect"));
        g_remove = reinterpret_cast<RemoveSpEffect_t>(
            resolve_fn(kRemoveSpEffectAob, 0, "RemoveSpEffect"));
        if (!g_apply)
            flog("[ERROR] no apply function -- talismans will NOT be applied");

        // Resolve the GameDataMan global (for Progression Mode inventory reads).
        // Optional: if it fails, Progression Mode safely no-ops (gate fails open).
        g_gamedataman_var = resolve_global_ptr(kGameDataManAob, "GameDataMan");
        if (!g_gamedataman_var)
            flog("[WARN] GameDataMan unresolved -- Progression Mode + per-character presets disabled");

        // Load the per-character presets file into memory (worker-thread mirror).
        // GameDataMan is what get_character_name() reads through; without it the
        // key can't resolve and the mod stays on the global .ini selections.
        presets_startup_load();

        // Publish the .ini-configured overlay inputs/options to the shared state
        // BEFORE the (possibly long, see compat wait below) param wait, so the
        // custom toggle key works from the moment the overlay is up. build_state
        // re-applies the same values later; that's harmless.
        {
            std::lock_guard<std::mutex> lk(g_state_mutex);
            g_state.allow_stacking = cfg.allow_stacking;
            g_state.progression_mode = cfg.progression_mode;
            g_state.open_vk = cfg.open_vk;
            g_state.open_pad_mask = cfg.open_pad_mask;
            g_state.open_pad_is_hold = cfg.open_pad_is_hold;
            g_state.open_key_label = cfg.open_key_label;
            g_state.open_pad_label = cfg.open_pad_label;
            g_state.show_descriptions = cfg.show_descriptions;
            g_state.sort_mode = cfg.sort_mode;
            g_state.ui_scale = cfg.ui_scale;
            g_state.panel_x = cfg.panel_x;
            g_state.panel_y = cfg.panel_y;
            g_state.panel_w = cfg.panel_w;
            g_state.panel_h = cfg.panel_h;
        }

        // In-game overlay: focus-free input + the packet-producer thread. The
        // graphics side was already armed by overlay::bootstrap() at the top of
        // this worker; setup() adds the input detours (dinput8
        // GetDeviceState/GetDeviceData now, XInput lazily on the first
        // menu-open) and spawns the control thread. Nothing here creates a
        // window, device, or swapchain, and the game's Present/queue stay its
        // own -- safe alongside frame-gen / Special K / erdGameTools.
        overlay::setup();
        overlay::sync_open_keys(); // pick up the just-published toggle inputs

        flog("waiting for params...");
        from::CS::SoloParamRepository::wait_for_params(-1);

        // Compat mode: "params ready" is exactly when a code-patching mod like
        // erdGameTools starts applying its param edits and feature pokes -- the
        // window where its startup races crash (per Windows crash logs). Rather
        // than doing our own heavy work (full param iteration + FMG bank scan)
        // inside that window, wait until the player actually loads into the
        // world -- by then the other mod's boot phase is long over. Costs
        // nothing: talisman effects only ever apply to a loaded player anyway.
        if (compat_mod) {
            flog("compat: deferring talisman model build until the game world loads");
            while (!get_player_ins()) Sleep(500);
            Sleep(2000); // give the freshly loaded world a beat to settle
            flog("compat: world loaded -- proceeding");
        }

        flog("params ready -- building talisman model");
        build_state(cfg);
        overlay::sync_open_keys(); // setup() ran before build_state(); pick up the real toggle_key/toggle_gamepad_combo now

        // Self-heal the .ini: add any managed [overlay] option this DLL version
        // expects but the file lacks, and evict any option this version has
        // RETIRED (focus_input), wherever it sits in the file. Runs even if the
        // player never opens the overlay, so upgrading is a no-op for them;
        // save_config preserves everything else. Only when the .ini actually
        // exists (else save_config warns).
        if (loaded) {
            static const char* kOverlayKeys[] = {
                "toggle_key", "toggle_gamepad_combo", "allow_stacking",
                "progression_mode", "show_descriptions", "sort_mode", "ui_scale",
            };
            static const char* kRetiredKeys[] = {"focus_input"};
            bool needs_migration = false;
            for (const char* k : kOverlayKeys)
                if (!ini.has("overlay", k)) { needs_migration = true; break; }
            // A retired key can have been left in ANY section by a hand-edited
            // or older file -- look for it everywhere, not just [overlay].
            if (!needs_migration) {
                for (const std::string& sec : ini.section_names()) {
                    for (const char* k : kRetiredKeys)
                        if (ini.has(sec, k)) { needs_migration = true; break; }
                    if (needs_migration) break;
                }
            }
            if (needs_migration) {
                flog("migrating .ini: adding missing / removing retired [overlay] option(s)");
                save_config();
            }
        }

        flog("entering apply/remove loop");
        run_loop();
    } catch (const std::exception& e) {
        flog("[ERROR] exception: %s", e.what());
    } catch (...) {
        flog("[ERROR] unknown exception in worker");
    }
    return 0;
}

} // namespace
} // namespace cte

// The one export. A DLL with an empty export directory whose DllMain does
// nothing but CreateThread has the loader-shape of an injected payload rather
// than a library, and the AV heuristics score that -- see
// src/CustomTalismanEffects.rc.in. It is also genuinely useful: a loader or a
// sibling mod can GetProcAddress this to identify the build.
extern "C" __declspec(dllexport) const char* CustomTalismanEffectsVersion() {
    return CTE_VERSION;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        cte::g_hinst = hinst;
        DisableThreadLibraryCalls(hinst);
        cte::log_line("==== CustomTalismanEffects loaded (DllMain attach) ====",
                      /*truncate=*/true);
        CreateThread(nullptr, 0, cte::run, nullptr, 0, nullptr);
    }
    return TRUE;
}
