// CustomTalismanEffects backend v2 -- per-instance DXGI swapchain vtable shadows.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <objbase.h>

#include <dxgi1_6.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include "overlay_dxgi_shadow.hpp"

namespace cte::overlay::dxgi_shadow {
namespace {

constexpr uint32_t kSwapchainEntries = 18;
constexpr uint32_t kSwapchain1Entries = 29;
constexpr uint32_t kSwapchain2Entries = 36;
constexpr uint32_t kSwapchain3Entries = 40;
constexpr uint32_t kSwapchain4Entries = 41;
constexpr uint32_t kMaximumEntries = kSwapchain4Entries;

constexpr uint32_t kPresentIndex = 8;
constexpr uint32_t kResizeBuffersIndex = 13;
constexpr uint32_t kPresent1Index = 22;
constexpr uint32_t kSetColorSpace1Index = 38;
constexpr uint32_t kResizeBuffers1Index = 39;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ResizeBuffers1Fn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
    const UINT*, IUnknown* const*);
using SetColorSpace1Fn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);

struct CallbackRecord {
    Callbacks callbacks{};
};

std::atomic<const CallbackRecord*> g_callbacks{nullptr};

enum class HookStatus : uint8_t {
    Pending,
    Installed,
    Collision,
};

struct Generation {
    std::atomic<bool> alive{true};
    std::atomic<bool> renderer_eligible{true};
    void* identity = nullptr;
    uint64_t id = 0;
    GUID sentinel_guid{};
    bool lifetime_tracked = false;
};

class LifetimeSentinel final : public IUnknown {
public:
    explicit LifetimeSentinel(Generation* generation) noexcept
        : generation_(generation) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid == __uuidof(IUnknown)) {
            *result = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return references_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining =
            references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            if (armed_ && generation_)
                generation_->alive.store(false, std::memory_order_release);
            delete this;
        }
        return remaining;
    }

    void arm() noexcept { armed_ = true; }

private:
    std::atomic<ULONG> references_{1};
    Generation* generation_ = nullptr;
    bool armed_ = false;
};

struct HookState {
    HookState* next = nullptr;
    void* object = nullptr;
    Generation* generation = nullptr;
    void** expected_vtable = nullptr;
    uint32_t entry_count = 0;
    std::atomic<HookStatus> status{HookStatus::Pending};
    std::array<void*, kMaximumEntries> shadow{};

    PresentFn next_present = nullptr;
    Present1Fn next_present1 = nullptr;
    ResizeBuffersFn next_resize_buffers = nullptr;
    ResizeBuffers1Fn next_resize_buffers1 = nullptr;
    SetColorSpace1Fn next_set_color_space1 = nullptr;
};

std::atomic<HookState*> g_hook_states{nullptr};
constexpr size_t kStateIndexCapacity = 4096;
static_assert((kStateIndexCapacity & (kStateIndexCapacity - 1)) == 0);
struct StateIndexEntry {
    std::atomic<void*> object{nullptr};
    std::atomic<HookState*> state{nullptr};
};
std::array<StateIndexEntry, kStateIndexCapacity> g_state_index{};
std::atomic<uint64_t> g_next_generation_id{1};
INIT_ONCE g_install_once = INIT_ONCE_STATIC_INIT;
CRITICAL_SECTION g_install_lock{};

BOOL CALLBACK initialize_install_lock(PINIT_ONCE, PVOID, PVOID*) noexcept {
    return InitializeCriticalSectionEx(&g_install_lock, 4000, 0);
}

struct InstallGuard {
    bool owns = false;
    InstallGuard() noexcept {
        if (InitOnceExecuteOnce(&g_install_once, initialize_install_lock,
                               nullptr, nullptr)) {
            EnterCriticalSection(&g_install_lock); // recursive on one thread
            owns = true;
        }
    }
    void release() noexcept {
        if (owns) {
            LeaveCriticalSection(&g_install_lock);
            owns = false;
        }
    }
    ~InstallGuard() { release(); }
};

