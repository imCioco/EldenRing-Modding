// The LEGACY overlay's PRESENTATION layer: a SEPARATE, INDEPENDENT transparent
// top-most window, rendered offscreen by a private D3D11 device and presented as
// a per-pixel-alpha LAYERED window (UpdateLayeredWindow). Split out of
// overlay.cpp (2026-07-18); the design notes below are the whole reason this
// code looks the way it does.
//
// This is backend v1. It ships as the opt-in fallback for setups the
// in-swapchain backend v2 doesn't work on, and runs only when the .ini says
// `[overlay_render] renderer = window` -- see overlay_legacy.hpp.
//
// Why a separate window (not a swapchain hook): this path never touches the
// game's DXGI swapchain. That makes the overlay compatible with anything that
// wraps or renders on the game's swapchain -- other overlay mods (MapForGoblins
// 2.0.4+), Special K, NVIDIA Smooth Motion / frame-gen, ReShade. The old
// Present/ExecuteCommandLists DX12 hook shared GPU + command-queue state with the
// game and with those tools, and that shared state getting corrupted crashed the
// game.
//
// Why GDI layered presentation (not a private DXGI swapchain): overlay and
// frame-gen tools hook IDXGISwapChain::Present PROCESS-WIDE through the shared
// vtable (erdGameTools, RTSS / MSI Afterburner, frame-gen mods, capture tools),
// so a second swapchain in the process gets THEIR code run on MY presents:
// erdGameTools' DX12 renderer access-violated on my composition swapchain, and
// RTSS's frame limiter counted my presents against the game's cap (framerate
// tanked while the menu was open). A DLL-name allowlist (the old
// "erdGameTools.dll detected -> layered fallback") can never enumerate every
// such tool, so the hook-invisible path is now the ONLY path: no swapchain, no
// Present, nothing for any injector to see. UpdateLayeredWindow also gives true
// per-pixel input transparency -- the old DComp window needed a show-at-open /
// hide-at-close dance to not steal WM_SETCURSOR while closed, and un-hiding it
// over a DXGI-fullscreen game (ER requires Fullscreen display mode for HDR)
// tripped DXGI's fullscreen-occlusion watchdog: game minimized + black screen.
// Layered windows don't occlude for that check, and the window never changes
// visibility anyway. Also works under Wine/Proton, where composition
// swapchains are E_NOTIMPL.
//
// The window: WS_POPUP + WS_EX_LAYERED|TOPMOST|TOOLWINDOW|NOACTIVATE. Kept
// SHOWN for the mod's lifetime and NEVER activated: hiding it uncovers the
// game (a DWM present-mode transition), and taking focus deactivates the game
// -- which makes frame-gen mods (erdGameTools / Smooth Motion) tear down and
// re-init for several seconds, the classic ER "alt-tab freeze". Both open and
// close must change NOTHING the game's presentation or activation state can
// see. Closed = one committed alpha-0 frame (per-pixel input-transparent by
// construction) + WS_EX_TRANSPARENT (belt and braces).
//
// The present path: ImGui renders into a D3D11 texture, which is read back
// through a staging texture into a DIB and pushed to the layered window. The
// window's GEOMETRY IS STATIC: it spans the full game canvas at all times and
// is only moved/resized when the canvas itself changes (one plain
// UpdateLayeredWindow re-establishes it). Every other present pushes ONLY the
// dirty region -- the union of this frame's drawn-content box and the previous
// one -- via UpdateLayeredWindowIndirect(prcDirty), with explicit
// destination/size values equal to the already-established geometry. The
// Phase-0 screen-pixel audit proved that this call shape updates in place;
// NULL destination/size returned TRUE while silently updating nothing on this
// machine. Static frames still cost nothing, and small changes only
// re-upload/re-compose a strip.
// Both halves matter: pushing the full canvas each present cost the game a
// third of its framerate while the menu was open, and the earlier fix for that
// (moving/sizing the window to the content box EVERY present) made the topmost
// window's geometry churn at up to 90 Hz -- over a DXGI-fullscreen game (ER in
// HDR) that continuous occlusion re-evaluation toggled G-sync on/off in a loop
// and could trip the fullscreen-occlusion watchdog outright (game minimized to
// the taskbar). Nothing on this path blocks on vsync, so the open-menu loop
// paces itself to the display refresh rate instead of spinning.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include "overlay_legacy_present.hpp"
#include "overlay_legacy_input.hpp"
// The quest panel is NOT duplicated for this backend: it drives the mod's one
// shared frontend (src/overlay_ui.cpp), same as backend v2 does.
#include "overlay_ui.hpp"
#include "log.hpp"

