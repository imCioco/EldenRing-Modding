// The overlay's INPUT + FRONTEND CONTROL layer.  Graphics submission no longer
// happens here: this thread owns ImGui and publishes immutable CPU draw packets;
// overlay_present.cpp consumes them inline on the game's Present thread.
//
// This replaces the previous separate-window overlay (its own top-most D3D11 +
// DirectComposition window). That design existed to avoid touching the game's
// swapchain at all; backend v2 reaches the same goal from the other side -- it
// observes DXGI creation, composites into the game's EXISTING D3D12 backbuffer
// with private resources immediately before the real Present, and creates no
// window, device, queue, or presentation swapchain of its own. See
// overlay_present.cpp for the invariants that keep that safe alongside other
// injected overlays, frame-gen mods and Special K.
//
// The other two thirds live next door and this file drives both:
//   - overlay_present.cpp -- DXGI hook chaining, swapchain/queue validation,
//     and the private D3D12 renderer that targets the game's current buffer.
//   - overlay_ui.cpp -- the talisman panel itself. Backend-agnostic; it gets a
//     ui::Frame snapshot each frame and hands back a ui::Result.
//
// Architecture:
//   - bootstrap() installs lightweight graphics discovery hooks early. It MUST
//     run before any slow worker work (config parsing, the compat wait, param
//     waits): missing the game's CreateSwapChain* transaction makes exact D3D12
//     queue association impossible, and the overlay then stays unavailable.
//   - setup() resolves the input APIs and spawns a detached control thread. It
//     creates a message-only input sink plus this mod's private ImGui context;
//     it never creates a visible window, graphics device, or swapchain.
//   - Each iteration the game's client area ("the canvas") is re-tracked; the
//     ImGui logical canvas always spans it, whatever the window's actual box.
//   - Open/close (default Insert / L3+R3) + Esc/B are polled with GetAsyncKeyState.
//   - While the menu is open, keyboard + mouse are captured focus-free by
//     re-targeting the process's raw-input registration at our window (which
//     also starves the game's raw-input reader, so menu input never leaks into
//     gameplay). The virtual cursor follows the native OS position once the
//     game's pin is intercepted, with raw-delta integration as a safe fallback;
//     the game's registration is restored on close. Gamepad: XInput +
//     DirectInput8 detours hand the game neutral pad input while the menu is
//     open.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <Xinput.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_win32.h>

#include "overlay.hpp"
#include "overlay_coexist.hpp"
#include "overlay_present.hpp"
#include "overlay_ui.hpp"
#include "state.hpp"
#include "hooks.hpp"
#include "log.hpp"

#pragma comment(lib, "dxguid.lib") // IID_IDirectInput8W/A, GUID_SysKeyboard

// From imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

