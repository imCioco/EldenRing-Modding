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
// command queue.  If creation was missed, the backend stays unavailable until
// arm_adoption() recovers the live swapchain with exact-queue evidence; a
// bare device-level queue guess is never used.
bool install_hooks();

// True once the interception layer is installed.  This does not imply that a
// game swapchain has been observed yet.
bool hooks_installed();

// Late-load fallback: when the game created its swapchain before
// install_hooks() could observe it (delayed loaders, injectors), this arms
// pass-through Present observers that find the live swapchain, install the
// same per-instance shadow on it, and bind its queue only from exact or
// single-direct-queue evidence (Elden Ring adapter); anything ambiguous stays
// pass-through.  Idempotent while pending; calling again after a resolution
// re-arms the search.  Overlay/control thread only.  The hint window (may be
// null) selects the display for a one-shot vendor HDR state query.
void arm_adoption(HWND game_window_hint);

// Log where the creation-hook prologues (or factory vtable slots) point today
// and how often the detours have run -- distinguishes "loaded too late" from
// "hook overwritten by another mod", and names the owning module in the
// latter case.
void log_creation_hook_forensics();

// True once a swapchain creation has been observed through this mod's own
// creation detours AND that swapchain was shadowed and queue-bound.  A visible
// game window is NOT evidence of a late load: bootstrap does config and
// flag-scanner work between installing the hooks and reaching the discovery
// check, so the game can legitimately create and present its swapchain in that
// window.  Arming adoption on window presence alone is a false positive that
// costs three extra process-wide detours and, because try_adopt_swapchain
// rejects any already-shadowed chain, can never help.
bool observed_swapchain_creation();

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