#pragma comment(lib, "d3d11.lib")

// From imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

namespace cte::overlay_legacy::present {

// The frontend lives in cte::overlay::ui and is shared with backend v2 -- this
// alias is what keeps the ui:: calls below pointing at it after the legacy
// backend moved out of cte::overlay.
namespace ui = cte::overlay::ui;

// ── shared state (declared in the header) ──
HWND  g_hwnd = nullptr;
HWND  g_game_hwnd = nullptr;
UINT  g_back_w = 0, g_back_h = 0;
POINT g_canvas_pos{};

uint64_t g_present_hash = 0;
int g_presents_attempted = 0;
int g_presents_landed = 0;
int g_present_failed_updates = 0;
int g_present_full_reestablishes = 0;

namespace {

const wchar_t* kOverlayClass = L"CustomTalismanEffects_OverlayWindow";

ID3D11Device*        g_d3d_device = nullptr;
ID3D11DeviceContext* g_d3d_ctx = nullptr;

// Layered-presentation targets: ImGui renders into g_ltex, which is read back
// via g_lstaging into the g_ldib DIB and pushed with UpdateLayeredWindow.
ID3D11Texture2D*        g_ltex = nullptr;
ID3D11RenderTargetView* g_lrtv = nullptr;
ID3D11Texture2D*        g_lstaging = nullptr;
HDC     g_lmemdc = nullptr;
HBITMAP g_ldib = nullptr;
void*   g_ldibbits = nullptr;

// Established (on-screen) window geometry, maintained by render_frame. The
// window always spans the full canvas; presents never move or resize it --
// per-frame geometry churn over a DXGI-fullscreen game toggles G-sync and can
// trip the fullscreen-occlusion watchdog (see the header comment).
bool  g_geom_ok = false; // window geometry matches g_canvas_pos + g_back_w/h
POINT g_geom_pos{};      // position the geometry was established at
UINT  g_geom_w = 0, g_geom_h = 0;
RECT  g_last_box{};      // last PRESENTED content box (empty = nothing shown)

bool g_context_inited = false; // ImGui context + win32 backend
bool g_d3d_inited = false;     // D3D11 + dx11 backend
bool g_present_failure_logged = false;

// ── window ──
LRESULT CALLBACK overlay_wndproc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INPUT:
        input::on_raw_input(reinterpret_cast<HRAWINPUT>(lParam));
        return DefWindowProcW(hWnd, msg, wParam, lParam); // required cleanup
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE; // NEVER take focus (see the input section)
    case WM_DESTROY:
        return 0;
    }
    // WM_SETCURSOR is deliberately NOT handled: we force NO cursor state at
    // all. Our window class has hCursor = nullptr, so falling through to
    // DefWindowProc leaves the cursor exactly as the game (or the OS) already
    // set it -- a game menu's own OS arrow stays visible over the panel (the
    // single pointer there; update_virtual_cursor's visible-cursor branch
    // tracks it and keeps the software cursor off), and during gameplay the
    // game's own hidden state is left alone. Forcing EITHER state here was a
    // shipped bug, at different times: an always-visible arrow both showed the
    // game's re-pinned-mid-screen pointer as a stuck arrow AND flip-flopped
    // CURSOR_SHOWING against the game's own hide every frame (the open-menu FPS
    // regression); always hiding it forced the visible->hidden SOURCE HANDOVER
    // in update_virtual_cursor to happen right at the panel edge -- exactly
    // where the cursor teleported.
    //
    // Legacy mouse messages still arrive over our topmost window while it is
    // interactive; raw input is the single mouse source (double-feeding makes
    // the cursor jump between the OS and virtual positions), so swallow them.
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST)
        return 0;
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return 0;
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ── layered-presentation render targets ──
void release_layered_targets() {
    if (g_lrtv) { g_lrtv->Release(); g_lrtv = nullptr; }
    if (g_ltex) { g_ltex->Release(); g_ltex = nullptr; }
    if (g_lstaging) { g_lstaging->Release(); g_lstaging = nullptr; }
    if (g_lmemdc) { DeleteDC(g_lmemdc); g_lmemdc = nullptr; }
    if (g_ldib) { DeleteObject(g_ldib); g_ldib = nullptr; }
    g_ldibbits = nullptr;
}