namespace cte {

namespace present = overlay::present;
namespace ui = overlay::ui;

namespace {

using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetState_t pXInputGetState = nullptr; // real pad reader (MinHook trampoline,
                                            // or the direct proc if hooking failed)

// Open/close inputs, snapshotted from g_state at setup().
unsigned int   g_open_vk = 0x2D;         // VK_INSERT
unsigned short g_open_pad_mask = 0x00C0; // L3+R3
bool g_open_pad_is_hold = false;         // true if the combo needs a hold, not a tap
constexpr long long kPadHoldMs = 1000;   // hardcoded hold duration for HOLD_ combos

std::atomic<bool> g_running{false};   // overlay thread alive
std::atomic<bool> g_menu_open{false};

// A message-only HWND receives raw input while the menu is open.  It is not a
// top-level or composited surface and can never activate, occlude, or alter the
// game's presentation mode.
HWND g_input_hwnd = nullptr;
HWND g_game_hwnd = nullptr;
UINT g_back_w = 0, g_back_h = 0;
POINT g_canvas_pos{};
int g_client_w = 0, g_client_h = 0;
ImGuiContext* g_imgui_context = nullptr;

// ── gamepad snapshot (real state read via the trampoline) ──
XINPUT_GAMEPAD g_pad{};
bool  g_pad_ok = false;

inline bool kd(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Is the app in the foreground our own process (game or our overlay)? Gates the
// open/close polling so Insert/Esc don't act while alt-tabbed to another app.
bool foreground_is_ours() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// ── input (focus-free) ──
// The game must NEVER lose foreground activation: frame-generation mods
// (erdGameTools / NVIDIA Smooth Motion) tear their pipeline down and re-init
// it whenever the game window is deactivated OR reactivated -- a multi-second
// frozen-frame stall (a plain alt-tab reproduces it with this mod unloaded).
// There is no overlay window to activate any more, and we never call
// SetForegroundWindow in either direction. While the menu is open, input is
// captured WITHOUT focus:
//   - Keyboard + mouse: the process's raw-input registration is re-targeted at
//     our window (RegisterRawInputDevices is per-process, per-usage, so one
//     call both routes WM_INPUT to us and SILENCES the game's raw-input reader
//     -- ER reads kb/mouse via raw input, so menu input can't leak into
//     gameplay). Close restores the captured registration, with a live-game-
//     window fallback when no prior entry was visible to the snapshot.
//   - Mouse cursor: a VIRTUAL cursor. A visible OS cursor is tracked directly;
//     during gameplay, SetCursorPos/ClipCursor detours free the hidden cursor
//     so GetCursorPos remains the native-ballistics position source. Until
//     those detours are proven active, raw deltas remain the safe fallback.
//     ImGui draws a software cursor only while the OS cursor is hidden --
//     exactly one pointer either way (see update_virtual_cursor).
//   - Gamepad: the still-focused game would act on pad presses, so the pad
//     APIs are MinHook-detoured to hand the GAME neutral input while the menu
//     is open: XInputGetState + XInputGetStateEx(#100), and -- the one ER
//     actually uses -- IDirectInputDevice8::GetDeviceState/GetDeviceData via
//     the shared dinput8 vtable. Our own polling reads the real state via the
//     XInput trampoline. (Two mods detouring the same code chain fine -- sibling
//     mods ship similar hooks -- as long as nobody unhooks mid-run; nobody does.)
//
// There is exactly ONE input mode. The old [overlay] focus_input = 1 escape
// hatch (the overlay window taking focus like a normal window) is gone with the
// window it applied to: the behavior it restored -- programmatically activating
// our window on open and the game's on close -- IS the alt-tab-freeze bug that
// the focus-free design exists to avoid. The .ini key is retired and stripped
// from the file on the next save; do not reintroduce the mode.

// Read the real pad (through the trampoline) for ImGui nav + the open combo.
void poll_gamepad() {
    g_pad_ok = false;
    if (!pXInputGetState) return;
    for (DWORD idx = 0; idx < XUSER_MAX_COUNT; ++idx) {
        XINPUT_STATE state{};
        if (pXInputGetState(idx, &state) == ERROR_SUCCESS) {
            g_pad = state.Gamepad;
            g_pad_ok = true;
            break;
        }
    }
}

// Feed the polled gamepad to ImGui nav (mouse + keyboard come in elsewhere).
void feed_gamepad() {
    if (!g_pad_ok) return;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    // Only strip the toggle-combo buttons (e.g. A+UP) from nav while that FULL
    // combo is actually held down together -- that's the one instant we must
    // stop it from also acting as a nav press. Stripping the bits unconditionally
    // permanently disables plain A (confirm) and plain DPad-Up (nav) for as long
    // as the menu stays open when either happens to be part of the open combo.
    const bool combo_held = g_open_pad_mask &&
        (g_pad.wButtons & g_open_pad_mask) == g_open_pad_mask;
    const WORD nbt = combo_held ? (g_pad.wButtons & ~g_open_pad_mask) : g_pad.wButtons;
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp,    (nbt & XINPUT_GAMEPAD_DPAD_UP) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown,  (nbt & XINPUT_GAMEPAD_DPAD_DOWN) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft,  (nbt & XINPUT_GAMEPAD_DPAD_LEFT) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (nbt & XINPUT_GAMEPAD_DPAD_RIGHT) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown,  (nbt & XINPUT_GAMEPAD_A) != 0); // A: confirm/open
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (nbt & XINPUT_GAMEPAD_B) != 0);
    // Y is intentionally NOT mapped to GamepadFaceUp: ImGui treats FaceUp as a
    // second generic "activate" input alongside A, so leaving it mapped made Y
    // toggle checkboxes too. Only A should confirm.
    io.AddKeyEvent(ImGuiKey_GamepadFaceLeft,  (nbt & XINPUT_GAMEPAD_X) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadL1,        (nbt & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadR1,        (nbt & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadStart,     (nbt & XINPUT_GAMEPAD_START) != 0);
    const float lx = g_pad.sThumbLX / 32767.0f;
    const float ly = g_pad.sThumbLY / 32767.0f;
    constexpr float DZ = 0.35f;
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft,  lx < -DZ, lx < -DZ ? -lx : 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, lx >  DZ, lx >  DZ ?  lx : 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp,    ly >  DZ, ly >  DZ ?  ly : 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown,  ly < -DZ, ly < -DZ ? -ly : 0.0f);
}

// Release every gamepad key we ever feed. Used while another app is foreground
// with the menu open: freezing feed_gamepad() there would leave the last state
// held down, and ImGui nav auto-repeats held keys (a dpad press straddling an
// alt-tab would scroll the list forever from the background).
void feed_gamepad_neutral() {
    ImGuiIO& io = ImGui::GetIO();
    for (ImGuiKey k : {ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown,
                       ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight,
                       ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight,
                       ImGuiKey_GamepadFaceLeft, ImGuiKey_GamepadL1,
                       ImGuiKey_GamepadR1, ImGuiKey_GamepadStart})
        io.AddKeyEvent(k, false);
    for (ImGuiKey k : {ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight,
                       ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown})
        io.AddKeyAnalogEvent(k, false, 0.0f);
}

// ── gamepad neutralization: XInput + DirectInput8 detours ──
// The game reads a NEUTRAL pad while the menu is open. XInput alone is NOT
// enough: with XInputGetState detoured (log: "detoured in xinput1_4.dll") ER
// still saw every pad press -- ER polls controllers through DirectInput8
// (which also explains its native DualShock support and its habit of picking
// up any DirectInput game device). So we detour BOTH: XInputGetState and
// XInputGetStateEx (ordinal 100) in every loaded xinput module, plus
// IDirectInputDevice8::GetDeviceState/GetDeviceData via their shared vtable.
// Do not "simplify" the dinput hooks away -- they are the ones that matter.

// Up to 3 xinput modules x {named export, ordinal 100}.
XInputGetState_t g_xi_real[6] = {}; // MinHook trampolines, filled by hook_xinput()

// Set for the span between "B just closed the menu" and that same physical
// press being released. Closing flips g_menu_open false the instant we see
// the press, but the controller's B button is typically still physically
// held for another poll or two -- without this, the game's very next
// GetDeviceState/XInputGetState call (right after we stop blocking) sees a
// real B-down and reads it as its own dodge/backstep input.
std::atomic<bool> g_pad_suppress_until_release{false};

bool pad_blocking() {
    return g_menu_open.load(std::memory_order_relaxed) ||
           g_pad_suppress_until_release.load(std::memory_order_relaxed);
}

template <int I>
DWORD WINAPI xi_detour(DWORD idx, XINPUT_STATE* state) {
    const DWORD r = g_xi_real[I](idx, state);
    if (r == ERROR_SUCCESS && state && pad_blocking())
        // Zero buttons/sticks/triggers; the packet number is left intact so
        // the game's "controller connected" logic never blinks.
        std::memset(&state->Gamepad, 0, sizeof(state->Gamepad));
    return r;
}

// ── DirectInput8: neutralize GetDeviceState / GetDeviceData ──
// Every device instance dinput8.dll hands out shares its vtable, so hooking
// through our own throwaway keyboard device intercepts the game's controller
// devices too. The A and W interfaces can carry distinct vtables -- both are
// resolved and hooked (deduped by address).
using DIGetDeviceState_t = HRESULT(WINAPI*)(IDirectInputDevice8W*, DWORD, LPVOID);
using DIGetDeviceData_t  = HRESULT(WINAPI*)(IDirectInputDevice8W*, DWORD,
                                            LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
DIGetDeviceState_t g_di_state_real[2] = {};
DIGetDeviceData_t  g_di_data_real[2]  = {};
std::atomic<bool>  g_dinput_hooked{false};

// Neutral joystick axes are NOT zero: the game configures each axis range
// (DIPROP_RANGE) and neutral is its midpoint -- memset(0) would slam a
// 0..65535-range stick into the top-left corner. Query once per device.
// DIJOYSTATE(2) starts with 8 contiguous LONG axes: lX..lRz, rglSlider[0..1].
struct JoyNeutral { LONG axis[8]; };
std::mutex g_di_mutex;
std::unordered_map<void*, JoyNeutral> g_joy_neutral;
std::unordered_map<void*, std::vector<uint8_t>> g_joy_frozen; // custom formats

JoyNeutral joy_neutral_for(IDirectInputDevice8W* dev) {
    {
        std::lock_guard<std::mutex> lk(g_di_mutex);
        auto it = g_joy_neutral.find(dev);
        if (it != g_joy_neutral.end()) return it->second;
    }
    JoyNeutral n{};
    for (int a = 0; a < 8; ++a) {
        DIPROPRANGE pr{};
        pr.diph.dwSize = sizeof(DIPROPRANGE);
        pr.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        pr.diph.dwObj = static_cast<DWORD>(a) * sizeof(LONG); // DIJOFS_X..
        pr.diph.dwHow = DIPH_BYOFFSET;
        // GetProperty (vtable slot 5) is unhooked -- no recursion.
        n.axis[a] = SUCCEEDED(dev->GetProperty(DIPROP_RANGE, &pr.diph))
                        ? (pr.lMin + pr.lMax) / 2 : 0;
    }
    std::lock_guard<std::mutex> lk(g_di_mutex);
    g_joy_neutral.emplace(dev, n);
    return n;
}

void di_neutralize_joy(IDirectInputDevice8W* dev, DWORD cb, void* data) {
    const JoyNeutral n = joy_neutral_for(dev);
    if (cb == sizeof(DIJOYSTATE)) {
        auto* js = static_cast<DIJOYSTATE*>(data);
        std::memcpy(&js->lX, n.axis, sizeof(n.axis));
        for (auto& pov : js->rgdwPOV) pov = 0xFFFFFFFFu; // centered
        std::memset(js->rgbButtons, 0, sizeof(js->rgbButtons));
    } else { // DIJOYSTATE2
        auto* js = static_cast<DIJOYSTATE2*>(data);
        std::memcpy(&js->lX, n.axis, sizeof(n.axis));
        for (auto& pov : js->rgdwPOV) pov = 0xFFFFFFFFu;
        std::memset(js->rgbButtons, 0, sizeof(js->rgbButtons));
        // The trailing velocity/acceleration/force blocks are deltas; zero is
        // genuinely neutral for them.
        std::memset(&js->lVX, 0,
                    static_cast<size_t>(reinterpret_cast<char*>(js + 1) -
                                        reinterpret_cast<char*>(&js->lVX)));
    }
}

template <int I>
HRESULT WINAPI di_state_detour(IDirectInputDevice8W* dev, DWORD cb, LPVOID data) {
    const HRESULT hr = g_di_state_real[I](dev, cb, data);
    if (hr != DI_OK || !data || !pad_blocking()) return hr;
    if (cb == sizeof(DIJOYSTATE) || cb == sizeof(DIJOYSTATE2)) {
        di_neutralize_joy(dev, cb, data);
    } else if (cb == 256 /* keyboard */ || cb == sizeof(DIMOUSESTATE) ||
               cb == sizeof(DIMOUSESTATE2)) {
        std::memset(data, 0, cb); // keys/buttons released, zero deltas
    } else {
        // Custom data format (layout unknown): hold the first blocked read
        // frozen -- stale-but-plausible beats zeroing unknown axes. The
        // snapshots are cleared on each menu open.
        std::lock_guard<std::mutex> lk(g_di_mutex);
        auto& snap = g_joy_frozen[dev];
        if (snap.size() != cb)
            snap.assign(static_cast<uint8_t*>(data),
                        static_cast<uint8_t*>(data) + cb);
        else
            std::memcpy(data, snap.data(), cb);
    }
    return hr;
}

template <int I>
HRESULT WINAPI di_data_detour(IDirectInputDevice8W* dev, DWORD cb_obj,
                              LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdw_inout,
                              DWORD flags) {
    const HRESULT hr = g_di_data_real[I](dev, cb_obj, rgdod, pdw_inout, flags);
    if (SUCCEEDED(hr) && pdw_inout && pad_blocking())
        *pdw_inout = 0; // "no buffered events" -- safe for every device type
    return hr;
}

// Install the dinput8 vtable hooks. Called from setup(); retried on the first
// menu open in case dinput8.dll loads after us. Idempotent.
void hook_dinput8() {
    if (g_dinput_hooked.load()) return;
    HMODULE dim = GetModuleHandleA("dinput8.dll");
    if (!dim) return; // not loaded (yet); caller retries
    using DI8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    const auto create =
        reinterpret_cast<DI8Create_t>(GetProcAddress(dim, "DirectInput8Create"));
    if (!create) return;

    void* state_fns[2] = {};
    void* data_fns[2] = {};
    int found = 0;
    const IID* iids[2] = {&IID_IDirectInput8W, &IID_IDirectInput8A};
    for (const IID* iid : iids) {
        void* dip = nullptr;
        if (FAILED(create(g_hinst, DIRECTINPUT_VERSION, *iid, &dip, nullptr)) || !dip)
            continue;
        // A and W share the method layout of the slots we touch; type as W.
        auto* di = static_cast<IDirectInput8W*>(dip);
        IDirectInputDevice8W* dev = nullptr;
        if (SUCCEEDED(di->CreateDevice(GUID_SysKeyboard, &dev, nullptr)) && dev) {
            void** vtbl = *reinterpret_cast<void***>(dev);
            void* st = vtbl[9];  // GetDeviceState
            void* da = vtbl[10]; // GetDeviceData
            bool dup = false;
            for (int i = 0; i < found; ++i)
                if (state_fns[i] == st) dup = true;
            if (!dup && found < 2) {
                state_fns[found] = st;
                data_fns[found] = da;
                ++found;
            }
            dev->Release();
        }
        di->Release(); // vtable code stays resident in dinput8.dll
    }
    if (!found) {
        flog("[overlay] [WARN] dinput8 vtable resolve failed; pad may leak while menu open");
        return;
    }

    const DIGetDeviceState_t sdet[2] = {&di_state_detour<0>, &di_state_detour<1>};
    const DIGetDeviceData_t  ddet[2] = {&di_data_detour<0>, &di_data_detour<1>};
    // Serialize create->apply against any other mod embedding this backend;
    // an interleaved install silently erases one side's detour (hooks.hpp).
    // A clobber here leaks pad input to the game while the menu is open.
    hooks::InstallLock install_lock;
    bool queued = false;
    for (int i = 0; i < found; ++i) {
        if (hooks::create(state_fns[i], reinterpret_cast<void*>(sdet[i]),
                          reinterpret_cast<void**>(&g_di_state_real[i])))
            queued = true;
        if (hooks::create(data_fns[i], reinterpret_cast<void*>(ddet[i]),
                          reinterpret_cast<void**>(&g_di_data_real[i])))
            queued = true;
    }
    if (queued && hooks::apply()) {
        g_dinput_hooked.store(true);
        flog("[overlay] dinput8 GetDeviceState/GetDeviceData detoured (%d vtable%s)",
             found, found > 1 ? "s" : "");
    } else {
        flog("[overlay] [WARN] dinput8 hook failed; pad may leak while menu open");
    }
}

// Detour XInputGetState + XInputGetStateEx(#100) in every currently-loaded
// xinput module. Idempotent; called on EVERY menu open, never at setup().
// Rationale: hooking early is what broke pad blocking -- without erdGameTools'
// init delay we hooked xinput1_4 early and dpad/LB/RB still leaked, while the
// delayed run hooked LATE and blocked everything. Hooking at first menu open
// guarantees (a) any lazily loaded xinput modules exist by then and (b) our
// detour is patched in LAST, i.e. outermost -- anything that hooked the same
// export earlier (Steam Input's emulation layer etc.) runs INSIDE us, so our
// zeroing is what the game finally receives.
void* g_xi_seen[6] = {};
int   g_xi_slot = 0;
bool  g_mh_ok = false; // hooks::init() result, set in bootstrap()/setup()

void hook_xinput() {
    if (!g_mh_ok || g_xi_slot >= 6) return;
    const char* xdlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    using Detour_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    const Detour_t detours[6] = {&xi_detour<0>, &xi_detour<1>, &xi_detour<2>,
                                 &xi_detour<3>, &xi_detour<4>, &xi_detour<5>};
    hooks::InstallLock install_lock; // see hooks.hpp -- create->apply must be atomic
    bool queued = false;
    for (const char* d : xdlls) {
        HMODULE h = GetModuleHandleA(d);
        if (!h) continue;
        // Named export + ordinal 100 (XInputGetStateEx: same signature, adds
        // the Guide button) -- a game reading Ex bypasses the named export.
        const char* names[2] = {"XInputGetState",
                                reinterpret_cast<const char*>(MAKEINTRESOURCEA(100))};
        for (int e = 0; e < 2; ++e) {
            void* fn = reinterpret_cast<void*>(GetProcAddress(h, names[e]));
            if (!fn) continue;
            bool dup = false; // 9_1_0 can forward to 1_4's export
            for (int i = 0; i < g_xi_slot; ++i)
                if (g_xi_seen[i] == fn) dup = true;
            if (dup || g_xi_slot >= 6) continue;
            if (hooks::create(fn, reinterpret_cast<void*>(detours[g_xi_slot]),
                              reinterpret_cast<void**>(&g_xi_real[g_xi_slot]))) {
                g_xi_seen[g_xi_slot] = fn;
                // If our own polling pointed at this raw proc, upgrade it to
                // the trampoline -- otherwise poll_gamepad would read through
                // our own detour and see a zeroed pad while the menu is open.
                if (!pXInputGetState ||
                    pXInputGetState == reinterpret_cast<XInputGetState_t>(fn))
                    pXInputGetState = g_xi_real[g_xi_slot];
                ++g_xi_slot;
                queued = true;
                flog("[overlay] XInputGetState%s detoured in %s",
                     e ? "Ex(#100)" : "", d);
            }
        }
    }
    if (queued && !hooks::apply())
        flog("[overlay] [WARN] MinHook apply failed; pad may leak to the game while menu open");
}

// ── cursor unpin (the real pointer speed/teleport fix) ──
// During gameplay ER re-pins the hidden OS cursor to the screen centre every
// frame via SetCursorPos (and may confine it with ClipCursor). That pin is
// WHY update_virtual_cursor's hidden-cursor branch has to integrate raw
// deltas instead of trusting GetCursorPos -- the pinned position is
// meaningless there. While OUR menu is open the game is input-starved anyway
// (see the input section up top), so the pin serves nothing: swallowing it
// frees the OS cursor, and GetCursorPos + full native Windows pointer
// ballistics (speed AND "Enhance pointer precision" acceleration) becomes the
// position source everywhere the menu is open -- the source-handover
// teleport and the integration-drift speed mismatch both disappear at once,
// because there's only one source left.
using SetCursorPos_t = BOOL(WINAPI*)(int, int);
using ClipCursor_t   = BOOL(WINAPI*)(const RECT*);
SetCursorPos_t g_real_SetCursorPos = nullptr; // MinHook trampolines
ClipCursor_t   g_real_ClipCursor = nullptr;
std::atomic<bool> g_unpin_hooked{false};

// Bumped every time a SetCursorPos warp from the game is swallowed. It being
// > 0 is PROOF the game's POSITION pinning is actually routed through our
// SetCursorPos detour (not just an assumption) -- update_virtual_cursor's
// free-cursor branch is gated on it, so if ER ever pins through some
// mechanism we didn't hook, no blocks get counted and the code silently stays
// on the pre-existing integration fallback (a stuck cursor cannot come back).
// Swallowed ClipCursor calls deliberately do NOT count: intercepting a clip
// proves nothing about how the position is pinned, and counting them once
// froze the pointer mid-screen in Fullscreen display mode (the game clipped
// through user32 -- swallowed, counted -- while re-pinning the position
// through a path our detour never saw, so GetCursorPos kept reading the
// pinned centre while we "proved" it free). Reset to 0 at each menu open.
std::atomic<uint32_t> g_scp_blocks{0};

// The game's last wanted clip rect while we were swallowing its ClipCursor
// calls, so close can replay it immediately instead of leaving one frame of
// an unclipped cursor before the game re-asserts its own pin/clip. A nullptr
// rc ("unclip") is recorded too via g_game_clip_valid = false.
std::mutex g_game_clip_mutex;
RECT g_game_clip{};
bool g_game_clip_valid = false;

BOOL WINAPI scp_detour(int x, int y) {
    if (g_unpin_hooked.load(std::memory_order_acquire) &&
        g_menu_open.load(std::memory_order_relaxed)) {
        ++g_scp_blocks;
        return TRUE; // swallow the warp -- the OS cursor stays free
    }
    return g_real_SetCursorPos(x, y);
}

BOOL WINAPI clip_detour(const RECT* rc) {
    if (g_unpin_hooked.load(std::memory_order_acquire) &&
        g_menu_open.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lk(g_game_clip_mutex);
        // Close can race a call that passed the first menu-open check. Recheck
        // under the snapshot mutex so no stale request lands after close has
        // consumed and cleared the saved clip.
        if (g_menu_open.load(std::memory_order_relaxed)) {
            if (rc) { g_game_clip = *rc; g_game_clip_valid = true; }
            else    { g_game_clip_valid = false; } // nullptr = "unclip"
            return TRUE; // swallowed, but NOT counted as pin proof (see g_scp_blocks)
        }
    }
    return g_real_ClipCursor(rc);
}

// Our OWN ClipCursor calls (confining the free cursor to the game canvas
// while the menu is open, and replaying/releasing clips during lifecycle
// changes) use the TRAMPOLINE whenever one exists. Besides bypassing the live
// detour, this keeps a partially-created hook in safe pass-through mode.
BOOL real_clip(const RECT* rc) {
    return g_real_ClipCursor ? g_real_ClipCursor(rc) : ClipCursor(rc);
}

// Install the SetCursorPos/ClipCursor detours. Same open-time, idempotent
// MinHook pattern as hook_xinput/hook_dinput8 (see hook_xinput for the
// idiom); called from the open branch of update_menu_toggle. Publish the
// combined-ready flag only after BOTH hooks are created and the queued apply
// succeeds. A partial hook stays pass-through because the detours gate on that
// flag, while later opens can create the missing half or retry hooks::apply --
// failure therefore degrades to the integration fallback, never a stuck pin.
void hook_cursor_unpin() {
    if (g_unpin_hooked.load(std::memory_order_acquire)) return;
    if (!g_mh_ok) {
        flog("[overlay] [WARN] cursor-unpin unavailable (MinHook inactive); "
             "pointer speed fix inactive");
        return;
    }
    HMODULE u32 = GetModuleHandleA("user32.dll");
    if (!u32) {
        flog("[overlay] [WARN] cursor-unpin resolve failed (user32 missing); "
             "pointer speed fix inactive");
        return;
    }
    void* scp = reinterpret_cast<void*>(GetProcAddress(u32, "SetCursorPos"));
    void* clip = reinterpret_cast<void*>(GetProcAddress(u32, "ClipCursor"));
    if (!scp || !clip) {
        flog("[overlay] [WARN] cursor-unpin resolve failed; pointer speed fix inactive");
        return;
    }
    // These locals are overlay-thread-only bookkeeping. Keeping successful
    // creates across opens avoids asking MinHook to create the same target
    // twice, and lets a failed MH_ApplyQueued be retried directly.
    static bool scp_created = false, clip_created = false;
    hooks::InstallLock install_lock; // see hooks.hpp -- create->apply must be atomic
    if (!scp_created)
        scp_created = hooks::create(
            scp, reinterpret_cast<void*>(&scp_detour),
            reinterpret_cast<void**>(&g_real_SetCursorPos));
    if (!clip_created)
        clip_created = hooks::create(
            clip, reinterpret_cast<void*>(&clip_detour),
            reinterpret_cast<void**>(&g_real_ClipCursor));

    const bool applied = hooks::apply();
    if (scp_created && clip_created && applied) {
        g_unpin_hooked.store(true, std::memory_order_release);
        flog("[overlay] SetCursorPos/ClipCursor detoured "
             "(cursor stays free while menu open)");
    } else {
        flog("[overlay] [WARN] cursor-unpin hook incomplete "
             "(SetCursorPos=%s ClipCursor=%s apply=%s); pointer speed fix inactive",
             scp_created ? "ok" : "failed",
             clip_created ? "ok" : "failed", applied ? "ok" : "failed");
    }
}

// Confinement clip we've applied to the game canvas while the menu is open
// (maintained in the overlay_thread open-frame loop). Tracked so we only call
// ClipCursor when the desired rect actually changed, not every frame.
bool g_clip_applied = false;
RECT g_clip_rect_applied{};

// ── raw-input capture (keyboard/mouse without focus) ──
// Windows keeps ONE raw-input registration per usage per process; registering
// kb + mouse with our hwnd (RIDEV_INPUTSINK -- we are never the foreground
// thread) routes WM_INPUT to us and starves whoever registered before (the
// game, or DirectInput on its behalf). The two-entry registration is NOT
// atomic, so failure immediately restores the snapshot; close restores each
// captured entry, with a live-game-window fallback for a missing usage.
std::vector<RAWINPUTDEVICE> g_rid_saved; // the game's kb/mouse (+ page-only) entries
bool g_raw_captured = false;

constexpr USHORT kHidPage = 0x01, kHidMouse = 0x02, kHidKeyboard = 0x06,
                 kHidPageOnly = 0x00; // RIDEV_PAGEONLY whole-page registration

// Matches a specific mouse/keyboard registration OR a PAGEONLY whole-page
// registration (usUsage 0) that covers them. An earlier version of this
// function ignored PAGEONLY entries entirely: some raw-input consumers
// register the whole generic-desktop page instead of mouse/keyboard
// individually, so the open-time snapshot could come back looking empty for
// a game that really did have raw input registered. An empty snapshot makes
// close-time restore degrade to RIDEV_REMOVE, which is exactly the dead-mouse
// bug (keyboard survived because ER also polls it via GetAsyncKeyState).
bool rid_is_kbm(const RAWINPUTDEVICE& r) {
    return r.usUsagePage == kHidPage &&
           (r.usUsage == kHidMouse || r.usUsage == kHidKeyboard ||
            r.usUsage == kHidPageOnly);
}

// The process's current kb/mouse (+ page-only) raw-input registrations.
// GetRegisteredRawInputDevices is two calls (query the count, then fetch); if
// another thread registers a device between them, the second call returns
// (UINT)-1 (buffer now too small) instead of the entries. Treating that as
// "nothing registered" is another way the snapshot could come back empty when
// it shouldn't. Retry the whole count+fetch a few times before giving up.
std::vector<RAWINPUTDEVICE> rid_process_kbm() {
    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT n = 0;
        if (GetRegisteredRawInputDevices(nullptr, &n, sizeof(RAWINPUTDEVICE)) ==
            static_cast<UINT>(-1))
            continue;
        if (!n) return {};
        std::vector<RAWINPUTDEVICE> all(n);
        const UINT got =
            GetRegisteredRawInputDevices(all.data(), &n, sizeof(RAWINPUTDEVICE));
        if (got == static_cast<UINT>(-1)) continue; // raced; retry count+fetch
        all.resize(got);
        std::vector<RAWINPUTDEVICE> out;
        for (const auto& r : all)
            if (rid_is_kbm(r)) out.push_back(r);
        return out;
    }
    flog("[overlay] [WARN] rid_process_kbm: GetRegisteredRawInputDevices "
         "failed/raced 3x; treating as no registrations");
    return {};
}

bool rid_register_ours() {
    RAWINPUTDEVICE rid[2] = {
        {kHidPage, kHidMouse,    RIDEV_INPUTSINK, g_input_hwnd},
        {kHidPage, kHidKeyboard, RIDEV_INPUTSINK, g_input_hwnd},
    };
    return RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE)) != FALSE;
}

