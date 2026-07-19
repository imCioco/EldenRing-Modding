#pragma once

// Per-instance DXGI swapchain interception for CustomTalismanEffects backend v2.
//
// This module never patches a shared DXGI implementation function.  It copies
// the current vtable of each interface alias exposed by one swapchain, replaces
// only the five methods CustomTalismanEffects observes, and atomically publishes that
// private table on the object.  The copied entries are the immediate next links
// in whatever hook/proxy chain was present at installation time.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <dxgi1_6.h>

#include <cstdint>

namespace cte::overlay::dxgi_shadow {

enum class PresentKind : uint8_t {
    Present,
    Present1,
};

struct PresentCall {
    PresentKind kind = PresentKind::Present;
    UINT sync_interval = 0;
    UINT flags = 0;

    // Null for Present.  For Present1 this pointer and all memory it references
    // are borrowed from the caller and remain valid only during the callback.
    const DXGI_PRESENT_PARAMETERS* parameters = nullptr;
};

enum class ResizeKind : uint8_t {
    ResizeBuffers,
    ResizeBuffers1,
};

struct ResizeCall {
    ResizeKind kind = ResizeKind::ResizeBuffers;
    UINT buffer_count = 0;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT flags = 0;

    // Non-null only for ResizeBuffers1.  These are borrowed caller pointers and
    // remain valid only while before_resize/after_resize is running.
    const UINT* creation_node_masks = nullptr;
    IUnknown* const* present_queues = nullptr;
};

// Callbacks execute on the thread that invoked the DXGI method.  They must not
// retain borrowed argument pointers.  Any C++ exception thrown by a callback is
// contained by this module; the downstream COM method is still called exactly
// once.  Exceptions from downstream code are deliberately not intercepted.
struct Callbacks {
    void (*before_present)(IDXGISwapChain* swapchain,
                           const PresentCall& call) = nullptr;
    void (*after_present)(IDXGISwapChain* swapchain,
                          const PresentCall& call, HRESULT result) = nullptr;

    bool (*before_resize)(IDXGISwapChain* swapchain,
                          const ResizeCall& call) = nullptr;
    void (*after_resize)(IDXGISwapChain* swapchain,
                         const ResizeCall& call, HRESULT result,
                         bool resources_released) = nullptr;

    void (*before_color_space)(IDXGISwapChain3* swapchain,
                               DXGI_COLOR_SPACE_TYPE color_space) = nullptr;
    void (*after_color_space)(IDXGISwapChain3* swapchain,
                              DXGI_COLOR_SPACE_TYPE color_space,
                              HRESULT result) = nullptr;
};

// Callback records are immutable after publication and intentionally retained
// for process life, just like installed hook metadata.  Reconfiguration is
// therefore safe while a Present is in flight: one call uses one snapshot.
bool configure_callbacks(const Callbacks& callbacks) noexcept;
void clear_callbacks() noexcept;

enum class InstallResult : uint8_t {
    Installed,
    AlreadyInstalled,
    PartiallyInstalled,
    InvalidObject,
    OutOfMemory,
    Collision,
};

// Install private vtables on every distinct IDXGISwapChain..IDXGISwapChain4
// interface pointer returned by QueryInterface.  Aliases are deduplicated and
// copied at the greatest known interface length for that pointer.
//
// A compare/exchange mismatch means another component changed the vptr after
// it was copied.  CustomTalismanEffects never overwrites that newer table; the affected
// alias fails closed.  Successfully installed aliases are not rolled back,
// because a later hook may already have chained through them.
InstallResult install(IDXGISwapChain* swapchain) noexcept;

// Stable, module-local lifetime generation assigned during install. Unlike
// IDXGIObject private data, this does not collapse wrapper/native objects that
// delegate their private-data store to the same underlying swapchain.
uint64_t generation_token(IDXGISwapChain* swapchain) noexcept;
// Rendering eligibility is deliberately separate from lifetime identity.
// A partial alias installation must remain visible to resize/color/device-loss
// cleanup callbacks even though it is permanently excluded from drawing.
bool renderer_eligible(IDXGISwapChain* swapchain) noexcept;
bool generation_alive(uint64_t generation) noexcept;

} // namespace cte::overlay::dxgi_shadow