HRESULT STDMETHODCALLTYPE present_detour(IDXGISwapChain* swapchain,
                                          UINT sync_interval, UINT flags);
HRESULT STDMETHODCALLTYPE present1_detour(
    IDXGISwapChain1* swapchain, UINT sync_interval, UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters);
HRESULT STDMETHODCALLTYPE resize_buffers_detour(
    IDXGISwapChain* swapchain, UINT count, UINT width, UINT height,
    DXGI_FORMAT format, UINT flags);
HRESULT STDMETHODCALLTYPE resize_buffers1_detour(
    IDXGISwapChain3* swapchain, UINT count, UINT width, UINT height,
    DXGI_FORMAT format, UINT flags, const UINT* creation_node_masks,
    IUnknown* const* present_queues);
HRESULT STDMETHODCALLTYPE set_color_space1_detour(
    IDXGISwapChain3* swapchain, DXGI_COLOR_SPACE_TYPE color_space);

bool readable_protection(DWORD protection) noexcept {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    switch (protection & 0xFFu) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool readable_range(const void* address, size_t byte_count) noexcept {
    if (!address || byte_count == 0) return false;

    uintptr_t cursor = reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = cursor + byte_count;
    if (end < cursor) return false;

    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info,
                         sizeof(info)) != sizeof(info) ||
            info.State != MEM_COMMIT || !readable_protection(info.Protect))
            return false;

        const uintptr_t region_base =
            reinterpret_cast<uintptr_t>(info.BaseAddress);
        const uintptr_t region_end = region_base + info.RegionSize;
        if (region_end <= cursor) return false;
        cursor = region_end < end ? region_end : end;
    }
    return true;
}

// These helpers contain only POD operations.  Keeping SEH this narrow avoids
// bypassing C++ destructors or leaving a backend lock held after a fault.
bool read_vtable(void* object, void**** slot, void*** table) noexcept {
    if (!object || !slot || !table ||
        !readable_range(object, sizeof(void*)))
        return false;
    __try {
        *slot = reinterpret_cast<void***>(object);
        *table = **slot;
        return *table != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *slot = nullptr;
        *table = nullptr;
        return false;
    }
}