// Permanent open/close-only diagnostics (never called per-frame) for the
// capture/restore dance: dumps EVERY raw-input registration the process
// currently holds, not just kb/mouse, so a bad snapshot or a botched restore
// shows up directly in the log instead of only as a "mouse is dead" report
// with nothing to go on.
void log_rid_state(const char* tag) {
    DWORD err = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT n = 0;
        if (GetRegisteredRawInputDevices(nullptr, &n,
                                         sizeof(RAWINPUTDEVICE)) ==
            static_cast<UINT>(-1)) {
            err = GetLastError();
            continue;
        }
        if (!n) {
            flog("[overlay] rawinput %s: 0 registrations", tag);
            return;
        }
        std::vector<RAWINPUTDEVICE> all(n);
        const UINT got = GetRegisteredRawInputDevices(
            all.data(), &n, sizeof(RAWINPUTDEVICE));
        if (got == static_cast<UINT>(-1)) {
            err = GetLastError();
            continue;
        }
        all.resize(got);
        for (const auto& r : all) {
            const char* who = !r.hwndTarget ? "null"
                             : r.hwndTarget == g_input_hwnd ? "ours"
                             : r.hwndTarget == g_game_hwnd ? "game"
                             : "other";
            flog("[overlay] rawinput %s: page=0x%X usage=0x%X "
                 "flags=0x%08X hwnd=%p (%s)", tag,
                 static_cast<unsigned>(r.usUsagePage),
                 static_cast<unsigned>(r.usUsage),
                 static_cast<unsigned>(r.dwFlags), r.hwndTarget, who);
        }
        return;
    }
    flog("[overlay] [WARN] rawinput %s: GetRegisteredRawInputDevices "
         "failed/raced 3x (err %lu)", tag, err);
}

