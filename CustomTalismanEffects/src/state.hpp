#pragma once

// Shared, mutex-guarded model of which talismans are enabled + the overlay
// options. Written by the ImGui overlay (user clicks) and read by the worker
// loop (which applies/removes the SpEffects). Also the source the overlay's
// "Save" writes back to the .ini.

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cte {

struct Talisman {
    int              accessory_id = 0;
    std::string      name;                 // display name (live FMG, else baked) -- .ini key
    std::string      effect;               // human-readable effect (overlay hover text)
    int              group = -1;           // accessoryGroup; -1 == no family
    int              sort_id = 0;          // sortId (EquipParamAccessory) == in-game menu order
    std::vector<int> sp_ids;               // resident SpEffect ids (>0)
    bool             enabled = false;      // desired on/off
    bool             is_base = true;       // id is in the baked base-game table (Base vs Mod-Added tab)
};

struct State {
    std::vector<Talisman> talismans;

    bool allow_stacking = false;           // ignore talisman families when set

    // Progression Mode: only display + apply talismans the player currently
    // owns in their inventory, so the usable pool grows as they find talismans.
    bool progression_mode = false;
    // Accessory ids the player currently possesses (refreshed by the worker loop
    // while progression_mode is on). `possessed_valid` stays false until the
    // first good inventory read, so the gate "fails open" -- if GameDataMan is
    // unresolved or the bag hasn't been read, nothing is hidden/suppressed.
    std::unordered_set<int> possessed_accessories;
    bool possessed_valid = false;

    // Overlay open/close inputs (configurable in the .ini).
    unsigned int   open_vk       = 0x2D;   // VK_INSERT
    unsigned short open_pad_mask = 0x00C0; // XINPUT LEFT_THUMB | RIGHT_THUMB (L3+R3)
    bool           open_pad_is_hold = false; // true if toggle_gamepad_combo had a HOLD_ prefix
    // Human-readable labels of the above (the raw .ini strings), for the
    // overlay's hotkey-hint footer.
    std::string open_key_label = "Insert";
    std::string open_pad_label = "L3+R3";

    // Whole-overlay scale: 0 = auto (derived from the game's resolution, so 4K
    // and ultrawide canvases get a proportionally larger panel), else a forced
    // 0.5-2.0 from [overlay] ui_scale.
    float ui_scale = 0.0f;

    // Saved panel geometry ([panel] in the .ini, canvas pixels). w/h <= 0 =
    // nothing saved yet -> the overlay uses its default placement. The overlay
    // writes the live values back every open frame; saved on menu close.
    float panel_x = -1.0f;
    float panel_y = -1.0f;
    float panel_w = 0.0f;
    float panel_h = 0.0f;

    bool save_requested = false;           // overlay asked to persist to the .ini

    // ── per-character presets (session_store) ──
    // Set by the worker when a character with no saved preset loads while OTHER
    // characters already have presets: the overlay shows an inline import banner
    // and the talismans default to all-off until the player resolves it.
    bool import_prompt_active = false;
    // Candidate characters to import from: (section key, display name). Filled by
    // the worker alongside import_prompt_active; read by the overlay's banner.
    std::vector<std::pair<std::string, std::string>> import_candidates;
    // Banner -> worker request: copy this character's preset into g_state, then
    // save it under the CURRENT character. Empty key + import_requested == the
    // banner's "Start fresh" (keep all-off; just clear the prompt + save).
    std::string import_from_key;
    bool import_requested = false;

    // Hide the HUD status icons of the effects THIS MOD applies. A talisman the
    // player physically wears keeps its icon -- its ids never reach the worker's
    // `applied` set. See icons.hpp.
    bool hide_effect_icons = false;

    bool show_descriptions = true;
    int  sort_mode = 0; // 0 = Talisman ID, 1 = Name, 2 = Group (in-game order)

    // True when the loaded regulation has talismans beyond the baked base-game
    // set (an overhaul is active). Drives whether the overlay shows the
    // Base/Mod-Added tabs at all.
    bool has_mod_added = false;

    // SpEffect ids active on the player from a source OTHER than this mod
    // (i.e. an actually-equipped talisman). Published by the worker loop each
    // tick; the overlay uses it to highlight / lock already-equipped talismans.
    std::unordered_set<int> external_active;
};

// Lock this around every access to g_state.
extern std::mutex g_state_mutex;
extern State      g_state;

// --- helpers that assume g_state_mutex is ALREADY held ---------------------

// After enabling talismans[idx], enforce family exclusivity: if stacking is off
// and this talisman has a real group (>= 0), disable every other enabled
// talisman sharing that group.
void apply_exclusivity_locked(size_t idx);

// Collapse every family to at most one enabled member (used when stacking is
// turned back off, and at startup after loading the .ini).
void collapse_groups_locked();

// Re-sort g_state.talismans in place according to g_state.sort_mode
// (0 = Talisman ID, 1 = Name, 2 = In-game menu order / sortId).
void sort_talismans_locked();

} // namespace cte