bool create_layered_targets(UINT w, UINT h) {
    release_layered_targets();
    if (!g_d3d_device || w == 0 || h == 0) return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(g_d3d_device->CreateTexture2D(&td, nullptr, &g_ltex)) || !g_ltex) return false;
    if (FAILED(g_d3d_device->CreateRenderTargetView(g_ltex, nullptr, &g_lrtv)) || !g_lrtv) return false;
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g_d3d_device->CreateTexture2D(&td, nullptr, &g_lstaging)) || !g_lstaging) return false;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(w);
    bi.bmiHeader.biHeight = -static_cast<LONG>(h); // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    g_ldib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &g_ldibbits, nullptr, 0);
    g_lmemdc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!g_ldib || !g_lmemdc || !g_ldibbits) return false;
    SelectObject(g_lmemdc, g_ldib);
    g_back_w = w; g_back_h = h;
    // Fresh surface: everything on screen is stale, and the window geometry
    // no longer matches -- the next present must re-establish it in full.
    g_geom_ok = false;
    g_last_box = RECT{};
    return true;
}

// ── D3D11 (offscreen) + ImGui dx11 backend ──
// POD-only locals: SEH-guarded by init_d3d_seh (a torn GPU state can AV).
// No swapchain, ever: the render target is a plain texture read back into a
// DIB, so present-hooking tools (RTSS / frame-gen / erdGameTools / capture)
// have nothing of ours to intercept.
bool init_d3d() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT; // render target matches the DIB layout
    D3D_FEATURE_LEVEL got{};
    const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                 want, 2, D3D11_SDK_VERSION,
                                 &g_d3d_device, &got, &g_d3d_ctx))) {
        flog("[overlay-legacy] D3D11CreateDevice failed");
        return false;
    }

    RECT cr{};
    GetClientRect(g_hwnd, &cr);
    UINT w = static_cast<UINT>(cr.right - cr.left), h = static_cast<UINT>(cr.bottom - cr.top);
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    if (!create_layered_targets(w, h)) {
        flog("[overlay-legacy] layered targets failed");
        return false;
    }

    if (!g_context_inited) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr; // don't drop an imgui.ini next to the game
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        // Draw our own software cursor; never touch the OS cursor image.
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        ui::load_fonts(io);
        ui::apply_style();
        ImGui_ImplWin32_Init(g_hwnd);
        g_context_inited = true;
    }

    if (!ImGui_ImplDX11_Init(g_d3d_device, g_d3d_ctx)) {
        flog("[overlay-legacy] ImGui_ImplDX11_Init failed");
        return false;
    }
    g_d3d_inited = true;
    flog("[overlay-legacy] separate-window overlay ready (%ux%u, layered)", w, h);
    return true;
}