// Rebuild the restore set from the snapshot and push it back to the OS. Used
// both by the normal close-time restore (raw_input_capture(false)) and by the
// open-time failure path below: RegisterRawInputDevices on our 2-entry array
// is NOT atomic, so a failed open call can still have replaced ONE of the two
// usages, and that half-stolen registration must not leak forever.
//
// For each of mouse (0x02) and keyboard (0x06):
//   - a captured SPECIFIC entry restores verbatim;
//   - no specific entry but a captured PAGEONLY entry means the game's
//     whole-page registration was never displaced -- just remove our own
//     specific registration and let the page registration keep covering it;
//   - no captured entry at all is the suspected dead-mouse-bug case: we don't
//     actually know what the game wants (an empty snapshot used to mean
//     "nothing was registered", but could just as well mean our capture code
//     didn't see it -- see rid_is_kbm/rid_process_kbm above). Leaving
//     RIDEV_REMOVE here is what produced that bug, so instead point raw input
//     at the game window directly -- the most plausible consumer -- and log
//     it. Falls back to RIDEV_REMOVE if the game window isn't alive.
// A captured PAGEONLY entry is always re-registered too (harmless no-op if it
// was never actually displaced).
bool raw_input_restore() {
    const RAWINPUTDEVICE* specific[2] = {nullptr, nullptr}; // [mouse, keyboard]
    const RAWINPUTDEVICE* pageonly = nullptr;
    for (const auto& r : g_rid_saved) {
        if (r.usUsage == kHidMouse) specific[0] = &r;
        else if (r.usUsage == kHidKeyboard) specific[1] = &r;
        else if (r.usUsage == kHidPageOnly) pageonly = &r;
    }
    const USHORT usages[2] = {kHidMouse, kHidKeyboard};
    const char* names[2] = {"mouse", "keyboard"};
    std::vector<RAWINPUTDEVICE> restore;
    for (int i = 0; i < 2; ++i) {
        if (specific[i]) {
            restore.push_back(*specific[i]);
        } else if (pageonly) {
            restore.push_back({kHidPage, usages[i], RIDEV_REMOVE, nullptr});
        } else if (g_game_hwnd && IsWindow(g_game_hwnd)) {
            restore.push_back({kHidPage, usages[i], 0, g_game_hwnd});
            flog("[overlay] [WARN] raw-input restore: no prior %s registration "
                 "was captured -- pointing raw %s at the game window",
                 names[i], names[i]);
        } else {
            restore.push_back({kHidPage, usages[i], RIDEV_REMOVE, nullptr});
        }
    }
    if (pageonly) restore.push_back(*pageonly);

    if (RegisterRawInputDevices(restore.data(), static_cast<UINT>(restore.size()),
                                sizeof(RAWINPUTDEVICE)))
        return true;
    // Per-entry fallback: one stale hwnd (game recreated its window) must not
    // block the other usage; downgrade stale entries to REMOVE -- the game
    // re-registers on demand when it next touches raw input. A PAGEONLY entry
    // must downgrade to RIDEV_REMOVE|RIDEV_PAGEONLY (usUsage 0) -- Windows
    // rejects a bare REMOVE for a page registration.
    bool ok = true;
    for (auto& r : restore) {
        if (RegisterRawInputDevices(&r, 1, sizeof(RAWINPUTDEVICE))) continue;
        const bool is_page = r.usUsage == kHidPageOnly;
        const DWORD rm_flags = is_page
            ? static_cast<DWORD>(RIDEV_REMOVE | RIDEV_PAGEONLY)
            : static_cast<DWORD>(RIDEV_REMOVE);
        RAWINPUTDEVICE rm{r.usUsagePage, r.usUsage, rm_flags, nullptr};
        if (!RegisterRawInputDevices(&rm, 1, sizeof(RAWINPUTDEVICE))) ok = false;
    }
    if (!ok) {
        flog("[overlay] [ERROR] raw-input restore failed (err %lu) -- "
             "game input may be dead; retrying", GetLastError());
        return false;
    }
    flog("[overlay] [WARN] raw-input restore downgraded to unregister");
    return true;
}

void raw_input_capture(bool on) {
    if (on == g_raw_captured) return;
    if (on) {
        g_rid_saved = rid_process_kbm();
        flog("[overlay] rawinput: captured %zu kb/mouse entr%s",
             g_rid_saved.size(), g_rid_saved.size() == 1 ? "y" : "ies");
        log_rid_state("at capture"); // right BEFORE registering ours
        if (!rid_register_ours()) {
            const DWORD capture_error = GetLastError();
            // RegisterRawInputDevices on our 2-entry array is NOT atomic -- a
            // failed call may still have replaced ONE usage. Restore right
            // away so that half-stolen registration can't leak forever (close
            // would never restore it: g_raw_captured stays false below).
            raw_input_restore();
            flog("[overlay] [WARN] raw-input capture failed (err %lu) -- "
                 "keyboard/mouse degraded while menu open", capture_error);
            return;
        }
        g_raw_captured = true;
        return;
    }
    // Restore the snapshot (see raw_input_restore for the per-usage rules);
    // g_raw_captured only clears once the restore actually lands.
    const bool restored = raw_input_restore();
    if (!restored) return; // stays "captured": the render loop retries every tick
    log_rid_state("after restore"); // right AFTER the restore completes
    g_raw_captured = false;
}

// The game (or DirectInput) can re-register raw input while our menu is open
// (device hotplug, window recreation). A captured PAGEONLY registration is
// expected to remain targeted at its original window -- our two specific
// overrides coexist with it -- so that exact entry is not a newcomer. Verify
// both specific usages explicitly, fold only changed/non-page entries into the
// restore snapshot, then re-route mouse + keyboard when either override moved.
void raw_input_reassert() {
    if (!g_raw_captured) return;
    bool mouse_ours = false, keyboard_ours = false, saw_newcomer = false;
    for (const auto& r : rid_process_kbm()) {
        if (r.hwndTarget == g_input_hwnd) {
            if (r.usUsage == kHidMouse) mouse_ours = true;
            else if (r.usUsage == kHidKeyboard) keyboard_ours = true;
            continue;
        }

        // PAGEONLY was never displaced, so seeing the exact saved entry is
        // normal. A changed page entry is a real re-registration and must
        // replace the snapshot just like a changed specific usage.
        bool expected_page = false;
        if (r.usUsage == kHidPageOnly) {
            for (const auto& s : g_rid_saved) {
                if (s.usUsagePage == r.usUsagePage &&
                    s.usUsage == r.usUsage && s.dwFlags == r.dwFlags &&
                    s.hwndTarget == r.hwndTarget) {
                    expected_page = true;
                    break;
                }
            }
        }
        if (expected_page) continue;

        saw_newcomer = true;
        bool merged = false;
        for (auto& s : g_rid_saved)
            if (s.usUsage == r.usUsage) { s = r; merged = true; }
        if (!merged) g_rid_saved.push_back(r);
    }
    if ((!mouse_ours || !keyboard_ours || saw_newcomer) &&
        !rid_register_ours())
        flog("[overlay] [WARN] raw-input re-assert failed (err %lu)", GetLastError());
}

