#pragma once

// CustomTalismanEffects's LEGACY (v1) overlay backend, kept alive as the opt-in fallback
// for the handful of setups the in-swapchain backend v2 doesn't work on.
// Selected with `[overlay_render] renderer = window` in CustomTalismanEffects.ini; the
// default (`ingame`) runs backend v2 (overlay.hpp) instead. Exactly one of the
// two is ever started -- see run() in main.cpp.
//
// Dear ImGui drawn in its OWN transparent top-most window, rendered offscreen
// by a private D3D11 device and presented as a per-pixel-alpha GDI layered
// window -- it never touches the game's swapchain and creates none of its own,
// so it coexists with other overlay mods / frame-gen / RTSS / ReShade /
// Special K. In this mode CustomTalismanEffects installs no DXGI or NVAPI hook at all,
// which is precisely why it survives where backend v2 struggles.
//
// Implementation is split across two translation units:
//   overlay_legacy.cpp         -- input capture, open/close, thread, watchdogs
//   overlay_legacy_present.cpp -- window, D3D11, the layered present path
// The quest panel itself is NOT duplicated here: this backend drives the mod's
// one and only frontend, src/overlay_ui.cpp, through the same ui::Frame /
// ui::Result contract backend v2 uses. The internal contracts live in
// overlay_legacy_present.hpp / overlay_legacy_input.hpp (plus the shared
// overlay_ui.hpp); this header is the only surface the rest of the mod sees.

namespace cte::overlay_legacy {

// Resolve the pad reader, install the DirectInput8 detours (the pad APIs ER
// actually polls) and spawn the dedicated overlay thread that owns the window +
// D3D11 + ImGui. XInput detours are deliberately installed later, at the first
// menu open -- see hook_xinput() in overlay_legacy.cpp. The overlay window never
// takes focus (activation changes trigger frame-gen mods' seconds-long re-init),
// and keyboard/mouse are captured while the menu is open by re-targeting the
// process's raw-input registration at CustomTalismanEffects's window.
void setup();

// Re-copy the open key / gamepad combo from g_state into the overlay's runtime
// globals. setup() runs before params are ready (so before build_state() has
// applied the .ini's toggle_key/toggle_gamepad_combo to g_state) -- call this
// AFTER build_state() so the real configured combo takes effect instead of the
// hardcoded defaults setup() saw.
void sync_open_keys();

} // namespace cte::overlay_legacy