// Bounding box of everything ImGui drew this frame (panel, popups, tooltips,
// software cursor), in canvas pixels. Empty ({0,0,0,0}) when nothing was
// drawn (blank/closed frame). render_frame presents only the union of this
// box and the previously shown one -- that dirty region is what preserves the
// game's framerate while the menu is open (a full-canvas upload every frame
// made DWM re-upload + recompose the whole screen).
bool rect_empty(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }

RECT rect_union(const RECT& a, const RECT& b) {
    if (rect_empty(a)) return b;
    if (rect_empty(b)) return a;
    return RECT{a.left < b.left ? a.left : b.left,
                a.top < b.top ? a.top : b.top,
                a.right > b.right ? a.right : b.right,
                a.bottom > b.bottom ? a.bottom : b.bottom};
}

RECT content_bounds(bool draw) {
    const LONG cw = static_cast<LONG>(g_back_w), ch = static_cast<LONG>(g_back_h);
    const RECT none{0, 0, 0, 0};
    if (!draw) return none;
    const ImDrawData* dd = ImGui::GetDrawData();
    if (!dd) return none;
    float x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX;
    for (int i = 0; i < dd->CmdListsCount; ++i) {
        const ImDrawList* cl = dd->CmdLists[i];
        for (int v = 0; v < cl->VtxBuffer.Size; ++v) {
            const ImVec2 p = cl->VtxBuffer.Data[v].pos;
            if (p.x < x0) x0 = p.x;
            if (p.y < y0) y0 = p.y;
            if (p.x > x1) x1 = p.x;
            if (p.y > y1) y1 = p.y;
        }
    }
    if (x1 <= x0 || y1 <= y0) return none;
    RECT r{static_cast<LONG>(x0) - 2, static_cast<LONG>(y0) - 2,
           static_cast<LONG>(x1) + 3, static_cast<LONG>(y1) + 3}; // pad AA fringe
    if (r.left >= cw || r.top >= ch || r.right <= 0 || r.bottom <= 0) return none;
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > cw) r.right = cw;
    if (r.bottom > ch) r.bottom = ch;
    return r;
}

// ── dirty detection ──
// ImGui is immediate-mode: an unchanged UI produces byte-identical draw data.
// Hashing it lets the open-menu loop SKIP the whole render + readback +
// UpdateLayeredWindow cycle for static frames (no mouse movement, no
// animation), which drops the overlay's GPU/DWM cost to ~zero while idle-open.
// This is how external "ESP"-style overlays stay cheap: they only re-present
// when something actually changed. The canvas origin/size are hashed in too:
// identical pixels still need a re-present when the game window moved.
uint64_t fnv1a(const void* data, size_t n, uint64_t h) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// ── frame pacing ──
// Nothing on the layered path blocks on vsync (there is no Present), so the
// open-menu loop must pace itself: unthrottled, the render + readback +
// UpdateLayeredWindow cycle burns a CPU core and GPU copy bandwidth against
// the game. Target the refresh rate of the monitor the game is on; queried at
// each menu open (cheap, and displays/refresh rates change).
std::chrono::nanoseconds g_frame_period{16'666'667}; // 60 Hz until measured

} // namespace

// ── the contract with the rest of the overlay (overlay_legacy_present.hpp) ──

bool create_window() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = overlay_wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kOverlayClass;
    wc.hCursor = nullptr;
    RegisterClassExW(&wc);

    // WS_EX_NOACTIVATE: the overlay must never take focus -- activation changes
    // on the game window make frame-gen mods re-init for seconds (alt-tab
    // freeze); input is captured focus-free (see overlay_legacy.cpp's input section).
    // WS_EX_LAYERED: presentation is UpdateLayeredWindow (per-pixel alpha).
    const DWORD ex = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                     WS_EX_NOACTIVATE;
    g_hwnd = CreateWindowExW(ex, kOverlayClass, L"CustomTalismanEffects overlay", WS_POPUP,
                             0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) {
        flog("[overlay-legacy] CreateWindowExW failed; overlay disabled");
        return false;
    }
    return true;
}