// ── WM_INPUT -> ImGui ──
// Mouse position is a VIRTUAL cursor whose SOURCE is chosen per frame
// (update_virtual_cursor below):
//   - OS cursor VISIBLE (the game is in a menu showing a free-moving arrow --
//     raw-input starvation doesn't stop the OS pointer itself): GetCursorPos
//     is the truth and the OS arrow is the one pointer. Integrating a private
//     cursor from raw deltas alongside it made TWO diverging pointers (the
//     double-cursor bug).
//   - OS cursor HIDDEN (normal gameplay): once the SetCursorPos detour has
//     demonstrably swallowed the game's position pin (and a liveness check
//     confirms the OS position actually follows the mouse -- Fullscreen mode
//     can pin through paths a user32 hook can't see), GetCursorPos is the
//     free native-ballistics position. Without that proof, integrate raw
//     deltas so an unobserved pin can never freeze the pointer at screen centre.
// Raw input always supplies buttons/wheel (they must reach ImGui while the
// game stays starved) plus the deltas accumulated here.
float g_vmx = 0.0f, g_vmy = 0.0f;       // virtual cursor, canvas coords
float g_raw_dx = 0.0f, g_raw_dy = 0.0f; // relative deltas since last frame
float g_raw_ax = -1.0f, g_raw_ay = -1.0f; // pending absolute pos (screen px), <0 = none

// Windows pointer-speed multiplier applied to integrated raw deltas.
// Recomputed once at each menu open by mouse_speed_multiplier(); left at 1.0
// (Control Panel default) until the first open.
float g_mouse_scale = 1.0f;