bool copy_vtable(void** destination, void** source,
                 uint32_t entry_count) noexcept {
    const size_t byte_count = static_cast<size_t>(entry_count) * sizeof(void*);
    if (!destination || !readable_range(source, byte_count)) return false;
    __try {
        std::memcpy(destination, source, byte_count);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool compare_exchange_vtable(void*** slot, void** expected,
                             void** replacement) noexcept {
    if (!slot || !replacement) return false;
    __try {
        void* const observed = InterlockedCompareExchangePointer(
            reinterpret_cast<void* volatile*>(slot), replacement, expected);
        return observed == expected;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void publish_state(HookState* state) noexcept {
    HookState* head = g_hook_states.load(std::memory_order_relaxed);
    do {
        state->next = head;
    } while (!g_hook_states.compare_exchange_weak(
        head, state, std::memory_order_release, std::memory_order_relaxed));

    // Present must not walk every historical swapchain alias. The fixed
    // open-addressed index publishes the newest claim for an exact address;
    // exhaustion falls back to the append-only list without affecting safety.
    const uintptr_t bits = reinterpret_cast<uintptr_t>(state->object);
    size_t slot = static_cast<size_t>(
        ((bits >> 4u) * static_cast<uintptr_t>(11400714819323198485ull)) &
        (kStateIndexCapacity - 1));
    for (size_t probe = 0; probe < kStateIndexCapacity; ++probe) {
        StateIndexEntry& entry =
            g_state_index[(slot + probe) & (kStateIndexCapacity - 1)];
        void* key = entry.object.load(std::memory_order_acquire);
        if (key == state->object) {
            entry.state.store(state, std::memory_order_release);
            break;
        }
        if (!key && entry.object.compare_exchange_strong(
                        key, state->object, std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
            entry.state.store(state, std::memory_order_release);
            break;
        }
    }
}

HookState* find_state(void* object) noexcept {
    if (!object) return nullptr;
    const uintptr_t bits = reinterpret_cast<uintptr_t>(object);
    const size_t slot = static_cast<size_t>(
        ((bits >> 4u) * static_cast<uintptr_t>(11400714819323198485ull)) &
        (kStateIndexCapacity - 1));
    for (size_t probe = 0; probe < kStateIndexCapacity; ++probe) {
        StateIndexEntry& entry =
            g_state_index[(slot + probe) & (kStateIndexCapacity - 1)];
        void* const key = entry.object.load(std::memory_order_acquire);
        if (!key) break;
        if (key != object) continue;
        HookState* const indexed =
            entry.state.load(std::memory_order_acquire);
        if (indexed && indexed->object == object && indexed->generation &&
            indexed->generation->alive.load(std::memory_order_acquire) &&
            indexed->status.load(std::memory_order_acquire) !=
                HookStatus::Collision)
            return indexed;
        break;
    }

    // Newest-first plus the lifetime generation makes raw address reuse safe.
    for (HookState* state = g_hook_states.load(std::memory_order_acquire);
         state; state = state->next) {
        if (state->object != object) continue;
        if (!state->generation ||
            !state->generation->alive.load(std::memory_order_acquire))
            continue;
        if (state->status.load(std::memory_order_acquire) !=
            HookStatus::Collision)
            return state;
    }
    return nullptr;
}

HookStatus existing_claim_status(void* object, bool& found) noexcept {
    for (HookState* state = g_hook_states.load(std::memory_order_acquire);
         state; state = state->next) {
        if (state->object != object || !state->generation ||
            !state->generation->alive.load(std::memory_order_acquire))
            continue;
        const HookStatus status =
            state->status.load(std::memory_order_acquire);
        if (status == HookStatus::Collision) continue;
        found = true;
        return status;
    }
    found = false;
    return HookStatus::Collision;
}

Generation* live_generation(void* identity) noexcept {
    for (HookState* state = g_hook_states.load(std::memory_order_acquire);
         state; state = state->next) {
        Generation* const generation = state->generation;
        if (generation && generation->identity == identity &&
            generation->alive.load(std::memory_order_acquire))
            return generation;
    }
    return nullptr;
}

Generation* create_generation(IDXGISwapChain* swapchain,
                              void* identity) noexcept {
    Generation* const generation = new (std::nothrow) Generation;
    if (!generation) return nullptr;
    generation->identity = identity;
    generation->id =
        g_next_generation_id.fetch_add(1, std::memory_order_relaxed);
    if (generation->id == 0)
        generation->id =
            g_next_generation_id.fetch_add(1, std::memory_order_relaxed);

    LifetimeSentinel* const sentinel =
        new (std::nothrow) LifetimeSentinel(generation);
    if (!sentinel) {
        delete generation;
        return nullptr;
    }

    if (SUCCEEDED(CoCreateGuid(&generation->sentinel_guid)) &&
        SUCCEEDED(swapchain->SetPrivateDataInterface(
            generation->sentinel_guid, sentinel))) {
        generation->lifetime_tracked = true;
        sentinel->arm();
    }
    // Success leaves one DXGI-owned reference. On failure the unarmed sentinel
    // is deleted and the generation deliberately remains live forever, so a
    // later raw-address reuse is rejected rather than mistaken for this object.
    sentinel->Release();
    return generation;
}

void* query_identity(IUnknown* object) noexcept {
    if (!object) return nullptr;
    IUnknown* identity = nullptr;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&identity))) || !identity)
        return nullptr;
    void* const result = identity;
    identity->Release();
    return result;
}

enum class AliasInstallResult : uint8_t {
    Installed,
    AlreadyInstalled,
    Invalid,
    OutOfMemory,
    Collision,
};

AliasInstallResult install_alias(void* object, Generation* generation,
                                  uint32_t entry_count) noexcept {
    if (!object || !generation || entry_count < kSwapchainEntries ||
        entry_count > kMaximumEntries)
        return AliasInstallResult::Invalid;

    // Hook metadata is append-only.  Refusing a second claim prevents an
    // installation race from publishing a state that an older detour could
    // mistake for its own.  A destruction cookie can later permit safe reuse.
    bool claim_found = false;
    const HookStatus claim_status = existing_claim_status(object, claim_found);
    if (claim_found) {
        return claim_status == HookStatus::Installed
            ? AliasInstallResult::AlreadyInstalled
            : AliasInstallResult::Collision;
    }

    void*** vtable_slot = nullptr;
    void** current_vtable = nullptr;
    if (!read_vtable(object, &vtable_slot, &current_vtable))
        return AliasInstallResult::Invalid;

    HookState* const state = new (std::nothrow) HookState;
    if (!state) return AliasInstallResult::OutOfMemory;

    state->object = object;
    state->generation = generation;
    state->expected_vtable = current_vtable;
    state->entry_count = entry_count;
    if (!copy_vtable(state->shadow.data(), current_vtable, entry_count)) {
        // The state is not externally visible yet, so reclaiming it is safe.
        delete state;
        return AliasInstallResult::Invalid;
    }

    // A concurrent installer may have published CustomTalismanEffects's table after this
    // thread made its initial claim check.  Never put our detour behind itself:
    // doing so would make a generic per-object lookup recurse forever.
    if (state->shadow[kPresentIndex] ==
        reinterpret_cast<void*>(&present_detour)) {
        delete state;
        return AliasInstallResult::AlreadyInstalled;
    }

    state->next_present = reinterpret_cast<PresentFn>(
        state->shadow[kPresentIndex]);
    state->next_resize_buffers = reinterpret_cast<ResizeBuffersFn>(
        state->shadow[kResizeBuffersIndex]);
    state->shadow[kPresentIndex] = reinterpret_cast<void*>(&present_detour);
    state->shadow[kResizeBuffersIndex] =
        reinterpret_cast<void*>(&resize_buffers_detour);

    if (entry_count > kPresent1Index) {
        state->next_present1 = reinterpret_cast<Present1Fn>(
            state->shadow[kPresent1Index]);
        state->shadow[kPresent1Index] = reinterpret_cast<void*>(&present1_detour);
    }
    if (entry_count > kSetColorSpace1Index) {
        state->next_set_color_space1 = reinterpret_cast<SetColorSpace1Fn>(
            state->shadow[kSetColorSpace1Index]);
        state->shadow[kSetColorSpace1Index] =
            reinterpret_cast<void*>(&set_color_space1_detour);
    }
    if (entry_count > kResizeBuffers1Index) {
        state->next_resize_buffers1 = reinterpret_cast<ResizeBuffers1Fn>(
            state->shadow[kResizeBuffers1Index]);
        state->shadow[kResizeBuffers1Index] =
            reinterpret_cast<void*>(&resize_buffers1_detour);
    }

    if (!state->next_present || !state->next_resize_buffers ||
        (entry_count > kPresent1Index && !state->next_present1) ||
        (entry_count > kSetColorSpace1Index && !state->next_set_color_space1) ||
        (entry_count > kResizeBuffers1Index && !state->next_resize_buffers1)) {
        delete state;
        return AliasInstallResult::Invalid;
    }

    // Publish before changing the object.  This closes the race where another
    // thread calls Present immediately after the vptr exchange.  Because the
    // first claim wins, our detour cannot be reachable on this object before
    // this compare/exchange succeeds.
    publish_state(state);
    if (!compare_exchange_vtable(vtable_slot, current_vtable,
                                 state->shadow.data())) {
        state->status.store(HookStatus::Collision, std::memory_order_release);
        return AliasInstallResult::Collision;
    }

    state->status.store(HookStatus::Installed, std::memory_order_release);
    return AliasInstallResult::Installed;
}

struct HeldInterface {
    IUnknown* pointer = nullptr;
    uint32_t entry_count = 0;
};

struct Alias {
    void* pointer = nullptr;
    uint32_t entry_count = 0;
};

template <typename Interface>
void* query_alias(IDXGISwapChain* swapchain, uint32_t entry_count,
                  std::array<HeldInterface, 5>& held,
                  uint32_t& held_count) noexcept {
    Interface* result = nullptr;
    if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&result))) && result &&
        held_count < held.size()) {
        held[held_count++] = {result, entry_count};
        return result;
    }
    if (result) result->Release();
    return nullptr;
}