bool init_d3d_seh() {
    __try { return init_d3d(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void resize_seh(UINT w, UINT h) {
    if (w == 0 || h == 0) return;
    __try { create_layered_targets(w, h); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void shutdown() {
    if (g_d3d_inited) {
        __try { ImGui_ImplDX11_Shutdown(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_d3d_inited = false;
    }
    if (g_context_inited) {
        __try { ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_context_inited = false;
    }
    release_layered_targets();
    if (g_d3d_ctx) { g_d3d_ctx->Release(); g_d3d_ctx = nullptr; }
    if (g_d3d_device) { g_d3d_device->Release(); g_d3d_device = nullptr; }
    if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = nullptr; }
}

void rebuild_fonts() {
    if (!g_context_inited) return;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();     // drops every ImFont*; ui::load_fonts reassigns them
    ui::load_fonts(io);
    io.Fonts->Build();
    // Drop the GPU font texture; ImGui_ImplDX11_NewFrame re-creates the device
    // objects on the next frame because the font SRV is now null.
    ImGui_ImplDX11_InvalidateDeviceObjects();
    flog("[overlay-legacy] font atlas rebuilt (language switch)");
}

// Pick the foreground window if it belongs to our process (skip tool windows, i.e.
// our own overlay). Cached so we keep covering the game after focus moves.
HWND find_game_window() {
    HWND fg = GetForegroundWindow();
    if (fg && fg != g_hwnd) {
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) {
            const LONG ex = GetWindowLongW(fg, GWL_EXSTYLE);
            if (!(ex & WS_EX_TOOLWINDOW)) g_game_hwnd = fg;
        }
    }
    return g_game_hwnd;
}

// Track the game's client area ("the canvas"): screen origin into g_canvas_pos,
// size out. Deliberately does NOT position the window -- render_frame changes
// geometry only when this canvas itself changes. The old per-frame SetWindowPos
// churned DWM and picked z-order fights with other topmost overlays such as the
// RTSS OSD.
void track_game_canvas(int& out_w, int& out_h) {
    out_w = out_h = 0;
    HWND game = find_game_window();
    if (!game || !IsWindow(game)) return;
    RECT cr{};
    if (!GetClientRect(game, &cr)) return;
    POINT tl{cr.left, cr.top};
    ClientToScreen(game, &tl);
    const int w = cr.right - cr.left, h = cr.bottom - cr.top;
    if (w <= 0 || h <= 0) return;
    g_canvas_pos = tl;
    out_w = w;
    out_h = h;
}

// Toggle the overlay window between hit-testable (menu open: it swallows the
// legacy mouse messages so they can't reach the game) and click-through (menu
// closed: everything falls through). Only WS_EX_TRANSPARENT toggles --
// WS_EX_NOACTIVATE is permanent, the overlay never takes focus. The window
// stays SHOWN either way; keeping the game covered holds its DWM presentation
// steady so closing never triggers a present-mode transition.
void set_click_through(bool on) {
    if (!g_hwnd) return;
    LONG ex = GetWindowLongW(g_hwnd, GWL_EXSTYLE);
    if (on) ex |=  WS_EX_TRANSPARENT;
    else    ex &= ~WS_EX_TRANSPARENT;
    SetWindowLongW(g_hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

uint64_t draw_data_hash() {
    const ImDrawData* dd = ImGui::GetDrawData();
    uint64_t h = 14695981039346656037ull;
    h = fnv1a(&g_canvas_pos, sizeof(g_canvas_pos), h);
    h = fnv1a(&g_back_w, sizeof(g_back_w), h);
    h = fnv1a(&g_back_h, sizeof(g_back_h), h);
    if (!dd) return h;
    for (int i = 0; i < dd->CmdListsCount; ++i) {
        const ImDrawList* cl = dd->CmdLists[i];
        h = fnv1a(cl->VtxBuffer.Data,
                  static_cast<size_t>(cl->VtxBuffer.Size) * sizeof(ImDrawVert), h);
        h = fnv1a(cl->IdxBuffer.Data,
                  static_cast<size_t>(cl->IdxBuffer.Size) * sizeof(ImDrawIdx), h);
        h = fnv1a(cl->CmdBuffer.Data,
                  static_cast<size_t>(cl->CmdBuffer.Size) * sizeof(ImDrawCmd), h);
    }
    return h;
}

void force_full_reestablish() { g_geom_ok = false; }

void reset_open_telemetry() {
    g_present_hash = 0; // force the first open frame past the dirty-skip
    g_presents_attempted = 0;
    g_presents_landed = 0;
    g_present_failed_updates = 0;
    g_present_full_reestablishes = 0;
    g_present_failure_logged = false;
}

// One rendered frame (SEH-wrapped). Clears to transparent; draws ImGui only
// when the caller passes draw==true. Presentation keeps the window geometry
// STATIC: when the canvas moved/resized (or the targets were recreated) one
// plain UpdateLayeredWindow re-establishes the full-canvas geometry; every
// other present goes through UpdateLayeredWindowIndirect with the SAME explicit
// dst/size and prcDirty = union(this frame's content box, the previous one).
// Phase 0 proved this updates pixels in place while the NULL dst/size form is a
// silent TRUE/no-op on this machine. The union also erases vacated pixels: the
// render target is cleared to alpha-0 every frame, so copying the union region
// back naturally blanks whatever the panel/cursor uncovered. Returns true only
// when the requested pixels are already correct or the Win32 present API
// reported success. POD-only locals in the __try frame.
bool render_frame(bool draw) {
    const RECT b = content_bounds(draw); // C++ objects stay out of the __try frame
    const bool need_full = !g_geom_ok ||
        g_geom_pos.x != g_canvas_pos.x || g_geom_pos.y != g_canvas_pos.y ||
        g_geom_w != g_back_w || g_geom_h != g_back_h;
    RECT pr; // the region rendered/read back/presented by this call
    if (need_full) {
        pr = RECT{0, 0, static_cast<LONG>(g_back_w), static_cast<LONG>(g_back_h)};
    } else {
        pr = rect_union(b, g_last_box);
        if (rect_empty(pr)) { g_last_box = b; return true; } // already blank
    }
    ++g_presents_attempted;
    bool staging_mapped = false;
    HDC screen = nullptr;
    __try {
        const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // fully transparent
        if (!g_lrtv || !g_lstaging || !g_ltex || !g_ldib || !g_d3d_ctx)
            return false;
        g_d3d_ctx->OMSetRenderTargets(1, &g_lrtv, nullptr);
        g_d3d_ctx->ClearRenderTargetView(g_lrtv, clear);
        if (draw) ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        D3D11_BOX box{static_cast<UINT>(pr.left), static_cast<UINT>(pr.top), 0,
                      static_cast<UINT>(pr.right), static_cast<UINT>(pr.bottom), 1};
        g_d3d_ctx->CopySubresourceRegion(g_lstaging, 0, box.left, box.top, 0,
                                         g_ltex, 0, &box);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(g_d3d_ctx->Map(g_lstaging, 0, D3D11_MAP_READ, 0, &m)))
            return false;
        staging_mapped = true;
        const size_t stride = static_cast<size_t>(g_back_w) * 4;
        const size_t xoff = static_cast<size_t>(pr.left) * 4;
        const size_t rowbytes = static_cast<size_t>(pr.right - pr.left) * 4;
        for (LONG y = pr.top; y < pr.bottom; ++y)
            memcpy(static_cast<uint8_t*>(g_ldibbits) + static_cast<size_t>(y) * stride + xoff,
                   static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch + xoff,
                   rowbytes);
        g_d3d_ctx->Unmap(g_lstaging, 0);
        staging_mapped = false;

        POINT src{0, 0};
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        screen = GetDC(nullptr);
        if (!screen) return false;
        BOOL landed = FALSE;
        DWORD error = ERROR_SUCCESS;
        if (need_full) {
            // Geometry (re-)establish: the ONE place the window can actually
            // move/resize -- full-canvas position + size, full upload.
            SIZE sz{static_cast<LONG>(g_back_w), static_cast<LONG>(g_back_h)};
            POINT dst{g_canvas_pos.x, g_canvas_pos.y};
            SetLastError(ERROR_SUCCESS);
            landed = UpdateLayeredWindow(g_hwnd, screen, &dst, &sz, g_lmemdc,
                                          &src, 0, &bf, ULW_ALPHA);
            if (landed) {
                g_geom_ok = true;
                g_geom_pos = g_canvas_pos;
                g_geom_w = g_back_w;
                g_geom_h = g_back_h;
                g_last_box = b;
                ++g_presents_landed;
                ++g_present_full_reestablishes;
            } else {
                g_geom_ok = false;
            }
        } else {
            // Phase-0 winner B. These values exactly match the established
            // geometry, so the call updates in place; GetWindowRect stayed
            // unchanged throughout the mechanical pixel audit.
            POINT dst{g_geom_pos.x, g_geom_pos.y};
            SIZE sz{static_cast<LONG>(g_geom_w), static_cast<LONG>(g_geom_h)};
            UPDATELAYEREDWINDOWINFO ulwi{};
            ulwi.cbSize = sizeof(ulwi);
            ulwi.hdcDst = screen;
            ulwi.pptDst = &dst;
            ulwi.psize = &sz;
            ulwi.hdcSrc = g_lmemdc;
            ulwi.pptSrc = &src;
            ulwi.crKey = 0;
            ulwi.pblend = &bf;
            ulwi.dwFlags = ULW_ALPHA;
            ulwi.prcDirty = &pr;
            SetLastError(ERROR_SUCCESS);
            landed = UpdateLayeredWindowIndirect(g_hwnd, &ulwi);
            error = GetLastError(); // capture before ReleaseDC/flog can replace it
            if (landed) {
                g_last_box = b;
                ++g_presents_landed;
            } else {
                ++g_present_failed_updates;
                // Degrade to the known-good full path next present. The hash
                // remains stale too, so the gate retries instead of declaring
                // a failed frame to be on screen.
                g_geom_ok = false;
                if (!g_present_failure_logged) {
                    flog("[overlay-legacy] [WARN] present update failed (err %lu) -- "
                         "degrading to full re-establish", error);
                    g_present_failure_logged = true;
                }
            }
        }
        ReleaseDC(nullptr, screen);
        screen = nullptr;
        return landed != FALSE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // A bad frame must never take the whole game down or poison its retry
        // by leaving the staging texture mapped / a screen DC outstanding.
        __try {
            if (staging_mapped && g_d3d_ctx)
                g_d3d_ctx->Unmap(g_lstaging, 0);
            if (screen) ReleaseDC(nullptr, screen);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_geom_ok = false;
        return false;
    }
}

void update_frame_period() {
    DWORD hz = 0;
    HMONITOR mon = MonitorFromWindow(g_game_hwnd ? g_game_hwnd : g_hwnd,
                                     MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
            hz = dm.dmDisplayFrequency;
    }
    if (hz < 30 || hz > 480) hz = 60; // 0/1 = "hardware default"
    g_frame_period = std::chrono::nanoseconds(1'000'000'000ull / hz);
}

// Sleep the remainder of the current frame slot. Self-resyncing: after a stall
// or a close/open gap the schedule snaps to "now" instead of fast-forwarding.
void pace_frame() {
    static std::chrono::steady_clock::time_point next{};
    const auto now = std::chrono::steady_clock::now();
    if (next < now - g_frame_period || next > now + g_frame_period)
        next = now;
    next += g_frame_period;
    std::this_thread::sleep_until(next);
}

} // namespace cte::overlay_legacy::present