void raw_mouse(const RAWMOUSE& m, ImGuiIO& io) {
    if (m.usFlags & MOUSE_MOVE_ABSOLUTE) {
        // Absolute device (tablet / touch / RDP): coords are 0..65535 across
        // the (virtual) desktop; convert to screen pixels for the next frame.
        const bool virt = (m.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
        const int vx = virt ? GetSystemMetrics(SM_XVIRTUALSCREEN) : 0;
        const int vy = virt ? GetSystemMetrics(SM_YVIRTUALSCREEN) : 0;
        const int vw = GetSystemMetrics(virt ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
        const int vh = GetSystemMetrics(virt ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
        g_raw_ax = static_cast<float>(vx) + m.lLastX / 65535.0f * static_cast<float>(vw);
        g_raw_ay = static_cast<float>(vy) + m.lLastY / 65535.0f * static_cast<float>(vh);
    } else if (m.lLastX != 0 || m.lLastY != 0) {
        g_raw_dx += static_cast<float>(m.lLastX);
        g_raw_dy += static_cast<float>(m.lLastY);
    }
    const USHORT f = m.usButtonFlags;
    if (f & RI_MOUSE_LEFT_BUTTON_DOWN)   io.AddMouseButtonEvent(0, true);
    if (f & RI_MOUSE_LEFT_BUTTON_UP)     io.AddMouseButtonEvent(0, false);
    if (f & RI_MOUSE_RIGHT_BUTTON_DOWN)  io.AddMouseButtonEvent(1, true);
    if (f & RI_MOUSE_RIGHT_BUTTON_UP)    io.AddMouseButtonEvent(1, false);
    if (f & RI_MOUSE_MIDDLE_BUTTON_DOWN) io.AddMouseButtonEvent(2, true);
    if (f & RI_MOUSE_MIDDLE_BUTTON_UP)   io.AddMouseButtonEvent(2, false);
    if (f & RI_MOUSE_BUTTON_4_DOWN)      io.AddMouseButtonEvent(3, true);
    if (f & RI_MOUSE_BUTTON_4_UP)        io.AddMouseButtonEvent(3, false);
    if (f & RI_MOUSE_BUTTON_5_DOWN)      io.AddMouseButtonEvent(4, true);
    if (f & RI_MOUSE_BUTTON_5_UP)        io.AddMouseButtonEvent(4, false);
    if (f & RI_MOUSE_WHEEL)
        io.AddMouseWheelEvent(0.0f,
            static_cast<float>(static_cast<SHORT>(m.usButtonData)) / WHEEL_DELTA);
    if (f & RI_MOUSE_HWHEEL)
        io.AddMouseWheelEvent(
            -static_cast<float>(static_cast<SHORT>(m.usButtonData)) / WHEEL_DELTA, 0.0f);
}

// Forward a raw key as a synthetic WM_KEY* to the ImGui Win32 backend (reuses
// its full VK -> ImGuiKey mapping), then translate printable keys ourselves.
// A message-only HWND can receive WM_INPUT but never the normal WM_CHAR stream;
// forwarding a synthetic WM_CHAR back through the HWND backend left InputText
// dependent on focus state that this window can never own. Feed UTF-16 straight
// into this private ImGui context instead. IME still requires a real focused
// text service, but normal layout/dead-key text works without activating or
// deactivating the game window.
uint32_t g_raw_key_events = 0;
uint32_t g_text_characters = 0;
bool g_search_activated = false;
bool g_imgui_focus_known = false;
bool g_imgui_focused = false;

void raw_keyboard(const RAWKEYBOARD& k) {
    if (k.VKey == 0 || k.VKey >= 255) return; // fake keys / overrun marker
    ++g_raw_key_events;
    const bool down = (k.Flags & RI_KEY_BREAK) == 0;
    const UINT scan = k.MakeCode & 0xFFu;
    LPARAM lp = 1 | (static_cast<LPARAM>(scan) << 16);
    if (k.Flags & RI_KEY_E0) lp |= static_cast<LPARAM>(1) << 24;
    if (!down) lp |= (static_cast<LPARAM>(1) << 30) | (static_cast<LPARAM>(1) << 31);
    ImGui_ImplWin32_WndProcHandler(g_input_hwnd, down ? WM_KEYDOWN : WM_KEYUP,
                                   k.VKey, lp);

    if (!down) return;

    // Ctrl/Alt shortcuts must not emit text. Right-Alt (AltGr) is the exception:
    // Windows exposes it as Ctrl+Alt and many European layouts need it.
    const bool ctrl = kd(VK_CONTROL);
    const bool alt = kd(VK_MENU);
    const bool altgr = kd(VK_RMENU) && ctrl;
    if ((ctrl || alt) && !altgr) return;

    BYTE ks[256] = {};
    const int live_keys[] = {
        VK_SHIFT, VK_LSHIFT, VK_RSHIFT,
        VK_CONTROL, VK_LCONTROL, VK_RCONTROL,
        VK_MENU, VK_LMENU, VK_RMENU
    };
    for (const int vk : live_keys)
        if (kd(vk)) ks[vk] = 0x80;
    // ToUnicodeEx expects the key being translated to be down even though this
    // thread never receives a conventional WM_KEYDOWN for it.
    ks[k.VKey] |= 0x80;
    if (GetKeyState(VK_CAPITAL) & 1) ks[VK_CAPITAL] |= 0x01;
    if (GetKeyState(VK_NUMLOCK) & 1) ks[VK_NUMLOCK] |= 0x01;
    if (GetKeyState(VK_SCROLL) & 1) ks[VK_SCROLL] |= 0x01;

    DWORD game_tid = 0;
    if (g_game_hwnd) game_tid = GetWindowThreadProcessId(g_game_hwnd, nullptr);
    const HKL layout = GetKeyboardLayout(game_tid);
    UINT unicode_scan = scan;
    if (k.Flags & RI_KEY_E0) unicode_scan |= 0xE000u;
    WCHAR buf[8] = {};
    const int n = ToUnicodeEx(k.VKey, unicode_scan, ks, buf,
                              static_cast<int>(sizeof(buf) / sizeof(buf[0])), 0, layout);
    if (n <= 0) return; // n < 0 leaves a dead key ready for the next character
    ImGuiIO& io = ImGui::GetIO();
    for (int i = 0; i < n; ++i) {
        if (buf[i] < 32) continue;
        io.AddInputCharacterUTF16(static_cast<ImWchar16>(buf[i]));
        ++g_text_characters;
    }
}

// The message-only input sink's WM_INPUT handler.
bool g_context_ready = false; // ImGui context exists (set once init succeeds)

void handle_raw_input(HRAWINPUT h) {
    // Only feed ImGui while it will consume events next frame; a failed
    // restore with the menu closed must not grow the io queue unboundedly.
    if (!g_context_ready || !g_raw_captured || !g_menu_open.load()) return;
    // RIDEV_INPUTSINK keeps delivering while another app is foreground
    // (alt-tab / second monitor); that input belongs to the other app -- eating
    // it would move our virtual cursor and scroll the list from the background.
    if (!foreground_is_ours()) return;
    RAWINPUT ri{};
    UINT sz = sizeof(ri);
    if (GetRawInputData(h, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) ==
        static_cast<UINT>(-1))
        return;
    if (ri.header.dwType == RIM_TYPEMOUSE)
        raw_mouse(ri.data.mouse, ImGui::GetIO());
    else if (ri.header.dwType == RIM_TYPEKEYBOARD)
        raw_keyboard(ri.data.keyboard);
}

// Process-wide name: it lives in overlay_coexist.hpp with everything else a
// second copy of this backend would collide on.
constexpr const wchar_t* kInputWindowClass = coexist::kInputWindowClass;

LRESULT CALLBACK input_wndproc(HWND hwnd, UINT message,
                               WPARAM wparam, LPARAM lparam) {
    if (message == WM_INPUT) {
        handle_raw_input(reinterpret_cast<HRAWINPUT>(lparam));
        // Required by the raw-input contract for foreground packets; harmless
        // for RIDEV_INPUTSINK packets.
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    if (message == WM_DESTROY) return 0;
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool create_input_window() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = input_wndproc;
    wc.hInstance = g_hinst;
    wc.lpszClassName = kInputWindowClass;
    const ATOM atom = RegisterClassExW(&wc);
    if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    g_input_hwnd = CreateWindowExW(
        0, kInputWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
        nullptr, g_hinst, nullptr);
    return g_input_hwnd != nullptr;
}

void destroy_input_window() {
    if (g_input_hwnd) {
        DestroyWindow(g_input_hwnd);
        g_input_hwnd = nullptr;
    }
    UnregisterClassW(kInputWindowClass, g_hinst);
}

// Refresh the control-thread coordinate mapping from the selected swapchain.
// Backbuffer pixels and client pixels can differ under compatibility scaling;
// cursor conversion accounts for that instead of assuming a 1:1 mapping.
// Why the last refresh_canvas() failed, so the user-visible "menu requested
// before a validated game swapchain was ready" line is triage-grade on its own.
// Control thread only.
const char* g_canvas_block_reason = "not evaluated yet";

bool refresh_canvas() {
    const present::Canvas host = present::canvas();
    if (!host.ready || !host.hwnd || !IsWindow(host.hwnd) ||
        host.width < 2 || host.height < 2) {
        g_canvas_block_reason =
            !host.ready      ? "the backend has not published a ready canvas"
            : !host.hwnd     ? "the published canvas has no window"
            : !IsWindow(host.hwnd) ? "the published canvas window is gone"
                             : "the published canvas is degenerate";
        g_game_hwnd = nullptr;
        g_back_w = g_back_h = 0;
        g_client_w = g_client_h = 0;
        return false;
    }

    RECT client{};
    POINT origin{};
    if (!GetClientRect(host.hwnd, &client) || !ClientToScreen(host.hwnd, &origin)) {
        g_canvas_block_reason = "the game window geometry is unreadable";
        return false;
    }
    const int client_w = client.right - client.left;
    const int client_h = client.bottom - client.top;
    if (client_w < 1 || client_h < 1) {
        g_canvas_block_reason = "the game client rect is empty";
        return false;
    }
    g_canvas_block_reason = nullptr;

    g_game_hwnd = host.hwnd;
    g_back_w = host.width;
    g_back_h = host.height;
    g_canvas_pos = origin;
    g_client_w = client_w;
    g_client_h = client_h;
    return true;
}

// Does a plausible game window already exist in this process? Used to detect
// a late load (the window -- and therefore the swapchain -- predates this
// mod's DXGI creation hooks) and to pick the display for the adoption path's
// vendor HDR query. Returns the largest qualifying window.
HWND find_existing_game_window() {
    struct Search {
        HWND best = nullptr;
        int best_area = 0;
    } search;
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* search = reinterpret_cast<Search*>(param);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) ||
                GetAncestor(hwnd, GA_ROOT) != hwnd ||
                (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0)
                return TRUE;
            RECT client{};
            if (!GetClientRect(hwnd, &client)) return TRUE;
            const int width = client.right - client.left;
            const int height = client.bottom - client.top;
            if (width < 640 || height < 360) return TRUE;
            if (width * height > search->best_area) {
                search->best_area = width * height;
                search->best = hwnd;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.best;
}

bool init_frontend() {
    IMGUI_CHECKVERSION();
    g_imgui_context = ImGui::CreateContext();
    if (!g_imgui_context) return false;
    ImGui::SetCurrentContext(g_imgui_context);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // never create imgui.ini
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard |
                      ImGuiConfigFlags_NavEnableGamepad |
                      ImGuiConfigFlags_NoMouseCursorChange;
    ui::load_fonts(io);
    ui::apply_style();
    if (!ImGui_ImplWin32_Init(g_input_hwnd)) return false;
    present::publish_font_atlas(io.Fonts);
    return present::renderer_healthy();
}

void shutdown_frontend() {
    g_context_ready = false;
    if (!g_imgui_context) return;
    ImGui::SetCurrentContext(g_imgui_context);
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(g_imgui_context);
    g_imgui_context = nullptr;
}

// Feed modifier state from the hardware. The win32 backend derives modifiers
// from GetKeyState, which only updates for threads that receive real keyboard
// messages -- ours never does.
void update_modifiers() {
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl,  kd(VK_CONTROL));
    io.AddKeyEvent(ImGuiMod_Shift, kd(VK_SHIFT));
    io.AddKeyEvent(ImGuiMod_Alt,   kd(VK_MENU));
    io.AddKeyEvent(ImGuiMod_Super, kd(VK_LWIN) || kd(VK_RWIN));
}

// ── the pointer, single-image by construction ──
// Every frame while the menu is open: pick the position source and decide who
// draws the pointer image (see the WM_INPUT comment above):
//   OS cursor visible -> its position is the truth and it IS the pointer
//                        (game-menu arrow, moving freely); software cursor off.
//   OS cursor hidden  -> use its position after the pin detours are proven;
//                        otherwise integrate raw deltas as the safe fallback.
//                        ImGui's software cursor supplies the pointer image.
// WM_SETCURSOR forces no state in this non-legacy path, so crossing the panel
// never changes the position source. Never both cursor images.
bool g_soft_cursor = false;

// Windows' own mouse-speed slider (Control Panel > Mouse > Pointer Options)
// scales raw device counts by a documented, non-linear table before applying
// pointer ballistics; SPI_GETMOUSESPEED only exposes the 1-20 slider
// position, so we replicate the linear-interpolation part of that table
// here for the integration fallback in update_virtual_cursor. Acceleration
// ("Enhance pointer precision") is deliberately NOT replicated: with the
// cursor-unpin hook above, integration is only a fallback path (used when the
// unpin hook didn't land, or before the game's first pin this session), so
// matching ballistics exactly isn't worth the complexity -- the unpinned
// GetCursorPos path gets full native ballistics for free.
float mouse_speed_multiplier() {
    int speed = 10;
    SystemParametersInfo(SPI_GETMOUSESPEED, 0, &speed, 0);
    struct Pt { int speed; float mult; };
    static const Pt table[] = {
        {1, 0.03125f}, {2, 0.0625f}, {4, 0.25f}, {6, 0.5f}, {8, 0.75f},
        {10, 1.0f}, {12, 1.5f}, {14, 2.0f}, {16, 2.5f}, {18, 3.0f}, {20, 3.5f},
    };
    constexpr int n = sizeof(table) / sizeof(table[0]);
    if (speed <= table[0].speed) return table[0].mult;
    if (speed >= table[n - 1].speed) return table[n - 1].mult;
    for (int i = 0; i + 1 < n; ++i) {
        if (speed >= table[i].speed && speed <= table[i + 1].speed) {
            const float t = static_cast<float>(speed - table[i].speed) /
                            static_cast<float>(table[i + 1].speed - table[i].speed);
            return table[i].mult + (table[i + 1].mult - table[i].mult) * t;
        }
    }
    return 1.0f; // unreachable given the clamps above
}

// Liveness cross-check for the free-cursor path: g_scp_blocks > 0 proves the
// game's SetCursorPos warps route through our detour, but NOT that nothing
// else still pins the position (NtUserSetCursorPos-style paths are invisible
// to a user32 hook). So while trusting GetCursorPos with the cursor hidden,
// compare accumulated raw mouse deltas against how far the OS position
// actually moved: real mouse motion with a frozen OS position = some unhooked
// pin is winning -> stop trusting GetCursorPos for the rest of this open and
// fall back to raw-delta integration (the virtual cursor keeps working from
// wherever it is). Thresholds are generous so the slowest Windows pointer
// speed (x0.03) can't false-positive. All state reset at each menu open.
float g_live_raw = 0.0f;    // |raw deltas| accumulated on the trusted path
float g_live_moved = 0.0f;  // |GetCursorPos movement| over the same frames
POINT g_live_last{};
bool  g_live_seen = false;
bool  g_free_cursor_dead = false; // tripped: integration only, this open

// Per-open cursor-source telemetry (overlay thread only). Count after the final
// source decision, so a frame that trips the trusted-source liveness check is
// correctly attributed to integration rather than to the failed trusted path.
int g_cursor_os_visible_frames = 0;
int g_cursor_free_trusted_frames = 0;
int g_cursor_integration_frames = 0;

void update_virtual_cursor(bool fg_ours) {
    if (!fg_ours) {
        // Alt-tabbed / other monitor: the OS cursor belongs to the other app.
        // Freeze our pointer (raw input is already ignored, see handle_raw_input)
        // and drop queued motion so it doesn't replay on return.
        g_raw_dx = g_raw_dy = 0.0f;
        g_raw_ax = g_raw_ay = -1.0f;
        g_soft_cursor = false;
        return;
    }
    const float maxx = g_back_w > 1 ? g_back_w - 1.0f : 0.0f;
    const float maxy = g_back_h > 1 ? g_back_h - 1.0f : 0.0f;
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    const bool os_visible = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) != 0;
    POINT p{};
    bool use_os_pos = false;
    if (os_visible && GetCursorPos(&p)) {
        // Free cursor: track it and keep the fallback integrator seeded so a
        // later game-driven hide cannot create a position discontinuity.
        use_os_pos = true;
        g_live_seen = false; // liveness only judges the hidden-cursor path
        g_live_raw = g_live_moved = 0.0f;
    } else if (g_unpin_hooked.load() && g_scp_blocks.load() > 0 &&
               !g_free_cursor_dead && GetCursorPos(&p)) {
        // The game's cursor warps are being swallowed (g_scp_blocks > 0 is
        // PROOF its position pinning is actually routed through our
        // SetCursorPos detour, not just an assumption -- see
        // hook_cursor_unpin), so the hidden OS cursor should be FREE here too
        // and GetCursorPos the truth: full native pointer ballistics (speed
        // AND acceleration), no integration drift, no visible<->hidden
        // handover teleport. "Should be": the liveness check below catches a
        // pin arriving through a path our detour can't see (it did in
        // Fullscreen display mode) and demotes this open to integration.
        // Only judge frames where the cursor sits INSIDE the canvas: pressed
        // against an edge, our own confinement clip legitimately stops the OS
        // position while deltas keep arriving -- that must not read as a pin.
        // ER's pin target is the screen centre, i.e. always interior.
        const bool interior =
            p.x > g_canvas_pos.x + 2 &&
            p.x < g_canvas_pos.x + g_client_w - 3 &&
            p.y > g_canvas_pos.y + 2 &&
            p.y < g_canvas_pos.y + g_client_h - 3;
        if (interior) {
            g_live_raw += std::fabs(g_raw_dx) + std::fabs(g_raw_dy);
            if (g_live_seen)
                g_live_moved += std::fabs(static_cast<float>(p.x - g_live_last.x)) +
                                std::fabs(static_cast<float>(p.y - g_live_last.y));
            g_live_last = p;
            g_live_seen = true;
            if (g_live_raw >= 200.0f) {
                if (g_live_moved < 4.0f) {
                    g_free_cursor_dead = true;
                    flog("[overlay] [WARN] free-cursor check failed (mouse moved, "
                         "OS cursor didn't -- an unhooked pin is active); using "
                         "raw-delta integration for this open");
                }
                g_live_raw = g_live_moved = 0.0f;
            }
        } else {
            g_live_seen = false; // don't measure movement across an edge stay
        }
        use_os_pos = !g_free_cursor_dead;
    }
    if (use_os_pos) {
        if (os_visible) ++g_cursor_os_visible_frames;
        else ++g_cursor_free_trusted_frames;
        const float sx = g_client_w > 0 ? static_cast<float>(g_back_w) / g_client_w : 1.0f;
        const float sy = g_client_h > 0 ? static_cast<float>(g_back_h) / g_client_h : 1.0f;
        g_vmx = std::clamp(static_cast<float>(p.x - g_canvas_pos.x) * sx, 0.0f, maxx);
        g_vmy = std::clamp(static_cast<float>(p.y - g_canvas_pos.y) * sy, 0.0f, maxy);
        g_raw_dx = g_raw_dy = 0.0f; // drain so nothing replays on a source switch
        g_raw_ax = g_raw_ay = -1.0f;
    } else {
        ++g_cursor_integration_frames;
        // Fallback: the unpin hook never landed, g_scp_blocks is still 0 (the
        // game hasn't tried to warp yet this open, or warps through a
        // mechanism we didn't hook), or the liveness check tripped --
        // integrate raw deltas, scaled by g_mouse_scale to match the user's
        // configured Windows pointer speed. The absolute-device branch is NOT
        // scaled -- those are positions, not deltas.
        if (g_raw_ax >= 0.0f) { // absolute device (tablet / RDP) moved last
            const float sx = g_client_w > 0 ? static_cast<float>(g_back_w) / g_client_w : 1.0f;
            const float sy = g_client_h > 0 ? static_cast<float>(g_back_h) / g_client_h : 1.0f;
            g_vmx = std::clamp((g_raw_ax - static_cast<float>(g_canvas_pos.x)) * sx, 0.0f, maxx);
            g_vmy = std::clamp((g_raw_ay - static_cast<float>(g_canvas_pos.y)) * sy, 0.0f, maxy);
            g_raw_ax = g_raw_ay = -1.0f;
        } else {
            g_vmx = std::clamp(g_vmx + g_raw_dx * g_mouse_scale, 0.0f, maxx);
            g_vmy = std::clamp(g_vmy + g_raw_dy * g_mouse_scale, 0.0f, maxy);
        }
        g_raw_dx = g_raw_dy = 0.0f;
    }
    ImGui::GetIO().AddMousePosEvent(g_vmx, g_vmy);
    g_soft_cursor = !os_visible;
}

// ── open/close handling (polled on the overlay thread) ──
void update_menu_toggle() {
    const bool ours = foreground_is_ours();

    static bool prev_key_in = false;
    const bool key_in = ours && kd(static_cast<int>(g_open_vk));
    bool toggled = key_in && !prev_key_in;
    prev_key_in = key_in;

    const bool combo_in = ours && g_pad_ok && g_open_pad_mask &&
                          (g_pad.wButtons & g_open_pad_mask) == g_open_pad_mask;
    if (g_open_pad_is_hold) {
        static bool combo_active = false, combo_fired = false;
        static std::chrono::steady_clock::time_point combo_since;
        if (combo_in) {
            if (!combo_active) {
                combo_active = true;
                combo_fired = false;
                combo_since = std::chrono::steady_clock::now();
            } else if (!combo_fired) {
                const auto held_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - combo_since).count();
                if (held_ms >= kPadHoldMs) { toggled = true; combo_fired = true; }
            }
        } else {
            combo_active = false;
            combo_fired = false;
        }
    } else {
        static bool prev_combo_in = false;
        if (combo_in && !prev_combo_in) toggled = true;
        prev_combo_in = combo_in;
    }

    if (toggled) {
        const bool next_open = !g_menu_open.load();
        if (next_open && !refresh_canvas()) {
            static ULONGLONG last_not_ready_log = 0;
            const ULONGLONG now = GetTickCount64();
            if (now - last_not_ready_log > 2000) {
                flog("[overlay-v2] menu requested before a validated game "
                     "swapchain was ready: %s",
                     g_canvas_block_reason ? g_canvas_block_reason
                                           : "reason unrecorded");
                last_not_ready_log = now;
            }
            toggled = false;
        }
        // Only one overlay in the process may capture input at a time: the
        // raw-input registration is a per-process singleton and each overlay's
        // close restores the snapshot it took at open. Two open at once means
        // the second snapshotted the first's sink hwnd, and a close-order
        // inversion hands the game a dead sink -- no keyboard, no mouse. Take
        // ownership BEFORE publishing the open so a refusal leaves the state
        // machine untouched (overlay_coexist.hpp).
        if (toggled && next_open && !coexist::acquire_menu_ownership()) {
            static ULONGLONG last_busy_log = 0;
            const ULONGLONG now = GetTickCount64();
            if (now - last_busy_log > 2000) {
                flog("[overlay] another overlay in this process has its menu "
                     "open; refusing to open (close it first)");
                last_busy_log = now;
            }
            toggled = false;
        }
        // Cursor hooks persist after their first install. Reset the proof
        // counter BEFORE publishing a later open, or a game thread can enter
        // an already-live detour and have its first block erased afterwards.
        if (toggled && next_open) {
            g_scp_blocks.store(0, std::memory_order_relaxed);
            // Fresh liveness verdict each open (display mode / pin behavior
            // can change between opens).
            g_free_cursor_dead = false;
            g_live_seen = false;
            g_live_raw = g_live_moved = 0.0f;
        }
        if (toggled) g_menu_open.store(next_open);
    }

    static bool prev_esc = false, prev_padb = false;
    const bool esc = ours && kd(VK_ESCAPE);
    const bool padb = ours && g_pad_ok && (g_pad.wButtons & XINPUT_GAMEPAD_B) != 0;
    // g_pad is read through the trampoline (real, unblocked state -- see
    // poll_gamepad), so this sees the physical button release even while the
    // game's own reads are still being neutralized.
    if (!padb) g_pad_suppress_until_release.store(false);
    if (g_menu_open.load() && ((esc && !prev_esc) || (padb && !prev_padb))) {
        g_menu_open.store(false);
        // This same B press closed the menu; keep the pad blocked until it's
        // physically released so it can't also register as a dodge/backstep.
        if (padb) g_pad_suppress_until_release.store(true);
    }
    prev_esc = esc;
    prev_padb = padb;

    static bool prev_open = false;
    const bool open_now = g_menu_open.load();
    if (open_now && !prev_open) {
        // Opening changes only input routing plus one atomic render-visible bit.
        // There is no top-level HWND to show, activate, resize, or place in the
        // z-order, and no DXGI object is recreated.
        refresh_canvas();
        g_cursor_os_visible_frames = 0;
        g_cursor_free_trusted_frames = 0;
        g_cursor_integration_frames = 0;
        g_raw_key_events = 0;
        g_text_characters = 0;
        g_search_activated = false;
        g_imgui_focus_known = false;
        ui::update_scale(static_cast<int>(g_back_h));
        hook_xinput();       // install/refresh pad detours LAST-hooked = outermost
        hook_dinput8();      // no-op if already hooked; retry if dinput8 loaded late
        hook_cursor_unpin(); // no-op if already hooked (unpins the OS cursor)
        { // fresh freeze-frames for custom-format devices this session
            std::lock_guard<std::mutex> lk(g_di_mutex);
            g_joy_frozen.clear();
        }
        raw_input_capture(true);
        g_mouse_scale = mouse_speed_multiplier(); // match Windows pointer speed
        // Seed the virtual cursor from the OS position (during gameplay the
        // game parks the hidden cursor mid-screen -- a fine starting spot);
        // update_virtual_cursor drives it from here.
        POINT p{};
        if (GetCursorPos(&p)) {
            const float sx = g_client_w > 0 ? static_cast<float>(g_back_w) / g_client_w : 1.0f;
            const float sy = g_client_h > 0 ? static_cast<float>(g_back_h) / g_client_h : 1.0f;
            g_vmx = std::clamp(static_cast<float>(p.x - g_canvas_pos.x) * sx, 0.0f,
                               g_back_w > 1 ? g_back_w - 1.0f : 0.0f);
            g_vmy = std::clamp(static_cast<float>(p.y - g_canvas_pos.y) * sy, 0.0f,
                               g_back_h > 1 ? g_back_h - 1.0f : 0.0f);
        }
        g_raw_dx = g_raw_dy = 0.0f;
        g_raw_ax = g_raw_ay = -1.0f;
        present::set_visible(true);
    } else if (!open_now && prev_open) {
        present::set_visible(false);
        const uint32_t final_scp_blocks =
            g_scp_blocks.load(std::memory_order_relaxed);
        flog("[overlay-v2] menu closed; cursor sources: %d os-visible, %d free-trusted, "
             "%d integration, scp_blocks=%u, free_cursor_dead=%s; "
             "input: raw_keys=%u, text_chars=%u, search_active=%s",
             g_cursor_os_visible_frames, g_cursor_free_trusted_frames,
             g_cursor_integration_frames, final_scp_blocks,
             g_free_cursor_dead ? "yes" : "no", g_raw_key_events,
             g_text_characters, g_search_activated ? "yes" : "no");
        raw_input_capture(false); // give the game its raw input back
        // Cursor confinement teardown. Order vs raw_input_capture(false) above
        // doesn't matter functionally -- kept together here as one tidy close
        // block. Release our clip, replaying whatever the game last asked for
        // while we were swallowing its ClipCursor calls (it re-asserts its own
        // pin/clip within a frame once its calls pass through again, but
        // replaying immediately avoids a stray frame of a wide-open cursor).
        {
            RECT replay{};
            bool have_replay = false;
            {
                std::lock_guard<std::mutex> lk(g_game_clip_mutex);
                have_replay = g_game_clip_valid;
                if (have_replay) replay = g_game_clip;
                g_game_clip_valid = false;
            }
            if (!real_clip(have_replay ? &replay : nullptr))
                flog("[overlay] [WARN] cursor clip restore failed (err %lu)",
                     GetLastError());
            g_clip_applied = false;
            g_scp_blocks.store(0, std::memory_order_relaxed);
        }
        ui::reset_on_close();
        // Last: raw input and the cursor clip are back with the game above, so
        // another overlay taking ownership now snapshots the GAME's state, not
        // ours (overlay_coexist.hpp).
        coexist::release_menu_ownership();
        std::lock_guard<std::mutex> l(g_state_mutex);
        g_state.save_requested = true; // auto-save selections on close
    }
    prev_open = open_now;
}

// ── the overlay thread ──
void overlay_thread() {
    if (!create_input_window()) {
        flog("[overlay-v2] [ERROR] message-only input window creation failed");
        return;
    }
    if (!init_frontend()) {
        flog("[overlay-v2] [ERROR] ImGui frontend initialization failed");
        return;
    }
    g_context_ready = true;
    ImGui::SetCurrentContext(g_imgui_context);

    bool ever_had_canvas = false;
    bool canvas_loss_reported = false;
    ULONGLONG canvas_lost_since = 0;
    ULONGLONG open_unavailable_since = 0;
    UINT last_scaled_height = 0;

    // Swapchain discovery is creation-first: the DXGI creation detours are
    // the authoritative path (they capture the exact queue at creation). But
    // that call can be missed -- a delayed loader/injector starts this mod
    // after the game is already presenting, or a peer mod's unserialized
    // install erases the creation hook (hooks.hpp). Both are recovered by
    // present::arm_adoption(), which finds the live swapchain from
    // pass-through Present observers and shadows it in place. Trigger it
    // immediately when the game window predates the hooks, or from a 10s
    // watchdog otherwise; forensics lines attribute the cause either way.
    const ULONGLONG discovery_started = GetTickCount64();
    bool discovery_stall_handled = false;
    bool canvas_loss_adoption_armed = false;
    // A visible game window here proves nothing on its own: bootstrap does
    // config and flag-scanner work between installing the hooks and reaching
    // this point, so the game can legitimately create AND present its
    // swapchain in that gap. Only a window with no observed creation is a real
    // late load. Arming adoption otherwise costs three extra process-wide
    // detours and can never help anyway -- try_adopt_swapchain rejects any
    // chain that already carries a shadow.
    if (HWND existing = find_existing_game_window()) {
        if (present::observed_swapchain_creation()) {
            flog("[overlay-v2] game window already up, but the overlay observed "
                 "its swapchain creation; adoption not needed");
        } else {
            flog("[overlay-v2] late load detected: the game window predates "
                 "this mod's DXGI hooks; adopting the live swapchain");
            present::log_creation_hook_forensics();
            present::arm_adoption(existing);
            discovery_stall_handled = true;
        }
    }

    while (g_running.load(std::memory_order_acquire)) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        const bool canvas_valid = refresh_canvas();
        if (canvas_valid) {
            ever_had_canvas = true;
            canvas_lost_since = 0;
            canvas_loss_adoption_armed = false;
            if (canvas_loss_reported) {
                flog("[overlay-v2] game swapchain/window reacquired");
                canvas_loss_reported = false;
            }
        } else if (ever_had_canvas) {
            if (!canvas_lost_since) canvas_lost_since = GetTickCount64();
            if (GetTickCount64() - canvas_lost_since > 3000 &&
                !canvas_loss_reported) {
                flog("[overlay-v2] game swapchain/window unavailable; waiting for replacement");
                g_menu_open.store(false, std::memory_order_release);
                canvas_loss_reported = true;
            }
            // A replacement swapchain normally re-registers through the
            // creation detour within a frame or two. If it still has not
            // after 5s, assume the creation call was missed again (dead
            // creation hook) and search the live presents instead.
            if (GetTickCount64() - canvas_lost_since > 5000 &&
                !canvas_loss_adoption_armed) {
                canvas_loss_adoption_armed = true;
                flog("[overlay-v2] replacement swapchain never registered; "
                     "engaging live-swapchain adoption");
                present::log_creation_hook_forensics();
                present::arm_adoption(find_existing_game_window());
            }
        } else if (!discovery_stall_handled &&
                   GetTickCount64() - discovery_started > 10000) {
            discovery_stall_handled = true;
            if (present::observed_swapchain_creation()) {
                // Discovery worked; validation or presentation did not. The
                // one-shot backend diagnostics name the gate -- adoption
                // cannot help, because the chain is already shadowed.
                flog("[overlay-v2] [WARN] no usable canvas 10s after "
                     "bootstrap even though the overlay observed and shadowed "
                     "the swapchain creation -- validation is rejecting it");
            } else {
                flog("[overlay-v2] [WARN] no game swapchain discovered 10s "
                     "after bootstrap -- this mod's DXGI creation detour "
                     "never saw the creation call; engaging live-swapchain "
                     "adoption");
                present::log_creation_hook_forensics();
                present::arm_adoption(find_existing_game_window());
            }
        }

        poll_gamepad();
        update_menu_toggle();

        if (g_menu_open.load(std::memory_order_acquire)) {
            if (!canvas_valid || !present::renderer_healthy()) {
                const ULONGLONG now = GetTickCount64();
                if (!open_unavailable_since) open_unavailable_since = now;
                if (now - open_unavailable_since > 1500) {
                    flog("[overlay-v2] renderer/canvas unavailable for 1.5s; restoring input");
                    g_menu_open.store(false, std::memory_order_release);
                }
                Sleep(8);
                continue;
            }
            open_unavailable_since = 0;

            // Re-apply automatic UI scaling only when the actual backbuffer
            // height changes (a mid-session resolution/display-mode switch).
            // This mod never causes that resize.
            if (last_scaled_height != g_back_h) {
                ui::update_scale(static_cast<int>(g_back_h));
                last_scaled_height = g_back_h;
            }

            const bool foreground = foreground_is_ours();
            if (foreground) {
                const RECT wanted{
                    g_canvas_pos.x,
                    g_canvas_pos.y,
                    g_canvas_pos.x + g_client_w,
                    g_canvas_pos.y + g_client_h};
                if (!g_clip_applied ||
                    std::memcmp(&wanted, &g_clip_rect_applied, sizeof(RECT)) != 0) {
                    if (real_clip(&wanted)) {
                        g_clip_applied = true;
                        g_clip_rect_applied = wanted;
                    }
                }
            } else if (g_clip_applied) {
                if (real_clip(nullptr)) g_clip_applied = false;
            }

            raw_input_reassert();

            ImGui::SetCurrentContext(g_imgui_context);
            if (!g_imgui_focus_known || g_imgui_focused != foreground) {
                // ImGui must know whether its private canvas is usable, but the
                // message-only input sink can never receive WM_SETFOCUS. Mirror
                // process foreground state without changing any native focus.
                ImGui::GetIO().AddFocusEvent(foreground);
                g_imgui_focused = foreground;
                g_imgui_focus_known = true;
            }
            ImGui_ImplWin32_NewFrame();
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(static_cast<float>(g_back_w),
                                    static_cast<float>(g_back_h));
            io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
            if (foreground) feed_gamepad(); else feed_gamepad_neutral();
            update_modifiers();
            update_virtual_cursor(foreground);
            ImGui::NewFrame();
            io.MouseDrawCursor = g_soft_cursor;

            ui::Frame frontend_frame;
            frontend_frame.canvas_w = static_cast<float>(g_back_w);
            frontend_frame.canvas_h = static_cast<float>(g_back_h);
            frontend_frame.pad_connected = g_pad_ok;
            frontend_frame.pad_buttons = g_pad_ok ? g_pad.wButtons : 0;
            frontend_frame.open_pad_mask = g_open_pad_mask;
            const ui::Result result = ui::draw(frontend_frame);
            g_search_activated = g_search_activated || result.search_active;
            if (result.close_requested)
                g_menu_open.store(false, std::memory_order_release);

            ImGui::Render();
            present::publish_draw_data(ImGui::GetDrawData());

            // UI construction is decoupled from GPU presentation.  125 Hz keeps
            // input responsive while avoiding a busy control thread; every game
            // Present composites the latest immutable packet.
            Sleep(8);
        } else {
            open_unavailable_since = 0;
            if (g_raw_captured) raw_input_capture(false);
            Sleep(16);
        }
    }

    g_menu_open.store(false, std::memory_order_release);
    present::set_visible(false);
    // The loop exited without passing through the close edge, so ownership is
    // still ours. Hand it back on the acquiring thread while we still are it --
    // a Windows mutex cannot be released from anywhere else, and leaking it
    // would lock every other overlay in the process out until this DLL's
    // handle closes.
    if (g_raw_captured) raw_input_capture(false);
    coexist::release_menu_ownership();
}