void add_alias(std::array<Alias, 6>& aliases, uint32_t& alias_count,
               void* pointer, uint32_t entry_count) noexcept {
    if (!pointer) return;
    for (uint32_t i = 0; i < alias_count; ++i) {
        if (aliases[i].pointer == pointer) {
            if (entry_count > aliases[i].entry_count)
                aliases[i].entry_count = entry_count;
            return;
        }
    }
    if (alias_count < aliases.size())
        aliases[alias_count++] = {pointer, entry_count};
}

struct InterfaceReleaseScope {
    std::array<HeldInterface, 5>& held;
    uint32_t count;
    void release_now() noexcept {
        for (uint32_t i = 0; i < count; ++i) {
            if (held[i].pointer) {
                held[i].pointer->Release();
                held[i].pointer = nullptr;
            }
        }
        count = 0;
    }
    ~InterfaceReleaseScope() { release_now(); }
};

bool installed_alias_is_stable(void* alias, Generation* generation) noexcept {
    HookState* const state = find_state(alias);
    if (!state || state->generation != generation ||
        state->status.load(std::memory_order_acquire) != HookStatus::Installed)
        return false;
    void*** slot = nullptr;
    void** table = nullptr;
    return read_vtable(alias, &slot, &table) && table == state->shadow.data();
}

