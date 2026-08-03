#pragma once

// The one thing the presentation layer's WndProc needs from the input layer.
// The window belongs to overlay_legacy_present.cpp, but the policy for what its
// messages MEAN belongs to overlay_legacy.cpp, which owns the focus-free capture
// design. Everything else flows the other way (input calls into present::), so
// this header exists only to break that one cycle.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace cte::overlay_legacy::input {

// WM_INPUT -> ImGui. Feeds mouse buttons/wheel/deltas and synthetic key events;
// no-ops unless the menu is open, CustomTalismanEffects holds the capture, and the game is
// foreground (RIDEV_INPUTSINK keeps delivering while alt-tabbed, and that input
// belongs to the other app).
void on_raw_input(HRAWINPUT h);

} // namespace cte::overlay_legacy::input