// Keep SEH in a POD-only wrapper so an unexpected control-thread fault cannot
// leave input capture active. Graphics hooks remain fail-open/pass-through.
void seh_overlay_thread() {
    __try {
        overlay_thread();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        flog("[overlay] [ERROR] overlay thread crashed -- tearing down");
    }
}

// ── best-effort teardown (the process usually just exits) ──
void teardown() {
    present::set_visible(false);
    g_context_ready = false;
    raw_input_capture(false); // first restore attempt while the sink is alive
    real_clip(nullptr);       // never leave the desktop cursor trapped in the canvas
    g_clip_applied = false;
    shutdown_frontend();
    present::shutdown();

    // RegisterRawInputDevices can fail transiently. Exiting this owner thread
    // while the process registration still targets g_input_hwnd would destroy
    // that HWND and leave raw input pointed at a dead sink. Keep the
    // message-only window alive and retry instead; process termination can
    // still end the thread normally.
    while (g_raw_captured && g_input_hwnd && IsWindow(g_input_hwnd)) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(100);
        raw_input_capture(false);
    }
    destroy_input_window();
}

} // namespace

// ── the public surface (overlay.hpp) ──

bool overlay::bootstrap() {
    g_mh_ok = hooks::init();
    if (!g_mh_ok) {
        flog("[overlay-v2] [ERROR] hook engine initialization failed");
        return false;
    }
    return present::install_hooks();
}