template <typename Interface>
bool verify_alias_after_release(IDXGISwapChain* swapchain,
                                void* expected,
                                Generation* generation) noexcept {
    Interface* observed = nullptr;
    const HRESULT result =
        swapchain->QueryInterface(IID_PPV_ARGS(&observed));
    bool stable = false;
    if (!expected) {
        // COM requires its supported interface set to remain static.
        stable = FAILED(result) && !observed;
    } else {
        stable = SUCCEEDED(result) && observed == expected &&
                 installed_alias_is_stable(observed, generation);
    }
    if (observed) observed->Release();
    return stable;
}

struct IdentityStack {
    std::array<void*, 8> entries{};
    uint32_t size = 0;

    bool enter(void* identity) noexcept {
        for (uint32_t i = 0; i < size; ++i)
            if (entries[i] == identity) return false;
        if (size >= entries.size()) return false;
        entries[size++] = identity;
        return true;
    }

    void leave() noexcept {
        if (size != 0) --size;
    }
};

thread_local IdentityStack g_present_stack;
thread_local IdentityStack g_resize_stack;
thread_local IdentityStack g_color_stack;

struct RecursionScope {
    IdentityStack& stack;
    bool outer;

    RecursionScope(IdentityStack& target, void* identity) noexcept
        : stack(target), outer(target.enter(identity)) {}
    ~RecursionScope() {
        if (outer) stack.leave();
    }
};

void invoke_before_present(const CallbackRecord* record,
                           IDXGISwapChain* swapchain,
                           const PresentCall& call) noexcept {
    if (!record || !record->callbacks.before_present) return;
    try {
        record->callbacks.before_present(swapchain, call);
    } catch (...) {
    }
}

