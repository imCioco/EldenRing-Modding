#pragma once

// The LEGACY overlay's PRESENTATION layer: CustomTalismanEffects's own top-most window, the
// offscreen D3D11 device that ImGui renders into, and the GDI layered-window
// path that puts those pixels on screen. Split out of overlay.cpp (2026-07-18);
// reached only when the .ini asks for it (`[overlay_render] renderer = window`).
//
// Every design constraint here is load-bearing and was paid for with a shipped
// bug -- read the header comment of overlay_legacy_present.cpp before changing
// any of it. In short: no swapchain (present-hooking tools would run their code
// on my presents), no visibility changes (uncovering the game causes a DWM
// present-mode transition, and un-hiding over a DXGI-fullscreen game minimizes
// it), and no per-frame geometry changes (topmost-window churn over a
// fullscreen game toggles G-sync and trips the occlusion watchdog).
//
// ALL of this is overlay-thread-only. Nothing here is thread-safe, and nothing
// needs to be: exactly one thread ever touches it.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdint>

namespace cte::overlay_legacy::present {

// ── shared state (overlay thread only) ──
extern HWND  g_hwnd;        // the legacy overlay window
extern HWND  g_game_hwnd;   // tracked game main window
extern UINT  g_back_w;      // canvas (game client area) size
extern UINT  g_back_h;
extern POINT g_canvas_pos;  // canvas origin, in screen coords

// ── lifecycle ──
// Register the window class and create the (initially tiny, always
// WS_EX_NOACTIVATE + WS_EX_LAYERED) overlay window. Returns false if the window
// could not be created, in which case the overlay must be disabled.
bool create_window();

// Create the D3D11 device, the layered render/staging targets, and the ImGui
// context + backends. SEH-guarded: a torn GPU state can access-violate here.
bool init_d3d_seh();

// Re-create the layered targets at a new canvas size. SEH-guarded; no-op for a
// zero dimension.
void resize_seh(UINT w, UINT h);

// Release everything this layer owns (ImGui backends + context, D3D11 targets,
// device, window). Best-effort -- the process usually just exits.
void shutdown();

// Re-bake the ImGui font atlas (after a language switch: both the glyph
// coverage and the CJK merge precedence depend on the active language). MUST be
// called BETWEEN frames -- after ImGui::Render(), before the next
// ImGui_ImplDX11_NewFrame() -- because it invalidates every live ImFont*.
void rebuild_fonts();

// ── the game window / canvas ──
// Pick the foreground window if it belongs to this process (skipping tool
// windows, i.e. CustomTalismanEffects's own overlay). Cached in g_game_hwnd so the game
// stays tracked after focus moves elsewhere.
HWND find_game_window();

// Re-track the game's client area: origin into g_canvas_pos, size out. Does NOT
// move the window -- render_frame changes geometry only when the canvas itself
// changed.
void track_game_canvas(int& out_w, int& out_h);

// ── presentation ──
// Toggle between hit-testable (menu open) and click-through (menu closed). Only
// WS_EX_TRANSPARENT changes; WS_EX_NOACTIVATE is permanent and the window stays
// SHOWN either way.
void set_click_through(bool on);

// Render one frame and push it to the layered window. `draw` false clears to a
// fully transparent frame (the closed state). Returns true only when the
// requested pixels are already correct or the Win32 present API reported
// success -- the caller MUST NOT treat a failed present as being on screen.
bool render_frame(bool draw);

// Force the next present through the proven full-canvas re-establish path.
// Used by the close path: a failed in-place update must never leave a stale
// panel image behind.
void force_full_reestablish();

// Hash of this frame's ImGui draw data + canvas geometry. An unchanged UI
// hashes identically, which is what lets the open-menu loop skip the whole
// render/readback/present cycle for static frames.
uint64_t draw_data_hash();

// Last PRESENTED hash. The caller stores a new value ONLY after render_frame
// returned true, so a failed present leaves it stale and the next tick retries.
extern uint64_t g_present_hash;

// Per-open presentation telemetry, for the close-time summary line. A "full"
// present is a successful geometry re-establish; failed-update counts only the
// in-place branch, whose first failure also arms the full-path degradation.
extern int g_presents_attempted;
extern int g_presents_landed;
extern int g_present_failed_updates;
extern int g_present_full_reestablishes;

// Zero the counters and the present hash. Call at each menu open.
void reset_open_telemetry();

// ── frame pacing ──
// Nothing on the layered path blocks on vsync, so the open-menu loop paces
// itself. update_frame_period() re-reads the refresh rate of the monitor the
// game is on (call at each menu open); pace_frame() sleeps the remainder of the
// current frame slot.
void update_frame_period();
void pace_frame();

} // namespace cte::overlay_legacy::present
