#pragma once

// In-game config overlay: Dear ImGui, kept as a private statically linked
// frontend. Backend v2 publishes immutable draw packets and composites them
// into ELDEN RING's existing D3D12 backbuffer immediately before the real
// Present. It creates no top-level window, graphics device, command queue, or
// presentation swapchain -- the game keeps owning all of those, which is what
// keeps the overlay compatible with other overlay mods, frame-gen and Special K
// (see overlay_present.cpp for the full invariant list).
//
// Opens with the configured key (default Insert) or gamepad combo (default
// L3+R3), lets the player toggle talisman effects live, and enforces the family
// exclusivity / stacking rules.

namespace cte::overlay {

// Install the DXGI/D3D12 discovery hooks. Call this FIRST on the DllMain
// worker, immediately after it escapes loader lock and before any config
// parsing, compat wait, or param wait: the exact D3D12 command queue can only
// be captured from the game's CreateSwapChain* transaction, and missing it
// leaves the overlay unavailable for the session. Also owns MinHook init.
bool bootstrap();

// Start input handling and the frontend packet-producer thread. Detours the
// gamepad APIs so the game reads a neutral pad while the menu is open (dinput8
// GetDeviceState/GetDeviceData now; XInput at menu-open). Keyboard/mouse are
// captured focus-free by re-targeting the process's raw-input registration at a
// message-only sink window while open -- nothing ever takes focus, so frame-gen
// mods don't re-init. GPU resources initialize lazily on a validated game
// Present and exact command queue.
void setup();

// Re-copy the open key / gamepad combo from g_state into the overlay's runtime
// globals. setup() runs before params are ready (so before build_state() has
// applied the .ini's toggle_key/toggle_gamepad_combo to g_state) -- call this
// AFTER build_state() so the real configured combo takes effect instead of the
// hardcoded defaults setup() saw.
void sync_open_keys();

} // namespace cte::overlay