void invoke_after_present(const CallbackRecord* record,
                          IDXGISwapChain* swapchain, const PresentCall& call,
                          HRESULT result) noexcept {
    if (!record || !record->callbacks.after_present) return;
    try {
        record->callbacks.after_present(swapchain, call, result);
    } catch (...) {
    }
}

bool invoke_before_resize(const CallbackRecord* record,
                          IDXGISwapChain* swapchain,
                          const ResizeCall& call) noexcept {
    if (!record || !record->callbacks.before_resize) return true;
    try {
        return record->callbacks.before_resize(swapchain, call);
    } catch (...) {
        return false;
    }
}

void invoke_after_resize(const CallbackRecord* record,
                         IDXGISwapChain* swapchain, const ResizeCall& call,
                         HRESULT result, bool resources_released) noexcept {
    if (!record || !record->callbacks.after_resize) return;
    try {
        record->callbacks.after_resize(swapchain, call, result,
                                       resources_released);
    } catch (...) {
    }
}

void invoke_before_color_space(const CallbackRecord* record,
                               IDXGISwapChain3* swapchain,
                               DXGI_COLOR_SPACE_TYPE color_space) noexcept {
    if (!record || !record->callbacks.before_color_space) return;
    try {
        record->callbacks.before_color_space(swapchain, color_space);
    } catch (...) {
    }
}

void invoke_after_color_space(const CallbackRecord* record,
                              IDXGISwapChain3* swapchain,
                              DXGI_COLOR_SPACE_TYPE color_space,
                              HRESULT result) noexcept {
    if (!record || !record->callbacks.after_color_space) return;
    try {
        record->callbacks.after_color_space(swapchain, color_space, result);
    } catch (...) {
    }
}

HRESULT STDMETHODCALLTYPE present_detour(IDXGISwapChain* swapchain,
                                          UINT sync_interval, UINT flags) {
    HookState* const state = find_state(swapchain);
    if (!state || !state->next_present) return DXGI_ERROR_INVALID_CALL;

    RecursionScope recursion(g_present_stack, state->generation->identity);
    const CallbackRecord* const callbacks =
        g_callbacks.load(std::memory_order_acquire);
    const PresentCall call{PresentKind::Present, sync_interval, flags, nullptr};
    if (recursion.outer) invoke_before_present(callbacks, swapchain, call);

    const HRESULT result = state->next_present(swapchain, sync_interval, flags);
    if (recursion.outer)
        invoke_after_present(callbacks, swapchain, call, result);
    return result;
}

HRESULT STDMETHODCALLTYPE present1_detour(
    IDXGISwapChain1* swapchain, UINT sync_interval, UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters) {
    HookState* const state = find_state(swapchain);
    if (!state || !state->next_present1) return DXGI_ERROR_INVALID_CALL;

    RecursionScope recursion(g_present_stack, state->generation->identity);
    const CallbackRecord* const callbacks =
        g_callbacks.load(std::memory_order_acquire);
    const PresentCall call{
        PresentKind::Present1, sync_interval, flags, parameters};
    if (recursion.outer)
        invoke_before_present(callbacks, swapchain, call);

    const HRESULT result =
        state->next_present1(swapchain, sync_interval, flags, parameters);
    if (recursion.outer)
        invoke_after_present(callbacks, swapchain, call, result);
    return result;
}

HRESULT STDMETHODCALLTYPE resize_buffers_detour(
    IDXGISwapChain* swapchain, UINT count, UINT width, UINT height,
    DXGI_FORMAT format, UINT flags) {
    HookState* const state = find_state(swapchain);
    if (!state || !state->next_resize_buffers) return DXGI_ERROR_INVALID_CALL;

    RecursionScope recursion(g_resize_stack, state->generation->identity);
    const CallbackRecord* const callbacks =
        g_callbacks.load(std::memory_order_acquire);
    const ResizeCall call{ResizeKind::ResizeBuffers, count, width, height,
                          format, flags, nullptr, nullptr};
    const bool resources_released = recursion.outer
        ? invoke_before_resize(callbacks, swapchain, call) : false;

    const HRESULT result = state->next_resize_buffers(
        swapchain, count, width, height, format, flags);
    if (recursion.outer)
        invoke_after_resize(callbacks, swapchain, call, result,
                            resources_released);
    return result;
}

