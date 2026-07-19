#pragma once

// CustomTalismanEffects backend v2: DXGI interception and host-swapchain rendering.
//
// This interface deliberately exposes no D3D objects.  The UI/control thread
// publishes immutable ImGui packets, while the Present thread consumes them
// using resources owned solely by the backend.  No top-level overlay window,
// private presentation swapchain, display-mode change, or device recreation is
// part of this design.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdint>

struct ImDrawData;
struct ImFontAtlas;

namespace cte::overlay::present {

// Stable snapshot published by the selected game swapchain.  screen/client
// geometry is refreshed by the control thread; width/height are the actual
// backbuffer dimensions and therefore the ImGui logical canvas.
struct Canvas {
    HWND hwnd = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    bool hdr = false;
    bool ready = false;
};

// Install the DXGI/D3D12 discovery hooks.  Call once, as early as possible on
// a worker outside loader lock, so CreateSwapChain* can capture the exact
// command queue.  If creation was missed, the backend deliberately stays
// unavailable; a device-level queue guess is not a safe late-attach strategy.
bool install_hooks();

// True once the interception layer is installed.  This does not imply that a
// game swapchain has been observed yet.
bool hooks_installed();

// Current selected game canvas.  A seqlock makes this safe to read without
// ever blocking Present.
Canvas canvas();

// Publish visibility independently of draw data.  Closing is one atomic store:
// the next Present immediately stops submitting CustomTalismanEffects command lists.
void set_visible(bool visible);
bool visible();

// Deep-copy the frontend's CPU output.  Consumers never touch ImGui or its
// context, which isolates Present from the control/UI thread and from other
// injected ImGui copies.
void publish_draw_data(const ImDrawData* draw_data);
void publish_font_atlas(ImFontAtlas* atlas);
void clear_draw_data();

// Renderer failures are contained.  The control thread uses this signal to
// close the menu and restore input instead of leaving an invisible capture.
bool renderer_healthy();

// Stop submitting, drain the one renderer owner, and release or quarantine GPU
// objects without racing a Present callback. Hooks intentionally remain
// installed in pass-through mode: removing one edge from an arbitrary
// multi-DLL chain can sever hooks installed after ours. The module is pinned
// for process life.
void shutdown();

} // namespace cte::overlay::present