void overlay::sync_open_keys() {
    std::lock_guard<std::mutex> lk(g_state_mutex);
    g_open_vk = g_state.open_vk;
    g_open_pad_mask = g_state.open_pad_mask;
    g_open_pad_is_hold = g_state.open_pad_is_hold;
}

void overlay::setup() {
    sync_open_keys();

    // Gamepad. We must both READ the pad (menu nav + the open/close combo) and
    // BLOCK it from reaching the still-focused game while the menu is open.
    // Reading: resolve XInputGetState directly here (no hook needed to read).
    // Blocking: XInput detours are installed by hook_xinput() at MENU OPEN,
    // deliberately not here -- hooking early proved ineffective (see the
    // comment on hook_xinput); dinput8 (the path ER actually polls) is hooked
    // now and retried on open if the module loads late.
    {
        const char* xdlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
        for (const char* d : xdlls) {
            HMODULE h = GetModuleHandleA(d);
            if (!h) continue;
            pXInputGetState =
                reinterpret_cast<XInputGetState_t>(GetProcAddress(h, "XInputGetState"));
            if (pXInputGetState) break;
        }
        if (!pXInputGetState)
            if (HMODULE h = LoadLibraryA("xinput1_4.dll"))
                pXInputGetState =
                    reinterpret_cast<XInputGetState_t>(GetProcAddress(h, "XInputGetState"));
        if (!pXInputGetState)
            flog("[overlay] [WARN] XInputGetState not found; gamepad unavailable");

        g_mh_ok = g_mh_ok || hooks::init();
        if (!g_mh_ok)
            flog("[overlay] [WARN] MinHook init failed; pad will leak to the game while menu open");
        hook_dinput8();
    }

    // Spawn the frontend/control thread. It owns no graphics device or visible
    // window and never calls Present.
    if (g_running.exchange(true)) return; // already running
    std::thread([] {
        seh_overlay_thread();
        teardown();
        g_running.store(false);
    }).detach();
    flog("[overlay-v2] frontend control thread spawned (message-only input)");
}

} // namespace cte