HRESULT STDMETHODCALLTYPE resize_buffers1_detour(
    IDXGISwapChain3* swapchain, UINT count, UINT width, UINT height,
    DXGI_FORMAT format, UINT flags, const UINT* creation_node_masks,
    IUnknown* const* present_queues) {
    HookState* const state = find_state(swapchain);
    if (!state || !state->next_resize_buffers1)
        return DXGI_ERROR_INVALID_CALL;

    RecursionScope recursion(g_resize_stack, state->generation->identity);
    const CallbackRecord* const callbacks =
        g_callbacks.load(std::memory_order_acquire);
    const ResizeCall call{ResizeKind::ResizeBuffers1, count, width, height,
                          format, flags, creation_node_masks, present_queues};
    const bool resources_released = recursion.outer
        ? invoke_before_resize(callbacks, swapchain, call) : false;

    const HRESULT result = state->next_resize_buffers1(
        swapchain, count, width, height, format, flags, creation_node_masks,
        present_queues);
    if (recursion.outer)
        invoke_after_resize(callbacks, swapchain, call, result,
                            resources_released);
    return result;
}

HRESULT STDMETHODCALLTYPE set_color_space1_detour(
    IDXGISwapChain3* swapchain, DXGI_COLOR_SPACE_TYPE color_space) {
    HookState* const state = find_state(swapchain);
    if (!state || !state->next_set_color_space1)
        return DXGI_ERROR_INVALID_CALL;

    RecursionScope recursion(g_color_stack, state->generation->identity);
    const CallbackRecord* const callbacks =
        g_callbacks.load(std::memory_order_acquire);
    if (recursion.outer)
        invoke_before_color_space(callbacks, swapchain, color_space);
    const HRESULT result = state->next_set_color_space1(swapchain, color_space);
    if (recursion.outer)
        invoke_after_color_space(callbacks, swapchain, color_space, result);
    return result;
}

} // namespace

bool configure_callbacks(const Callbacks& callbacks) noexcept {
    CallbackRecord* const record = new (std::nothrow) CallbackRecord;
    if (!record) return false;
    record->callbacks = callbacks;
    g_callbacks.store(record, std::memory_order_release);
    return true;
}

void clear_callbacks() noexcept {
    g_callbacks.store(nullptr, std::memory_order_release);
}

