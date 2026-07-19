#pragma once

// The overlay's FRONTEND: everything the player actually looks at -- the font
// stack, the ER dark/gold theme, and the talisman panel itself (import banner,
// global toggles, search, the Base/Mod-Added lists, the detail pane and the
// footer).
//
// This half is deliberately backend-agnostic: it knows nothing about DXGI,
// D3D12, raw input, or the pad detours. Each frame the backend hands it a
// Frame snapshot of the few host facts the panel needs, and gets back a Result
// describing what the player asked for. That is the ENTIRE contract between the
// two halves -- keep it that way. If the panel starts needing another host
// fact, add a field here rather than reaching across into backend globals.
//
// The panel's own model is still g_state (state.hpp), shared with the worker
// thread and locked exactly as before -- the backend split changed nothing
// about how talisman selections are read or written.

struct ImGuiIO;

namespace cte::overlay::ui {

// Backend -> UI, refreshed every frame before draw().
struct Frame {
    float canvas_w = 0.0f;          // game client area, in pixels (= ImGui canvas)
    float canvas_h = 0.0f;
    bool  pad_connected = false;    // a controller was seen by the last poll
    // Raw XINPUT_GAMEPAD::wButtons from that same poll. The panel uses it for
    // the LB/RB tab switch (edge-detected here), which predates -- and is
    // independent of -- ImGui's gamepad nav mapping.
    unsigned short pad_buttons = 0;
    unsigned short open_pad_mask = 0; // configured open/close combo, for the footer hint
};

// UI -> backend. Applied by the caller immediately after draw() returns.
struct Result {
    bool close_requested = false;   // the panel's Close button was pressed
    // True while the search InputText owns ImGui's active ID. The backend uses
    // this only for one compact per-open input diagnostic; the UI remains the
    // sole owner of search/filter state.
    bool search_active = false;
};

// Build the font atlas (Latin + Cyrillic base, every available CJK face merged
// on top). Call once, during ImGui context creation, before the first frame is
// published.
void load_fonts(ImGuiIO& io);

// Apply the ER dark/gold theme at base metrics. Call once after load_fonts;
// update_scale() re-applies it internally whenever the scale changes.
void apply_style();

// Recompute the whole-overlay scale from the canvas height (or the ini's forced
// [overlay] ui_scale) and re-apply the theme at that scale. Cheap; call at each
// menu open so it tracks display/resolution changes.
void update_scale(int canvas_h);

// Draw the panel for this frame. Call between ImGui::NewFrame() and
// ImGui::Render().
Result draw(const Frame& f);

// Per-open UI state the backend resets when the menu closes (so reopening is
// never mid-interaction).
void reset_on_close();

} // namespace cte::overlay::ui