InstallResult install(IDXGISwapChain* swapchain) noexcept {
    if (!swapchain) return InstallResult::InvalidObject;

    std::array<HeldInterface, 5> held{};
    std::array<void*, 5> expected_aliases{};
    uint32_t held_count = 0;
    expected_aliases[0] = query_alias<IDXGISwapChain>(
        swapchain, kSwapchainEntries, held, held_count);
    expected_aliases[1] = query_alias<IDXGISwapChain1>(
        swapchain, kSwapchain1Entries, held, held_count);
    expected_aliases[2] = query_alias<IDXGISwapChain2>(
        swapchain, kSwapchain2Entries, held, held_count);
    expected_aliases[3] = query_alias<IDXGISwapChain3>(
        swapchain, kSwapchain3Entries, held, held_count);
    expected_aliases[4] = query_alias<IDXGISwapChain4>(
        swapchain, kSwapchain4Entries, held, held_count);
    InterfaceReleaseScope release_scope{held, held_count};

    // QueryInterface/Release are foreign COM calls.  Hold the returned aliases
    // across installation, but do not run their final Release while the
    // process-wide installation lock is owned: wrappers are allowed to
    // re-enter arbitrary code from Release.  Destruction order guarantees the
    // guard below is released before release_scope runs.
    InstallGuard install_guard;
    if (!install_guard.owns) return InstallResult::OutOfMemory;

    void* const identity = query_identity(swapchain);
    if (!identity) return InstallResult::InvalidObject;
    Generation* generation = live_generation(identity);
    if (!generation) generation = create_generation(swapchain, identity);
    if (!generation) return InstallResult::OutOfMemory;

    std::array<Alias, 6> aliases{};
    uint32_t alias_count = 0;
    add_alias(aliases, alias_count, swapchain, kSwapchainEntries);
    for (uint32_t i = 0; i < held_count; ++i)
        add_alias(aliases, alias_count, held[i].pointer, held[i].entry_count);

    uint32_t installed = 0;
    uint32_t already_installed = 0;
    uint32_t invalid = 0;
    uint32_t out_of_memory = 0;
    uint32_t collisions = 0;
    for (uint32_t i = 0; i < alias_count; ++i) {
        switch (install_alias(aliases[i].pointer, generation,
                              aliases[i].entry_count)) {
        case AliasInstallResult::Installed:
            ++installed;
            break;
        case AliasInstallResult::AlreadyInstalled:
            ++already_installed;
            break;
        case AliasInstallResult::Invalid:
            ++invalid;
            break;
        case AliasInstallResult::OutOfMemory:
            ++out_of_memory;
            break;
        case AliasInstallResult::Collision:
            ++collisions;
            break;
        }
    }

    const uint32_t usable = installed + already_installed;
    uint32_t failed = invalid + out_of_memory + collisions;

    // Release every temporary interface reference without the installation
    // lock, then immediately query each IID again. A tear-off/proxy that
    // destroys and recreates aliases could otherwise hand out an unshadowed
    // ResizeBuffers/SetColorSpace1 pointer after CustomTalismanEffects had retained
    // backbuffer references. Such objects are not safe to render into; a
    // stability failure permanently makes the generation pass-through.
    install_guard.release();
    release_scope.release_now();
    if (usable != 0 && failed == 0) {
        const bool stable =
            verify_alias_after_release<IDXGISwapChain>(
                swapchain, expected_aliases[0], generation) &&
            verify_alias_after_release<IDXGISwapChain1>(
                swapchain, expected_aliases[1], generation) &&
            verify_alias_after_release<IDXGISwapChain2>(
                swapchain, expected_aliases[2], generation) &&
            verify_alias_after_release<IDXGISwapChain3>(
                swapchain, expected_aliases[3], generation) &&
            verify_alias_after_release<IDXGISwapChain4>(
                swapchain, expected_aliases[4], generation);
        if (!stable) ++failed;
    }
    if (failed != 0)
        generation->renderer_eligible.store(false, std::memory_order_release);
    if (usable != 0 && failed != 0) return InstallResult::PartiallyInstalled;
    if (installed != 0) return InstallResult::Installed;
    if (already_installed != 0) return InstallResult::AlreadyInstalled;
    if (out_of_memory != 0) return InstallResult::OutOfMemory;
    if (collisions != 0) return InstallResult::Collision;
    return InstallResult::InvalidObject;
}

uint64_t generation_token(IDXGISwapChain* swapchain) noexcept {
    HookState* const state = find_state(swapchain);
    return state && state->generation ? state->generation->id : 0;
}

bool renderer_eligible(IDXGISwapChain* swapchain) noexcept {
    HookState* const state = find_state(swapchain);
    return state && state->generation &&
           state->generation->renderer_eligible.load(
               std::memory_order_acquire);
}

bool generation_alive(uint64_t generation) noexcept {
    if (generation == 0) return false;
    for (HookState* state = g_hook_states.load(std::memory_order_acquire);
         state; state = state->next) {
        Generation* const value = state->generation;
        if (value && value->id == generation &&
            value->alive.load(std::memory_order_acquire))
            return true;
    }
    return false;
}

} // namespace cte::overlay::dxgi_shadow
