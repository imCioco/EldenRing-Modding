// CustomTalismanEffects backend v2 -- DXGI hook coordination and swapchain selection.
//
// The detours in this file are deliberately small wrappers.  They validate a
// D3D12 HWND swapchain, acquire the exact creation queue, submit one private
// command list, release every
// lock, and then call the captured next link exactly once.  They never call a
// swapchain method recursively and never rediscover a "clean" DXGI original.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <imgui.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include "hooks.hpp"
#include "log.hpp"
#include "overlay_d3d12.hpp"
#include "overlay_dxgi_shadow.hpp"
#include "overlay_frame.hpp"
#include "overlay_present.hpp"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace cte::overlay::present {
namespace {

using Microsoft::WRL::ComPtr;
using clock_type = std::chrono::steady_clock;

constexpr uintptr_t kFontTextureTokenValue =
    static_cast<uintptr_t>(0x5150464F4E54ull); // "QPFONT"
const ImTextureID kFontTextureToken =
    reinterpret_cast<ImTextureID>(kFontTextureTokenValue);

std::atomic<bool> g_installed{false};
std::atomic<bool> g_stopping{false};
std::atomic<bool> g_renderer_ready{false};
std::atomic<bool> g_visible{false};
std::atomic<bool> g_renderer_healthy{true};
std::atomic<bool> g_retry_on_next_open{false};
std::atomic<uint64_t> g_font_generation{0};
bool g_packet_skip_warning_logged = false; // serialized frontend producer only

// Atomic fields plus a generation seqlock avoid taking a lock in either the
// Present callback or the control thread.  Every payload field is itself
// atomic, so this is data-race-free under the C++ memory model.
struct CanvasStore {
    std::atomic<uint64_t> sequence{0};
    std::atomic<uintptr_t> hwnd{0};
    std::atomic<uint32_t> width{0};
    std::atomic<uint32_t> height{0};
    std::atomic<bool> hdr{false};
    std::atomic<bool> ready{false};
} g_canvas;
SRWLOCK g_canvas_writer_lock = SRWLOCK_INIT;

void publish_canvas(HWND hwnd, uint32_t width, uint32_t height, bool hdr,
                    bool ready) noexcept {
    // The sequence counter is a seqlock and therefore requires serialized
    // writers. Present, resize, color-space, device-loss, and shutdown paths
    // can run on different threads, so protect only the tiny publication.
    AcquireSRWLockExclusive(&g_canvas_writer_lock);
    g_canvas.sequence.fetch_add(1, std::memory_order_acq_rel); // odd
    g_canvas.hwnd.store(reinterpret_cast<uintptr_t>(hwnd), std::memory_order_relaxed);
    g_canvas.width.store(width, std::memory_order_relaxed);
    g_canvas.height.store(height, std::memory_order_relaxed);
    g_canvas.hdr.store(hdr, std::memory_order_relaxed);
    g_canvas.ready.store(ready, std::memory_order_relaxed);
    g_canvas.sequence.fetch_add(1, std::memory_order_release); // even
    ReleaseSRWLockExclusive(&g_canvas_writer_lock);
}

void* com_identity(IUnknown* object) noexcept {
    if (!object) return nullptr;
    IUnknown* identity = nullptr;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&identity))) || !identity)
        return nullptr;
    void* result = identity;
    identity->Release();
    return result;
}

uint64_t now_ticks() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            clock_type::now().time_since_epoch()).count());
}

// -------------------------------------------------------------------------
// Queue discovery and creation-time swapchain binding

struct QueueBinding {
    QueueBinding() = default;
    QueueBinding(QueueBinding&&) noexcept = default;
    QueueBinding& operator=(QueueBinding&&) noexcept = default;
    QueueBinding(const QueueBinding&) = delete;
    QueueBinding& operator=(const QueueBinding&) = delete;

    uint64_t swapchain_generation = 0;
    ComPtr<ID3D12CommandQueue> queue;
    void* queue_identity = nullptr;
    void* device_identity = nullptr;
    uint64_t transaction_id = 0;
    uint32_t evidence_depth = 0;
    uint64_t last_seen_ms = 0;
    bool proxy_ambiguous = false;
    bool ambiguity_logged = false;
};

std::mutex g_registry_mutex;
std::vector<QueueBinding> g_queue_bindings;
std::atomic<uint64_t> g_next_binding_transaction{1};

constexpr size_t kMaximumQueueBindings = 32;
using RetiredQueues =
    std::array<ComPtr<ID3D12CommandQueue>, kMaximumQueueBindings + 1>;

void retire_queue(QueueBinding& binding, RetiredQueues& retired,
                  size_t& retired_count) noexcept {
    // The registry is capped, so this bound is an invariant rather than a
    // best-effort limit.  Moving first ensures vector erase/assignment cannot
    // call a proxy queue's Release while the registry mutex is held.
    if (binding.queue && retired_count < retired.size())
        retired[retired_count++] = std::move(binding.queue);
}

void prune_dead_bindings_locked(RetiredQueues& retired,
                                size_t& retired_count) noexcept {
    for (size_t index = 0; index < g_queue_bindings.size();) {
        QueueBinding& binding = g_queue_bindings[index];
        if (dxgi_shadow::generation_alive(binding.swapchain_generation)) {
            ++index;
            continue;
        }
        retire_queue(binding, retired, retired_count);
        g_queue_bindings.erase(g_queue_bindings.begin() +
                               static_cast<ptrdiff_t>(index));
    }
}

uint64_t next_binding_transaction() noexcept {
    uint64_t value =
        g_next_binding_transaction.fetch_add(1, std::memory_order_relaxed);
    if (value == 0)
        value = g_next_binding_transaction.fetch_add(1,
                                                      std::memory_order_relaxed);
    return value;
}

struct ThreadBindingTransaction {
    uint64_t id = 0;
    uint32_t depth = 0;
};

thread_local ThreadBindingTransaction g_creation_transaction;
thread_local ThreadBindingTransaction g_resize_transaction;

class BindingScope final {
public:
    explicit BindingScope(ThreadBindingTransaction& transaction) noexcept
        : transaction_(transaction) {
        if (transaction_.depth == 0)
            transaction_.id = next_binding_transaction();
        ++transaction_.depth;
        id_ = transaction_.id;
        depth_ = transaction_.depth;
    }
    ~BindingScope() {
        if (transaction_.depth != 0) --transaction_.depth;
        if (transaction_.depth == 0) transaction_.id = 0;
    }
    uint64_t id() const noexcept { return id_; }
    uint32_t depth() const noexcept { return depth_; }

private:
    ThreadBindingTransaction& transaction_;
    uint64_t id_ = 0;
    uint32_t depth_ = 0;
};

bool bind_swapchain_queue(IDXGISwapChain* swapchain,
                          IUnknown* device_or_queue,
                          uint64_t transaction_id,
                          uint32_t evidence_depth) noexcept {
    RetiredQueues retired{};
    size_t retired_count = 0;
    try {
        if (g_stopping.load(std::memory_order_acquire) || !swapchain ||
            !device_or_queue)
            return false;

        ComPtr<ID3D12CommandQueue> queue;
        if (FAILED(device_or_queue->QueryInterface(IID_PPV_ARGS(&queue))) || !queue)
            return false; // D3D11 swapchain or an unrelated creation call
        if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;

        ComPtr<ID3D12Device> queue_device;
        ComPtr<ID3D12Device> swap_device;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&queue_device))) || !queue_device ||
            FAILED(swapchain->GetDevice(IID_PPV_ARGS(&swap_device))) || !swap_device)
            return false;

        void* const queue_device_id = com_identity(queue_device.Get());
        void* const queue_id = com_identity(queue.Get());
        if (!queue_id || !queue_device_id ||
            queue_device_id != com_identity(swap_device.Get())) {
            flog("[overlay-v2] rejected swapchain creation queue: device identity mismatch");
            return false;
        }

        const uint64_t generation =
            dxgi_shadow::generation_token(swapchain);
        if (!generation || !dxgi_shadow::renderer_eligible(swapchain))
            return false;

        const uint64_t now = now_ticks();
        QueueBinding pending;
        pending.swapchain_generation = generation;
        pending.queue = std::move(queue);
        pending.queue_identity = queue_id;
        pending.device_identity = queue_device_id;
        pending.transaction_id = transaction_id;
        pending.evidence_depth = evidence_depth;
        pending.last_seen_ms = now;

        std::lock_guard lock(g_registry_mutex);
        if (g_stopping.load(std::memory_order_acquire)) return false;
        prune_dead_bindings_locked(retired, retired_count);
        bool proxy_ambiguous = false;
        for (const auto& binding : g_queue_bindings) {
            if (binding.transaction_id == transaction_id &&
                binding.evidence_depth > evidence_depth &&
                binding.swapchain_generation != generation &&
                binding.queue_identity != queue_id) {
                // A nested factory/ResizeBuffers1 call used a different queue.
                // The outer object is a proxy and DXGI provides no proof that
                // its caller-supplied queue sequences the presented buffers.
                proxy_ambiguous = true;
                break;
            }
        }
        for (auto& binding : g_queue_bindings) {
            if (binding.swapchain_generation == generation) {
                if (binding.transaction_id == transaction_id &&
                    binding.evidence_depth > evidence_depth) {
                    // The same installed object was observed at two wrapper
                    // layers. Preserve the innermost evidence.
                    return true;
                }
                if (binding.queue_identity != queue_id) {
                    retire_queue(binding, retired, retired_count);
                    binding.queue = std::move(pending.queue);
                }
                binding.queue_identity = queue_id;
                binding.device_identity = queue_device_id;
                binding.transaction_id = transaction_id;
                binding.evidence_depth = evidence_depth;
                binding.last_seen_ms = now;
                binding.proxy_ambiguous =
                    binding.proxy_ambiguous || proxy_ambiguous;
                return true;
            }
        }
        if (g_queue_bindings.size() >= kMaximumQueueBindings) {
            const auto oldest = std::min_element(
                g_queue_bindings.begin(), g_queue_bindings.end(),
                [](const QueueBinding& a, const QueueBinding& b) {
                    return a.last_seen_ms < b.last_seen_ms;
                });
            if (oldest != g_queue_bindings.end()) {
                retire_queue(*oldest, retired, retired_count);
                g_queue_bindings.erase(oldest);
            }
        }
        pending.proxy_ambiguous = proxy_ambiguous;
        g_queue_bindings.push_back(std::move(pending));
        if (proxy_ambiguous) {
            flog("[overlay-v2] nested proxy used a different queue; outer swapchain is pass-through only");
        } else {
            flog("[overlay-v2] captured D3D12 queue from the innermost observed swapchain transaction");
        }
        return true;
    } catch (...) {
        // Registry allocation failure disables this association only.  The
        // swapchain creation result and every later Present remain untouched.
        g_renderer_healthy.store(false, std::memory_order_release);
        return false;
    }
}

void erase_swapchain_binding(uint64_t generation) noexcept {
    if (!generation) return;
    RetiredQueues retired{};
    size_t retired_count = 0;
    try {
        std::lock_guard lock(g_registry_mutex);
        for (size_t index = 0; index < g_queue_bindings.size();) {
            QueueBinding& binding = g_queue_bindings[index];
            if (binding.swapchain_generation != generation) {
                ++index;
                continue;
            }
            retire_queue(binding, retired, retired_count);
            g_queue_bindings.erase(g_queue_bindings.begin() +
                                   static_cast<ptrdiff_t>(index));
        }
    } catch (...) {
        g_renderer_healthy.store(false, std::memory_order_release);
    }
}

ComPtr<ID3D12CommandQueue> queue_for_swapchain(
    uint64_t generation, void* device_identity) noexcept {
    try {
        std::unique_lock lock(g_registry_mutex, std::try_to_lock);
        if (!lock.owns_lock()) return {};

        for (auto& binding : g_queue_bindings) {
            if (binding.swapchain_generation == generation &&
                binding.device_identity == device_identity) {
                if (binding.proxy_ambiguous) {
                    if (!binding.ambiguity_logged) {
                        binding.ambiguity_logged = true;
                        flog("[overlay-v2] proxy presentation queue is unverified; rendering disabled for this swapchain");
                    }
                    return {};
                }
                binding.last_seen_ms = now_ticks();
                return binding.queue;
            }
        }
    } catch (...) {
        g_renderer_healthy.store(false, std::memory_order_release);
    }
    // Even a single DIRECT queue on the same device does not prove that it is
    // the queue DXGI associated with this swapchain.  Never guess.
    return {};
}

void bind_resize_queues(IDXGISwapChain3* swapchain, UINT requested_count,
                        const UINT* node_masks,
                        IUnknown* const* present_queues,
                        uint64_t transaction_id,
                        uint32_t evidence_depth) noexcept {
    try {
        if (!swapchain) return;
        const uint64_t generation =
            dxgi_shadow::generation_token(swapchain);
        if (!generation) return;

        // Per the API contract both arrays have exactly BufferCount entries.
        // Zero means "preserve the existing count", so there are no new array
        // entries to inspect and the prior exact association remains valid.
        if (requested_count == 0) return;

        ComPtr<IDXGISwapChain1> swapchain1;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain1))) ||
            !swapchain1 || FAILED(swapchain1->GetDesc1(&desc)) ||
            desc.BufferCount != requested_count || !present_queues) {
            erase_swapchain_binding(generation);
            return;
        }

        // ResizeBuffers1 permits a queue and node mask per backbuffer.  This
        // single-node renderer supports the common case only: every buffer is
        // presented on the exact same direct queue.  Heterogeneous arrays are
        // deliberately rejected instead of silently binding element zero.
        const UINT effective_count = requested_count;
        ComPtr<ID3D12CommandQueue> first_queue;
        void* first_identity = nullptr;
        UINT first_mask = 0;
        for (UINT i = 0; i < effective_count; ++i) {
            ComPtr<ID3D12CommandQueue> queue;
            if (!present_queues[i] ||
                FAILED(present_queues[i]->QueryInterface(IID_PPV_ARGS(&queue))) ||
                !queue || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
                erase_swapchain_binding(generation);
                return;
            }
            const void* identity = com_identity(queue.Get());
            const UINT mask = node_masks ? node_masks[i] : 0;
            if (i == 0) {
                first_queue = queue;
                first_identity = const_cast<void*>(identity);
                first_mask = mask;
            } else if (identity != first_identity || mask != first_mask) {
                flog("[overlay-v2] heterogeneous ResizeBuffers1 queues/nodes; pass-through only");
                erase_swapchain_binding(generation);
                return;
            }
        }
        if (!first_queue || (first_mask != 0 && first_mask != 1)) {
            erase_swapchain_binding(generation);
            return;
        }
        bind_swapchain_queue(swapchain, first_queue.Get(), transaction_id,
                             evidence_depth);
    } catch (...) {
        g_renderer_healthy.store(false, std::memory_order_release);
    }
}

// -------------------------------------------------------------------------
// Swapchain selection, color observation, and renderer session

struct ObservedSwapchain {
    uint64_t generation = 0;
    HWND hwnd = nullptr;
    void* device_identity = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t consecutive_presents = 0;
    uint64_t last_seen_ms = 0;
};

std::atomic<uint64_t> g_next_color_event_sequence{1};
std::atomic<bool> g_nvapi_observer_enabled{false};
std::atomic<uint64_t> g_nvapi_state_revision{0};

enum class NvapiHdrCommand : int32_t {
    Get = 0,
    Set = 1,
};

enum class NvapiHdrMode : int32_t {
    Off = 0,
    Uhda = 2,
    UhdaPassthrough = 5,
};

// All NVAPI HDR color-data versions begin with these three public fields.
// Elden Ring currently passes V1 (version 0x00010028).
struct NvapiHdrColorDataHeader {
    uint32_t version;
    int32_t command;
    int32_t hdr_mode;
};

struct NvapiDisplayBinding {
    uint32_t display_id = 0;
    std::array<char, CCHDEVICENAME> output_name{};
};

struct NvapiColorObservation {
    uint32_t display_id = 0;
    std::array<char, CCHDEVICENAME> output_name{};
    int32_t hdr_mode = -1;
    uint64_t event_sequence = 0;
};

std::mutex g_nvapi_mutex;
std::vector<NvapiDisplayBinding> g_nvapi_display_bindings;
std::vector<NvapiColorObservation> g_nvapi_color_observations;

uint64_t next_color_event_sequence() noexcept {
    uint64_t value =
        g_next_color_event_sequence.fetch_add(1, std::memory_order_relaxed);
    if (value == 0) {
        value = g_next_color_event_sequence.fetch_add(
            1, std::memory_order_relaxed);
    }
    return value;
}

struct ColorObservation {
    uint64_t generation = 0;
    DXGI_COLOR_SPACE_TYPE color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    uint64_t event_sequence = 0;
    bool known = false;
};

std::mutex g_render_mutex;
thread_local bool g_present_render_critical = false;
struct PresentRenderCriticalScope {
    PresentRenderCriticalScope() noexcept {
        g_present_render_critical = true;
    }
    ~PresentRenderCriticalScope() {
        g_present_render_critical = false;
    }
};
std::vector<ObservedSwapchain> g_observed_swapchains;
std::vector<ColorObservation> g_color_observations;
uint64_t g_primary_generation = 0;
HWND g_primary_hwnd = nullptr;
uint64_t g_color_unknown_logged_generation = 0;
std::unique_ptr<d3d12::Session> g_session;
struct PendingCompositedBuffer {
    uint64_t generation = 0;
    uint32_t slot = 0;
};
PendingCompositedBuffer g_pending_composited_buffer;
constexpr size_t kMaximumRetiredSessionsPerPresent = 3;
using RetiredSessions = std::array<std::unique_ptr<d3d12::Session>,
                                   kMaximumRetiredSessionsPerPresent>;

void retire_current_session(RetiredSessions& retired,
                            size_t& retired_count) noexcept {
    if (!g_session) return;
    if (retired_count < retired.size()) {
        retired[retired_count++] = std::move(g_session);
    } else {
        // This bound is not reachable in the current state machine. If a
        // future edit violates it, quarantine instead of running a potentially
        // re-entrant COM destruction while the renderer mutex is held.
        (void)g_session.release();
    }
}

bool g_session_rebuild_pending = false;
struct LifecycleGate {
    uint64_t generation = 0;
    uint32_t depth = 0;
};
LifecycleGate g_resize_gate;
LifecycleGate g_color_gate;

ColorObservation* color_record(uint64_t generation) {
    for (auto& item : g_color_observations)
        if (item.generation == generation) return &item;
    if (g_color_observations.size() >= 32) {
        const auto discard = std::find_if(
            g_color_observations.begin(), g_color_observations.end(),
            [](const ColorObservation& item) {
                return item.generation != g_primary_generation;
            });
        if (discard != g_color_observations.end())
            g_color_observations.erase(discard);
    }
    g_color_observations.push_back({generation});
    return &g_color_observations.back();
}

std::array<char, CCHDEVICENAME> copy_output_name(
    const char* source) noexcept {
    std::array<char, CCHDEVICENAME> result{};
    if (!source) return result;
    for (size_t index = 0; index + 1 < result.size() && source[index] != '\0';
         ++index)
        result[index] = source[index];
    return result;
}

const char* nvapi_hdr_mode_name(int32_t mode) noexcept {
    switch (static_cast<NvapiHdrMode>(mode)) {
    case NvapiHdrMode::Off: return "OFF";
    case NvapiHdrMode::Uhda: return "UHDA";
    case NvapiHdrMode::UhdaPassthrough: return "UHDA_PASSTHROUGH";
    default: return "unknown";
    }
}

void record_nvapi_display_binding(
    const std::array<char, CCHDEVICENAME>& output_name,
    uint32_t display_id) {
    {
        std::lock_guard lock(g_nvapi_mutex);
        bool binding_changed = false;
        const auto existing = std::find_if(
            g_nvapi_display_bindings.begin(), g_nvapi_display_bindings.end(),
            [display_id](const NvapiDisplayBinding& item) {
                return item.display_id == display_id;
            });
        if (existing != g_nvapi_display_bindings.end()) {
            binding_changed = existing->output_name[0] != '\0' &&
                              existing->output_name != output_name;
            existing->output_name = output_name;
        } else {
            if (g_nvapi_display_bindings.size() >= 32)
                g_nvapi_display_bindings.erase(
                    g_nvapi_display_bindings.begin());
            g_nvapi_display_bindings.push_back({display_id, output_name});
        }
        const auto observation = std::find_if(
            g_nvapi_color_observations.begin(),
            g_nvapi_color_observations.end(),
            [display_id](const NvapiColorObservation& item) {
                return item.display_id == display_id;
            });
        if (observation != g_nvapi_color_observations.end()) {
            if (binding_changed) {
                // Display IDs may be recycled after a topology change. Never
                // transfer an old HDR SET observation to a different output.
                g_nvapi_color_observations.erase(observation);
            } else if (observation->output_name[0] == '\0') {
                observation->output_name = output_name;
            }
        }
    }
    g_nvapi_state_revision.fetch_add(1, std::memory_order_release);
}

void record_nvapi_color_observation(uint32_t display_id, int32_t hdr_mode) {
    NvapiColorObservation observed;
    observed.display_id = display_id;
    observed.hdr_mode = hdr_mode;
    observed.event_sequence = next_color_event_sequence();

    {
        std::lock_guard lock(g_nvapi_mutex);
        const auto binding = std::find_if(
            g_nvapi_display_bindings.begin(), g_nvapi_display_bindings.end(),
            [display_id](const NvapiDisplayBinding& item) {
                return item.display_id == display_id;
            });
        if (binding != g_nvapi_display_bindings.end())
            observed.output_name = binding->output_name;

        const auto existing = std::find_if(
            g_nvapi_color_observations.begin(),
            g_nvapi_color_observations.end(),
            [display_id](const NvapiColorObservation& item) {
                return item.display_id == display_id;
            });
        if (existing != g_nvapi_color_observations.end()) {
            *existing = observed;
        } else {
            if (g_nvapi_color_observations.size() >= 32) {
                const auto oldest = std::min_element(
                    g_nvapi_color_observations.begin(),
                    g_nvapi_color_observations.end(),
                    [](const NvapiColorObservation& left,
                       const NvapiColorObservation& right) {
                        return left.event_sequence < right.event_sequence;
                    });
                if (oldest != g_nvapi_color_observations.end())
                    g_nvapi_color_observations.erase(oldest);
            }
            g_nvapi_color_observations.push_back(observed);
        }
    }
    g_nvapi_state_revision.fetch_add(1, std::memory_order_release);

    flog("[overlay-v2] observed successful NvAPI HDR SET "
         "(mode=%s, display_id=%u, output=%s)",
         nvapi_hdr_mode_name(hdr_mode), display_id,
         observed.output_name[0] ? observed.output_name.data() : "unmapped");
}

std::array<char, CCHDEVICENAME> monitor_output_name(
    HMONITOR monitor) noexcept {
    std::array<char, CCHDEVICENAME> result{};
    if (!monitor) return result;
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return result;
    if (WideCharToMultiByte(CP_ACP, 0, info.szDevice, -1, result.data(),
                            static_cast<int>(result.size()), nullptr,
                            nullptr) == 0)
        result = {};
    return result;
}

struct NvapiColorSnapshot {
    int32_t hdr_mode = -1;
    uint64_t event_sequence = 0;
    bool valid = false;
};

struct NvapiColorCache {
    uint64_t swapchain_generation = 0;
    uint64_t state_revision = 0;
    HMONITOR monitor = nullptr;
    NvapiColorSnapshot color;
} g_nvapi_color_cache;

NvapiColorSnapshot latest_nvapi_color_for_window(uint64_t generation,
                                                  HWND hwnd) {
    // Called with g_render_mutex held. A cheap monitor-handle comparison keeps
    // borderless/exclusive moves correct; name conversion and the vendor mutex
    // are needed only after an NVAPI state, monitor, or generation change.
    const uint64_t revision =
        g_nvapi_state_revision.load(std::memory_order_acquire);
    const HMONITOR monitor = revision != 0
        ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) : nullptr;
    if (g_nvapi_color_cache.swapchain_generation == generation &&
        g_nvapi_color_cache.state_revision == revision &&
        g_nvapi_color_cache.monitor == monitor)
        return g_nvapi_color_cache.color;

    NvapiColorSnapshot result;
    if (revision != 0) {
        const auto output_name = monitor_output_name(monitor);
        std::lock_guard lock(g_nvapi_mutex);
        for (const auto& item : g_nvapi_color_observations) {
            const bool matches_output =
                output_name[0] != '\0' && item.output_name[0] != '\0' &&
                _stricmp(output_name.data(), item.output_name.data()) == 0;
            if (!matches_output ||
                item.event_sequence <= result.event_sequence)
                continue;
            result.hdr_mode = item.hdr_mode;
            result.event_sequence = item.event_sequence;
            result.valid = true;
        }
    }
    g_nvapi_color_cache = {generation, revision, monitor, result};
    return result;
}

bool creation_default_is_authoritative(uint64_t generation,
                                       uint64_t transaction_id,
                                       uint32_t evidence_depth) noexcept {
    try {
        std::lock_guard lock(g_registry_mutex);
        for (const auto& binding : g_queue_bindings) {
            if (binding.transaction_id == transaction_id &&
                binding.evidence_depth > evidence_depth &&
                binding.swapchain_generation != generation) {
                // A factory wrapper created a distinct inner swapchain before
                // returning this object. It could also have changed the outer
                // object's color space before our instance shadow existed, so
                // its post-creation state is not the DXGI default we can prove
                // for a directly observed creation.
                return false;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

void record_creation_color_default(IDXGISwapChain* swapchain,
                                   uint64_t transaction_id,
                                   uint32_t evidence_depth) noexcept {
    try {
        if (!swapchain) return;
        const uint64_t generation =
            dxgi_shadow::generation_token(swapchain);
        if (!generation ||
            !creation_default_is_authoritative(generation, transaction_id,
                                               evidence_depth))
            return;

        ComPtr<IDXGISwapChain1> swapchain1;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain1))) ||
            !swapchain1 || FAILED(swapchain1->GetDesc1(&desc)))
            return;

        // DXGI defines the creation-time encoding by pixel format: floating
        // point swapchains start as linear scRGB, while normalized integer
        // formats (including R10) start as sRGB/SDR. A later successful
        // SetColorSpace1 callback replaces this baseline before the next
        // Present. This fact is essential for games that keep the default and
        // therefore never call the setter at all.
        const DXGI_COLOR_SPACE_TYPE default_space =
            desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
                ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

        std::lock_guard lock(g_render_mutex);
        ColorObservation* record = color_record(generation);
        if (!record->known) {
            record->color_space = default_space;
            record->known = true;
        }
    } catch (...) {
        g_renderer_healthy.store(false, std::memory_order_release);
    }
}

struct InferredColor {
    d3d12::ColorMode mode = d3d12::ColorMode::Unknown;
    d3d12::ColorEvidence evidence = d3d12::ColorEvidence::Dxgi;
};

d3d12::ColorMode dxgi_color_mode(DXGI_COLOR_SPACE_TYPE color_space,
                                  DXGI_FORMAT format) noexcept {
    if (color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 &&
        format == DXGI_FORMAT_R10G10B10A2_UNORM)
        return d3d12::ColorMode::Hdr10;
    if (color_space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 &&
        format == DXGI_FORMAT_R16G16B16A16_FLOAT)
        return d3d12::ColorMode::ScRgb;
    if (color_space == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 &&
        (format == DXGI_FORMAT_R8G8B8A8_UNORM ||
         format == DXGI_FORMAT_B8G8R8A8_UNORM ||
         format == DXGI_FORMAT_R10G10B10A2_UNORM))
        return d3d12::ColorMode::Sdr;
    return d3d12::ColorMode::Unknown;
}

constexpr d3d12::ColorMode nvapi_color_mode(int32_t hdr_mode,
                                             DXGI_FORMAT format) noexcept {
    if (hdr_mode == static_cast<int32_t>(NvapiHdrMode::UhdaPassthrough) &&
        format == DXGI_FORMAT_R10G10B10A2_UNORM)
        return d3d12::ColorMode::Hdr10;
    if (hdr_mode == static_cast<int32_t>(NvapiHdrMode::Uhda) &&
        format == DXGI_FORMAT_R16G16B16A16_FLOAT)
        return d3d12::ColorMode::ScRgb;
    if (hdr_mode == static_cast<int32_t>(NvapiHdrMode::Off) &&
        (format == DXGI_FORMAT_R8G8B8A8_UNORM ||
         format == DXGI_FORMAT_B8G8R8A8_UNORM ||
         format == DXGI_FORMAT_R10G10B10A2_UNORM))
        return d3d12::ColorMode::Sdr;
    return d3d12::ColorMode::Unknown;
}

static_assert(nvapi_color_mode(5, DXGI_FORMAT_R10G10B10A2_UNORM) ==
              d3d12::ColorMode::Hdr10);
static_assert(nvapi_color_mode(2, DXGI_FORMAT_R16G16B16A16_FLOAT) ==
              d3d12::ColorMode::ScRgb);

InferredColor infer_color_mode(uint64_t generation, DXGI_FORMAT format,
                               HWND hwnd) {
    const ColorObservation* dxgi_observation = nullptr;
    for (const auto& item : g_color_observations) {
        if (item.generation == generation && item.known) {
            dxgi_observation = &item;
            break;
        }
    }

    const NvapiColorSnapshot nvapi_observation =
        latest_nvapi_color_for_window(generation, hwnd);
    const uint64_t dxgi_sequence =
        dxgi_observation ? dxgi_observation->event_sequence : 0;
    if (nvapi_observation.valid &&
        nvapi_observation.event_sequence > dxgi_sequence) {
        return {nvapi_color_mode(nvapi_observation.hdr_mode, format),
                d3d12::ColorEvidence::NvidiaNvapi};
    }
    if (dxgi_observation) {
        return {dxgi_color_mode(dxgi_observation->color_space, format),
                d3d12::ColorEvidence::Dxgi};
    }

    // Reaching this fallback means a detected nested wrapper prevented
    // authoritative creation/color observation. The containing output and
    // buffer format are not proof of that wrapper's current encoding, so
    // FP16/R10 fail closed.
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
        format == DXGI_FORMAT_R10G10B10A2_UNORM)
        return {};
    return {d3d12::ColorMode::Sdr, d3d12::ColorEvidence::Dxgi};
}

bool valid_game_window(HWND hwnd) noexcept {
    if (!hwnd || !IsWindow(hwnd)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return false;
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) != 0) return false;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd || !IsWindowVisible(hwnd)) return false;
    return true;
}

bool valid_creation_window(HWND hwnd) noexcept {
    if (!hwnd || !IsWindow(hwnd)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return false;
    if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0)
        return false;
    return GetAncestor(hwnd, GA_ROOT) == hwnd;
}

bool supported_backbuffer_format(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM ||
           format == DXGI_FORMAT_B8G8R8A8_UNORM ||
           format == DXGI_FORMAT_R10G10B10A2_UNORM ||
           format == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

bool describe_swapchain(IDXGISwapChain* base,
                        ComPtr<IDXGISwapChain3>& swapchain3,
                        ComPtr<ID3D12Device>& device,
                        uint64_t& generation,
                        void*& device_id,
                        HWND& hwnd,
                        DXGI_SWAP_CHAIN_DESC1& desc) noexcept {
    if (!base || !dxgi_shadow::renderer_eligible(base) ||
        FAILED(base->QueryInterface(IID_PPV_ARGS(&swapchain3))) || !swapchain3)
        return false;
    if (FAILED(base->GetDevice(IID_PPV_ARGS(&device))) || !device)
        return false; // rejects D3D11 and foreign helper swapchains

    if (FAILED(swapchain3->GetDesc1(&desc)))
        return false;
    if (FAILED(swapchain3->GetHwnd(&hwnd)) || !valid_game_window(hwnd))
        return false; // composition/CoreWindow/hidden helper chains fail closed

    if (desc.BufferCount < 2 || desc.BufferCount > 8 || desc.SampleDesc.Count != 1)
        return false;
    if (!supported_backbuffer_format(desc.Format)) return false;
    if ((desc.BufferUsage & DXGI_USAGE_RENDER_TARGET_OUTPUT) == 0) return false;
    constexpr UINT kUnsupportedFlags =
        DXGI_SWAP_CHAIN_FLAG_RESTRICTED_CONTENT |
        DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY |
        DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED;
    if ((desc.Flags & kUnsupportedFlags) != 0) return false;
    if (desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD &&
        desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
        return false;

    generation = dxgi_shadow::generation_token(base);
    device_id = com_identity(device.Get());
    if (!generation || !device_id) return false;

    ComPtr<ID3D12Resource> backbuffer;
    if (FAILED(swapchain3->GetBuffer(swapchain3->GetCurrentBackBufferIndex(),
                                     IID_PPV_ARGS(&backbuffer))) || !backbuffer)
        return false;
    const D3D12_RESOURCE_DESC resource_desc = backbuffer->GetDesc();
    if (resource_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        resource_desc.DepthOrArraySize != 1 || resource_desc.MipLevels != 1 ||
        resource_desc.SampleDesc.Count != 1 ||
        resource_desc.Format != desc.Format ||
        (resource_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0 ||
        resource_desc.Width > UINT_MAX)
        return false;
    if (desc.Width == 0) desc.Width = static_cast<UINT>(resource_desc.Width);
    if (desc.Height == 0) desc.Height = resource_desc.Height;
    if (resource_desc.Width != desc.Width || resource_desc.Height != desc.Height)
        return false;
    // Tiny video/probe/helper chains are never the Elden Ring presentation
    // surface.  Requiring a plausible game canvas makes initial selection less
    // dependent on whichever injected module happens to Present first.
    return desc.Width >= 640 && desc.Height >= 360;
}

bool eligible_shadow_candidate(IDXGISwapChain* base) noexcept {
    if (!base) return false;
    ComPtr<ID3D12Device> device;
    ComPtr<IDXGISwapChain1> swapchain1;
    if (FAILED(base->GetDevice(IID_PPV_ARGS(&device))) || !device ||
        FAILED(base->QueryInterface(IID_PPV_ARGS(&swapchain1))) ||
        !swapchain1)
        return false;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    HWND hwnd = nullptr;
    if (FAILED(swapchain1->GetDesc1(&desc)) ||
        FAILED(swapchain1->GetHwnd(&hwnd)) || !valid_creation_window(hwnd) ||
        desc.BufferCount < 2 || desc.BufferCount > 8 ||
        desc.SampleDesc.Count != 1 || !supported_backbuffer_format(desc.Format) ||
        (desc.BufferUsage & DXGI_USAGE_RENDER_TARGET_OUTPUT) == 0 ||
        (desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD &&
         desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL))
        return false;

    constexpr UINT kUnsupportedFlags =
        DXGI_SWAP_CHAIN_FLAG_RESTRICTED_CONTENT |
        DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY |
        DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED;
    if ((desc.Flags & kUnsupportedFlags) != 0) return false;

    if (desc.Width < 640 || desc.Height < 360) {
        ComPtr<ID3D12Resource> buffer;
        if (FAILED(base->GetBuffer(0, IID_PPV_ARGS(&buffer))) || !buffer)
            return false;
        const D3D12_RESOURCE_DESC resource = buffer->GetDesc();
        if (resource.Width < 640 || resource.Width > UINT_MAX ||
            resource.Height < 360)
            return false;
    }
    return true;
}

bool select_primary(uint64_t generation, HWND hwnd, void* device_identity,
                    uint32_t width, uint32_t height,
                    RetiredSessions& retired, size_t& retired_count) {
    const uint64_t now = now_ticks();
    ObservedSwapchain* observed = nullptr;
    for (auto& item : g_observed_swapchains) {
        if (item.generation == generation) {
            observed = &item;
            break;
        }
    }
    if (!observed) {
        if (g_observed_swapchains.size() >= 16) {
            const auto discard = std::find_if(
                g_observed_swapchains.begin(), g_observed_swapchains.end(),
                [](const ObservedSwapchain& item) {
                    return item.generation != g_primary_generation;
                });
            if (discard != g_observed_swapchains.end())
                g_observed_swapchains.erase(discard);
        }
        g_observed_swapchains.push_back({generation, hwnd, device_identity,
                                         width, height, 0, now});
        observed = &g_observed_swapchains.back();
    }

    const bool same_shape = observed->hwnd == hwnd &&
        observed->device_identity == device_identity &&
        observed->width == width && observed->height == height &&
        now - observed->last_seen_ms <= 500;
    observed->consecutive_presents = same_shape
        ? std::min(observed->consecutive_presents + 1, 1000u) : 1u;
    observed->hwnd = hwnd;
    observed->device_identity = device_identity;
    observed->width = width;
    observed->height = height;
    observed->last_seen_ms = now;

    if (!g_primary_generation) {
        // Sustained foreground presents reject launchers, setup probes, video,
        // and helper swapchains without relying on DLL-name allowlists.
        if (observed->consecutive_presents < 8 ||
            GetAncestor(GetForegroundWindow(), GA_ROOT) != hwnd)
            return false;
        g_primary_generation = generation;
        g_primary_hwnd = hwnd;
        g_retry_on_next_open.store(false, std::memory_order_release);
        g_renderer_healthy.store(true, std::memory_order_release);
        flog("[overlay-v2] selected primary game swapchain (%ux%u)", width, height);
    } else if (g_primary_generation != generation) {
        // Keep a healthy primary sticky.  A new swapchain is adopted only after
        // the old one has stopped presenting, as happens during a real rebuild.
        auto old = std::find_if(g_observed_swapchains.begin(), g_observed_swapchains.end(),
            [](const ObservedSwapchain& item) {
                return item.generation == g_primary_generation;
            });
        if (old != g_observed_swapchains.end() && now - old->last_seen_ms < 2000)
            return false;
        if (observed->consecutive_presents < 8 ||
            GetAncestor(GetForegroundWindow(), GA_ROOT) != hwnd)
            return false;
        if (g_resize_gate.depth != 0 || g_color_gate.depth != 0)
            return false;
        if (g_session && !g_session->gpu_idle())
            return false;
        retire_current_session(retired, retired_count);
        g_session_rebuild_pending = false;
        g_primary_generation = generation;
        g_primary_hwnd = hwnd;
        g_retry_on_next_open.store(false, std::memory_order_release);
        g_renderer_healthy.store(true, std::memory_order_release);
        flog("[overlay-v2] adopted replacement primary swapchain (%ux%u)", width, height);
    }
    return g_primary_generation == generation;
}

void before_present_impl(IDXGISwapChain* base,
                         dxgi_shadow::PresentKind present_kind, UINT flags,
                         const DXGI_PRESENT_PARAMETERS* parameters) {
    if (g_stopping.load(std::memory_order_acquire) ||
        !g_renderer_ready.load(std::memory_order_acquire) ||
        (flags & DXGI_PRESENT_TEST) != 0)
        return;

    // A proxy can synchronously Present another canonical swapchain while a
    // CustomTalismanEffects render operation is validating COM state.  The shadow layer's
    // identity-keyed recursion guard intentionally treats those as different
    // objects, so reject the cross-identity re-entry here before touching the
    // non-recursive renderer mutex already owned by this thread.
    if (g_present_render_critical)
        return;

    // Declared before the lock so all Session destruction (and its foreign COM
    // Release cascade) happens only after render_lock has been released.
    RetiredSessions retired_sessions{};
    size_t retired_session_count = 0;
    ComPtr<IDXGISwapChain3> swapchain3;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    std::unique_ptr<d3d12::Session> candidate;
    uint64_t generation = 0;
    void* device_id = nullptr;
    HWND hwnd = nullptr;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    std::unique_lock render_lock(g_render_mutex, std::try_to_lock);
    if (!render_lock.owns_lock()) return;
    PresentRenderCriticalScope present_critical;

    const uint64_t call_generation = dxgi_shadow::generation_token(base);
    if (!call_generation) return;

    // Lifetime identity must survive a partial alias installation so resize
    // and device-loss cleanup still work. Rendering eligibility is a separate,
    // stricter gate. If eligibility is ever revoked on the selected chain,
    // immediately withdraw the canvas and retire the session once its own
    // fence proves that destruction is safe.
    if (!dxgi_shadow::renderer_eligible(base)) {
        if (call_generation == g_primary_generation) {
            publish_canvas(g_primary_hwnd, 0, 0, false, false);
            if (g_session) {
                if (g_session->gpu_idle()) {
                    retire_current_session(retired_sessions,
                                           retired_session_count);
                    g_session_rebuild_pending = false;
                } else {
                    g_session_rebuild_pending = true;
                }
            }
            g_retry_on_next_open.store(false, std::memory_order_release);
            g_renderer_healthy.store(false, std::memory_order_release);
        }
        return;
    }
    if ((g_resize_gate.depth != 0 &&
         call_generation == g_resize_gate.generation) ||
        (g_color_gate.depth != 0 &&
         call_generation == g_color_gate.generation))
        return;

    if (g_session_rebuild_pending && g_session && g_session->gpu_idle()) {
        retire_current_session(retired_sessions, retired_session_count);
        g_session_rebuild_pending = false;
        g_retry_on_next_open.store(true, std::memory_order_release);
    }
    if (g_session_rebuild_pending) return;

    if (!describe_swapchain(base, swapchain3, device, generation, device_id,
                            hwnd, desc))
        return;

    // Queue identity is part of swapchain identity for D3D12.  Do not publish
    // a usable canvas (and therefore do not capture input) until the exact
    // queue supplied to CreateSwapChain* or ResizeBuffers1 is known.
    queue = queue_for_swapchain(generation, device_id);
    if (!queue) {
        if (generation == g_primary_generation)
            publish_canvas(hwnd, desc.Width, desc.Height, false, false);
        return;
    }
    if (!select_primary(generation, hwnd, device_id, desc.Width, desc.Height,
                        retired_sessions, retired_session_count))
        return;

    const uint32_t current_slot = swapchain3->GetCurrentBackBufferIndex();
    if (current_slot >= desc.BufferCount) return;
    bool already_composited = false;
    if (g_pending_composited_buffer.generation != 0) {
        if (g_pending_composited_buffer.generation == generation &&
            g_pending_composited_buffer.slot == current_slot) {
            // The preceding downstream Present did not consume this buffer.
            // Its CustomTalismanEffects pixels are already present; drawing again would
            // compound straight-alpha blending on a retry.
            already_composited = true;
        } else {
            g_pending_composited_buffer = {};
        }
    }

    const InferredColor color =
        infer_color_mode(generation, desc.Format, hwnd);
    const d3d12::ColorMode color_mode = color.mode;
    const bool color_known = color_mode != d3d12::ColorMode::Unknown;
    publish_canvas(hwnd, desc.Width, desc.Height,
                   color_mode == d3d12::ColorMode::ScRgb ||
                       color_mode == d3d12::ColorMode::Hdr10,
                   color_known);

    if (!color_known) {
        if (g_color_unknown_logged_generation != generation) {
            g_color_unknown_logged_generation = generation;
            flog("[overlay-v2] primary color encoding is unverified (format %u); canvas remains unavailable",
                 static_cast<unsigned>(desc.Format));
        }
        return; // detected nested R10/FP16 attachment is ambiguous; fail closed
    }

    // A renderer failure is sticky for this session.  Continue lifecycle and
    // canvas observation, but never repeat allocation/recording failures on
    // every Present.  Pending work or a replacement tuple explicitly rearms.
    if (!g_renderer_healthy.load(std::memory_order_acquire)) return;

    // Warm all private device-child state as soon as the primary tuple and
    // encoding are authoritative. Opening the menu then performs no heap/PSO/
    // backbuffer allocation on its first visible Present.
    if (g_session &&
        !g_session->matches(swapchain3.Get(), queue.Get(), color_mode)) {
        if (!g_session->gpu_idle()) return;
        retire_current_session(retired_sessions, retired_session_count);
        g_session_rebuild_pending = false;
    }
    if (!g_session) {
        candidate = std::make_unique<d3d12::Session>();
        bool initialized = false;
        try {
            initialized = candidate->initialize(
                swapchain3.Get(), queue.Get(), color_mode, color.evidence);
        } catch (...) {
            if (retired_session_count < retired_sessions.size()) {
                retired_sessions[retired_session_count++] =
                    std::move(candidate);
            } else {
                (void)candidate.release();
            }
            throw;
        }
        if (!initialized) {
            if (retired_session_count < retired_sessions.size()) {
                retired_sessions[retired_session_count++] =
                    std::move(candidate);
            } else {
                (void)candidate.release();
            }
            g_retry_on_next_open.store(true, std::memory_order_release);
            g_renderer_healthy.store(false, std::memory_order_release);
            return;
        }
        g_session = std::move(candidate);
        g_retry_on_next_open.store(false, std::memory_order_release);
        g_renderer_healthy.store(true, std::memory_order_release);
    }

    if (!g_visible.load(std::memory_order_acquire)) return;

    // A nonblocking Present is explicitly allowed to reject the frame without
    // consuming the current buffer. Never modify it in that mode. Likewise,
    // null parameters make Present1 invalid, whereas null denotes an ordinary
    // full-frame Present in the base interface.
    if ((flags & DXGI_PRESENT_DO_NOT_WAIT) != 0 ||
        (present_kind == dxgi_shadow::PresentKind::Present1 && !parameters) ||
        already_composited)
        return;

    // A generic overlay cannot modify pixels outside Present1's declared dirty
    // regions without violating DXGI's partial-present contract.  Elden Ring
    // uses full presents; unfamiliar incremental chains are passed through.
    if (parameters && (parameters->DirtyRectsCount != 0 ||
                       parameters->pScrollRect != nullptr ||
                       parameters->pScrollOffset != nullptr))
        return;

    const auto packet = frame::acquire_frame();
    const auto font = frame::acquire_font_atlas();
    if (!packet || !font || packet->font_generation != font->generation ||
        packet->commands.empty())
        return;

    // Do not stretch a stale pre-resize UI packet onto a new buffer.  The
    // control thread will publish the correctly sized frame on its next tick.
    const double expected_w_value =
        static_cast<double>(packet->display_size.x) *
        packet->framebuffer_scale.x + 0.5;
    const double expected_h_value =
        static_cast<double>(packet->display_size.y) *
        packet->framebuffer_scale.y + 0.5;
    if (!std::isfinite(expected_w_value) ||
        !std::isfinite(expected_h_value) || expected_w_value < 0.0 ||
        expected_h_value < 0.0 ||
        expected_w_value > std::numeric_limits<uint32_t>::max() ||
        expected_h_value > std::numeric_limits<uint32_t>::max())
        return;
    const auto expected_w = static_cast<uint32_t>(expected_w_value);
    const auto expected_h = static_cast<uint32_t>(expected_h_value);
    if (expected_w != desc.Width || expected_h != desc.Height) return;

    const d3d12::RenderResult result =
        g_session->render(swapchain3.Get(), packet.get(), font.get(), color_mode);
    if (result == d3d12::RenderResult::Submitted) {
        g_pending_composited_buffer = {generation, current_slot};
    } else if (result == d3d12::RenderResult::RecoverableFailure) {
        // No command list was submitted for this result.  Rebuild every
        // renderer-owned object on the next explicit open instead of carrying
        // partially committed font/descriptor bookkeeping forward.
        flog("[overlay-v2] renderer recording failed; discarding private session");
        g_session_rebuild_pending = true;
        g_renderer_healthy.store(false, std::memory_order_release);
    } else if (result == d3d12::RenderResult::DeviceLost) {
        const HRESULT reason = g_session->device_removed_reason();
        flog("[overlay-v2] D3D12 device lost while rendering (0x%08X); pass-through",
             static_cast<unsigned>(reason));
        if (FAILED(reason)) {
            retire_current_session(retired_sessions, retired_session_count);
            g_session_rebuild_pending = false;
        } else {
            // Execute may have succeeded even though Signal failed.  Without a
            // fence or confirmed removal there is no proof the GPU is done.
            // Intentionally retain the session for process life.
            (void)g_session.release();
            g_session_rebuild_pending = false;
            flog("[overlay-v2] [WARN] unfenced session quarantined after queue failure");
        }
        publish_canvas(g_primary_hwnd, 0, 0, false, false);
        g_retry_on_next_open.store(false, std::memory_order_release);
        g_renderer_healthy.store(false, std::memory_order_release);
    }
}

void safe_before_present(IDXGISwapChain* swapchain,
                         dxgi_shadow::PresentKind present_kind, UINT flags,
                         const DXGI_PRESENT_PARAMETERS* parameters) noexcept {
    try {
        before_present_impl(swapchain, present_kind, flags, parameters);
    } catch (...) {
        // C++ failures are contained before the downstream call.  Structured
        // exceptions are not caught across RAII-held mutexes because doing so
        // would skip destructors and permanently poison the hook path.
        try {
            std::unique_lock lock(g_render_mutex, std::try_to_lock);
            if (lock.owns_lock() && g_session)
                g_session_rebuild_pending = true;
            else if (lock.owns_lock())
                g_retry_on_next_open.store(true, std::memory_order_release);
        } catch (...) {
        }
        g_renderer_healthy.store(false, std::memory_order_release);
    }
}

void after_present(IDXGISwapChain* swapchain, HRESULT result) noexcept {
    if (g_stopping.load(std::memory_order_acquire)) return;
    if (result != DXGI_ERROR_DEVICE_REMOVED && result != DXGI_ERROR_DEVICE_RESET)
        return;
    if (g_present_render_critical) {
        g_renderer_healthy.store(false, std::memory_order_release);
        g_retry_on_next_open.store(false, std::memory_order_release);
        return;
    }
    try {
        const uint64_t generation =
            dxgi_shadow::generation_token(swapchain);
        std::unique_ptr<d3d12::Session> retired_session;
        {
            std::lock_guard lock(g_render_mutex);
            if (generation != g_primary_generation) return;
            if (g_session) {
                const HRESULT reason = g_session->device_removed_reason();
                if (FAILED(reason)) {
                    retired_session = std::move(g_session);
                } else {
                    // Present failed but the device does not confirm removal.
                    // Its most recent submission may still be live; quarantine
                    // it without running a destructor under the renderer lock.
                    (void)g_session.release();
                }
                g_session_rebuild_pending = false;
            }
            g_renderer_healthy.store(false, std::memory_order_release);
            g_retry_on_next_open.store(false, std::memory_order_release);
            g_pending_composited_buffer = {};
            publish_canvas(g_primary_hwnd, 0, 0, false, false);
        }
    } catch (...) {
        // A removed device is already unusable; pass-through remains active.
    }
}

bool before_resize_impl(IDXGISwapChain* swapchain) {
    if (g_stopping.load(std::memory_order_acquire)) return true;
    if (g_present_render_critical) {
        g_renderer_healthy.store(false, std::memory_order_release);
        g_retry_on_next_open.store(false, std::memory_order_release);
        return false;
    }
    if (!swapchain) return true;
    const uint64_t generation = dxgi_shadow::generation_token(swapchain);
    std::unique_ptr<d3d12::Session> retiring_session;
    {
        std::lock_guard lock(g_render_mutex);
        if (generation == 0 || generation != g_primary_generation)
            return true;

        const bool already_retiring =
            g_resize_gate.depth != 0 &&
            g_resize_gate.generation == generation;
        if (g_resize_gate.depth == 0) g_resize_gate.generation = generation;
        if (g_resize_gate.generation == generation) ++g_resize_gate.depth;
        publish_canvas(g_primary_hwnd, 0, 0, false, false);

        // Concurrent ResizeBuffers calls on one swapchain are invalid host
        // behavior. The first owner may currently hold the only session outside
        // this mutex while draining it, so later calls must report that
        // resources were not released and let DXGI reject the overlap.
        if (already_retiring && !g_session)
            return false;

        if (g_session) {
            retiring_session = std::move(g_session);
            g_session_rebuild_pending = false;
        }
    }

    if (!retiring_session) return true;

    // The generation-keyed resize gate blocks matching Presents and prevents
    // primary replacement while this runs. D3D12/COM resource retirement is
    // intentionally outside g_render_mutex so a proxy Release cannot re-enter
    // a blocking lifecycle callback and deadlock the process.
    if (retiring_session->before_resize())
        return true;

    {
        std::lock_guard lock(g_render_mutex);
        if (!g_stopping.load(std::memory_order_acquire) &&
            generation == g_primary_generation && !g_session) {
            // Keep the live references so the downstream resize fails safely.
            g_session = std::move(retiring_session);
        }
        g_renderer_healthy.store(false, std::memory_order_release);
        g_retry_on_next_open.store(true, std::memory_order_release);
    }
    if (retiring_session) {
        // Coordinator ownership changed while the fence wait was in flight.
        // Completion is unproven, so quarantine instead of destroying.
        (void)retiring_session.release();
    }
    return false;
}

bool safe_before_resize(IDXGISwapChain* swapchain) noexcept {
    try {
        return before_resize_impl(swapchain);
    } catch (...) {
        g_renderer_healthy.store(false, std::memory_order_release);
        return false;
    }
}

void after_resize(IDXGISwapChain* swapchain, HRESULT result,
                  bool resources_released) noexcept {
    try {
        if (g_present_render_critical) {
            g_renderer_healthy.store(false, std::memory_order_release);
            g_retry_on_next_open.store(false, std::memory_order_release);
            return;
        }
        const uint64_t generation =
            dxgi_shadow::generation_token(swapchain);
        std::lock_guard lock(g_render_mutex);
        if (g_resize_gate.generation == generation &&
            g_resize_gate.depth != 0) {
            --g_resize_gate.depth;
            if (g_resize_gate.depth == 0) g_resize_gate.generation = 0;
        }
        if (generation != g_primary_generation) return;
        if (g_stopping.load(std::memory_order_acquire)) return;
        if (FAILED(result)) {
            if (result == DXGI_ERROR_DEVICE_REMOVED ||
                result == DXGI_ERROR_DEVICE_RESET) {
                g_retry_on_next_open.store(false, std::memory_order_release);
                g_renderer_healthy.store(false, std::memory_order_release);
            }
            return;
        }
        if (!resources_released) {
            // This should be unreachable for a conforming ResizeBuffers call:
            // our retained backbuffer refs should make it fail.  If a wrapper
            // nevertheless reports success, retain the unfenced old session
            // for process life and rediscover the replacement from scratch.
            if (g_session) (void)g_session.release();
            g_session_rebuild_pending = false;
            g_retry_on_next_open.store(false, std::memory_order_release);
            g_renderer_healthy.store(false, std::memory_order_release);
            flog("[overlay-v2] [WARN] resize succeeded with quarantined GPU resources");
            return;
        }
        g_pending_composited_buffer = {};
        g_retry_on_next_open.store(false, std::memory_order_release);
        g_renderer_healthy.store(true, std::memory_order_release);
    } catch (...) {
        g_renderer_healthy.store(false, std::memory_order_release);
    }
}

void begin_color_space(IDXGISwapChain3* swapchain) noexcept {
    try {
        if (g_present_render_critical) {
            g_renderer_healthy.store(false, std::memory_order_release);
            g_retry_on_next_open.store(false, std::memory_order_release);
            return;
        }
        const uint64_t generation =
            dxgi_shadow::generation_token(swapchain);
        if (!generation) return;
        std::lock_guard lock(g_render_mutex);
        if (generation == g_primary_generation) {
            if (g_color_gate.depth == 0) g_color_gate.generation = generation;
            if (g_color_gate.generation == generation) ++g_color_gate.depth;
        }
    } catch (...) {
        g_renderer_healthy.store(false, std::memory_order_release);
    }
}

void complete_color_space(IDXGISwapChain3* swapchain,
                          DXGI_COLOR_SPACE_TYPE color_space,
                          HRESULT result) noexcept {
    try {
        if (g_present_render_critical) {
            g_renderer_healthy.store(false, std::memory_order_release);
            g_retry_on_next_open.store(false, std::memory_order_release);
            return;
        }
        const uint64_t generation =
            dxgi_shadow::generation_token(swapchain);
        if (!generation) return;
        const bool successful =
            !g_stopping.load(std::memory_order_acquire) && SUCCEEDED(result);
        const uint64_t observed_sequence =
            successful ? next_color_event_sequence() : 0;
        std::lock_guard lock(g_render_mutex);
        if (g_color_gate.generation == generation &&
            g_color_gate.depth != 0) {
            --g_color_gate.depth;
            if (g_color_gate.depth == 0) g_color_gate.generation = 0;
        }
        const bool primary = generation == g_primary_generation;
        if (!successful) return;
        ColorObservation* record = color_record(generation);
        record->color_space = color_space;
        record->event_sequence = observed_sequence;
        record->known = true;
        flog("[overlay-v2] observed successful SetColorSpace1 "
             "(generation=%llu, space=%u, primary=%s)",
             static_cast<unsigned long long>(generation),
             static_cast<unsigned>(color_space), primary ? "yes" : "no");
        if (primary) {
            // A mode mismatch retires/rebuilds the private Session on the next
            // Present. CustomTalismanEffects never changes host color state itself.
            const bool hdr =
                color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                color_space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
            const Canvas old = canvas();
            publish_canvas(old.hwnd, old.width, old.height, hdr, old.ready);
        }
    } catch (...) {
        g_renderer_healthy.store(false, std::memory_order_release);
    }
}

// Adapters from the per-instance vtable layer into the renderer coordinator.
// The shadow module owns exact per-object downstream pointers; these callbacks
// never call DXGI themselves and therefore cannot bypass another hook.
void shadow_before_present(IDXGISwapChain* swapchain,
                           const dxgi_shadow::PresentCall& call) {
    safe_before_present(swapchain, call.kind, call.flags, call.parameters);
}

void shadow_after_present(IDXGISwapChain* swapchain,
                          const dxgi_shadow::PresentCall&, HRESULT result) {
    after_present(swapchain, result);
}

bool shadow_before_resize(IDXGISwapChain* swapchain,
                          const dxgi_shadow::ResizeCall&) {
    if (g_resize_transaction.depth == 0)
        g_resize_transaction.id = next_binding_transaction();
    ++g_resize_transaction.depth;
    return safe_before_resize(swapchain);
}

void shadow_after_resize(IDXGISwapChain* swapchain,
                         const dxgi_shadow::ResizeCall& call, HRESULT result,
                         bool resources_released) {
    struct TransactionEnd {
        ~TransactionEnd() {
            if (g_resize_transaction.depth != 0)
                --g_resize_transaction.depth;
            if (g_resize_transaction.depth == 0)
                g_resize_transaction.id = 0;
        }
    } transaction_end;
    const uint64_t transaction_id = g_resize_transaction.id;
    const uint32_t evidence_depth = g_resize_transaction.depth;
    if (!g_stopping.load(std::memory_order_acquire) && SUCCEEDED(result) &&
        call.kind == dxgi_shadow::ResizeKind::ResizeBuffers1) {
        ComPtr<IDXGISwapChain3> swapchain3;
        if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain3))) &&
            swapchain3) {
            bind_resize_queues(swapchain3.Get(), call.buffer_count,
                               call.creation_node_masks, call.present_queues,
                               transaction_id, evidence_depth);
        }
    }
    after_resize(swapchain, result, resources_released);
}

void shadow_before_color_space(IDXGISwapChain3* swapchain,
                               DXGI_COLOR_SPACE_TYPE) {
    begin_color_space(swapchain);
}

void shadow_after_color_space(IDXGISwapChain3* swapchain,
                              DXGI_COLOR_SPACE_TYPE color_space,
                              HRESULT result) {
    complete_color_space(swapchain, color_space, result);
}

// -------------------------------------------------------------------------
// Hook functions and immediate-next trampolines

using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using NvapiGetDisplayIdByNameFn =
    int(__cdecl*)(const char*, uint32_t*);
using NvapiHdrColorControlFn =
    int(__cdecl*)(uint32_t, NvapiHdrColorDataHeader*);

CreateSwapChainFn g_next_create_swapchain = nullptr;
CreateSwapChainForHwndFn g_next_create_swapchain_for_hwnd = nullptr;
NvapiGetDisplayIdByNameFn g_next_nvapi_get_display_id_by_name = nullptr;
NvapiHdrColorControlFn g_next_nvapi_hdr_color_control = nullptr;

int __cdecl nvapi_get_display_id_by_name_detour(const char* display_name,
                                                 uint32_t* display_id) {
    const int status =
        g_next_nvapi_get_display_id_by_name(display_name, display_id);
    if (!g_stopping.load(std::memory_order_acquire) &&
        g_nvapi_observer_enabled.load(std::memory_order_acquire) &&
        status == 0 && display_name && display_id) {
        try {
            record_nvapi_display_binding(copy_output_name(display_name),
                                         *display_id);
        } catch (...) {
            // The observer is optional. Never change the game's NVAPI result
            // or poison the generic DXGI backend because diagnostics failed.
        }
    }
    return status;
}

int __cdecl nvapi_hdr_color_control_detour(
    uint32_t display_id, NvapiHdrColorDataHeader* color_data) {
    constexpr uint32_t kEldenRingHdrColorDataV1 = 0x00010028u;
    const bool request_valid =
        color_data && color_data->version == kEldenRingHdrColorDataV1;
    const int32_t command = request_valid ? color_data->command : -1;
    const int32_t hdr_mode = request_valid ? color_data->hdr_mode : -1;

    const int status = g_next_nvapi_hdr_color_control(display_id, color_data);
    if (!g_stopping.load(std::memory_order_acquire) &&
        g_nvapi_observer_enabled.load(std::memory_order_acquire) &&
        status == 0 && request_valid &&
        command == static_cast<int32_t>(NvapiHdrCommand::Set)) {
        try {
            record_nvapi_color_observation(display_id, hdr_mode);
        } catch (...) {
            // Observation is fail-closed and must never alter host behavior.
        }
    }
    return status;
}

bool install_swapchain_shadow(IDXGISwapChain* swapchain) noexcept {
    const dxgi_shadow::InstallResult result = dxgi_shadow::install(swapchain);
    switch (result) {
    case dxgi_shadow::InstallResult::Installed:
    case dxgi_shadow::InstallResult::AlreadyInstalled:
        return dxgi_shadow::generation_token(swapchain) != 0 &&
               dxgi_shadow::renderer_eligible(swapchain);
    case dxgi_shadow::InstallResult::PartiallyInstalled:
        flog("[overlay-v2] [WARN] swapchain aliases only partially shadowed; fail-closed on missing aliases");
        return false;
    case dxgi_shadow::InstallResult::Collision:
        flog("[overlay-v2] [WARN] concurrent swapchain vtable change; alias left untouched");
        return false;
    case dxgi_shadow::InstallResult::OutOfMemory:
        flog("[overlay-v2] [ERROR] swapchain shadow allocation failed");
        return false;
    case dxgi_shadow::InstallResult::InvalidObject:
        flog("[overlay-v2] [WARN] unsupported swapchain interface; pass-through only");
        return false;
    }
    return false;
}

HRESULT STDMETHODCALLTYPE create_swapchain_detour(
    IDXGIFactory* factory, IUnknown* queue, DXGI_SWAP_CHAIN_DESC* desc,
    IDXGISwapChain** result_swapchain) {
    BindingScope binding_scope(g_creation_transaction);
    const HRESULT result =
        g_next_create_swapchain(factory, queue, desc, result_swapchain);
    if (!g_stopping.load(std::memory_order_acquire) && SUCCEEDED(result) &&
        result_swapchain && *result_swapchain &&
        eligible_shadow_candidate(*result_swapchain) &&
        install_swapchain_shadow(*result_swapchain) &&
        bind_swapchain_queue(*result_swapchain, queue,
                             binding_scope.id(), binding_scope.depth())) {
        record_creation_color_default(*result_swapchain, binding_scope.id(),
                                      binding_scope.depth());
    }
    return result;
}

HRESULT STDMETHODCALLTYPE create_swapchain_for_hwnd_detour(
    IDXGIFactory2* factory, IUnknown* queue, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
    IDXGIOutput* restrict_to_output, IDXGISwapChain1** result_swapchain) {
    BindingScope binding_scope(g_creation_transaction);
    const HRESULT result = g_next_create_swapchain_for_hwnd(
        factory, queue, hwnd, desc, fullscreen_desc, restrict_to_output,
        result_swapchain);
    if (!g_stopping.load(std::memory_order_acquire) && SUCCEEDED(result) &&
        result_swapchain && *result_swapchain &&
        eligible_shadow_candidate(*result_swapchain) &&
        install_swapchain_shadow(*result_swapchain) &&
        bind_swapchain_queue(*result_swapchain, queue,
                             binding_scope.id(), binding_scope.depth())) {
        record_creation_color_default(*result_swapchain, binding_scope.id(),
                                      binding_scope.depth());
    }
    return result;
}

struct HookTargets {
    void* create_swapchain = nullptr;
    void* create_swapchain_for_hwnd = nullptr;
    void* nvapi_get_display_id_by_name = nullptr;
    void* nvapi_hdr_color_control = nullptr;
    // The factory's shared vtable, for slot interception (see
    // install_factory_slot). Null falls back to code patching.
    void** factory_vtable = nullptr;
};

// ── DXGI creation interception: vtable slot, not code patch ──
// DEVIATION FROM QUESTPATH, and the reason this mod's overlay works while
// QuestPath is loaded. QuestPath MinHook-patches the first five bytes of
// dxgi!CreateSwapChain(ForHwnd). Two injected DLLs each running their own
// MinHook cannot do that concurrently: MH_CreateHook snapshots the target's
// bytes to build its trampoline, so when both create before either applies,
// both trampolines hold the ORIGINAL bytes and the second MH_ApplyQueued
// simply overwrites the first mod's JMP -- the loser's detour is silently
// orphaned for the rest of the session (observed 2026-07-19: both mods logged
// "hooks installed" in the same millisecond; only QuestPath ever saw a
// creation). MinHook chains correctly ONLY if the second create happens after
// the first patch is already live, which nothing serializes.
//
// A factory vtable slot is a different layer, so it cannot collide with a code
// patch in either load order. All DXGI factory instances of dxgi.dll's factory
// class share this vtable, so replacing the slot intercepts the game's factory
// too, and the previous slot value stays our immediate-next:
//
//   game -> our detour -> dxgi!CreateSwapChainForHwnd (QuestPath's JMP, if it
//           patched) -> QuestPath's detour -> its trampoline -> real function
//
// Both mods observe every creation, whichever loaded first. A single aligned
// pointer store is atomic on x64, so a concurrent caller reads either the old
// or the new slot -- never a torn pointer.
bool install_factory_slot(void** vtable, size_t index, void* detour,
                          void** next_out) noexcept {
    if (!vtable || !detour || !next_out) return false;
    void** slot = vtable + index;
    DWORD previous_protection = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE,
                        &previous_protection))
        return false;
    // Capture whatever is there now -- possibly another tool's detour, which
    // must keep running as our immediate-next.
    *next_out = *slot;
    *slot = detour;
    DWORD restored = 0;
    VirtualProtect(slot, sizeof(void*), previous_protection, &restored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return *next_out != nullptr;
}

bool is_elden_ring_process() noexcept {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    const wchar_t* name = path;
    for (const wchar_t* cursor = path; *cursor != L'\0'; ++cursor) {
        if (*cursor == L'\\' || *cursor == L'/') name = cursor + 1;
    }
    return lstrcmpiW(name, L"eldenring.exe") == 0;
}

void* find_nvapi_wrapper(const std::array<uint8_t, 13>& prefix,
                         const std::array<uint8_t, 12>& suffix,
                         size_t function_delta) noexcept {
    HMODULE module = GetModuleHandleW(nullptr);
    if (!module) return nullptr;
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        base + static_cast<size_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return nullptr;

    constexpr size_t kSignatureSize = 29;
    constexpr size_t kWildcardOffset = 13;
    constexpr size_t kWildcardSize = 4;
    constexpr size_t kMaximumScanBytes = 0x20000;
    const size_t code_offset = nt->OptionalHeader.BaseOfCode;
    if (code_offset >= nt->OptionalHeader.SizeOfImage) return nullptr;
    const size_t scan_size = std::min<size_t>(
        kMaximumScanBytes, nt->OptionalHeader.SizeOfImage - code_offset);
    if (scan_size < kSignatureSize || function_delta > code_offset)
        return nullptr;

    const uint8_t* scan = base + code_offset;
    const uint8_t* unique = nullptr;
    for (size_t offset = 0; offset + kSignatureSize <= scan_size; ++offset) {
        const uint8_t* candidate = scan + offset;
        if (std::memcmp(candidate, prefix.data(), prefix.size()) != 0 ||
            std::memcmp(candidate + kWildcardOffset + kWildcardSize,
                        suffix.data(), suffix.size()) != 0)
            continue;
        const uint8_t* function = candidate - function_delta;
        if (function < base + code_offset) continue;
        if (unique && unique != function) return nullptr;
        unique = function;
    }
    return const_cast<uint8_t*>(unique);
}

void resolve_nvapi_hook_targets(HookTargets& targets) noexcept {
    if (!is_elden_ring_process()) return;

    // These are the stable lazy-resolution interiors emitted by the NVAPI
    // import library in the supported Elden Ring executable. Scanning only the
    // first 128 KiB keeps authoritative DXGI creation hooks on the cold path.
    // Wildcards cover the RIP-relative cache displacement; deriving the entry
    // from the interior remains compatible with an earlier prologue hook.
    constexpr std::array<uint8_t, 13> kDisplayPrefix = {
        0xB9, 0x90, 0x71, 0x45, 0xAE, 0xFF, 0xD0,
        0x48, 0x8B, 0xD8, 0x48, 0x89, 0x05};
    constexpr std::array<uint8_t, 12> kDisplaySuffix = {
        0x48, 0x85, 0xC0, 0x75, 0x07, 0xB8,
        0xFD, 0xFF, 0xFF, 0xFF, 0xEB, 0x48};
    constexpr std::array<uint8_t, 13> kHdrPrefix = {
        0xB9, 0x24, 0xA2, 0x1D, 0x35, 0xFF, 0xD0,
        0x48, 0x8B, 0xD8, 0x48, 0x89, 0x05};
    constexpr std::array<uint8_t, 12> kHdrSuffix = {
        0x48, 0x85, 0xC0, 0x75, 0x07, 0xB8,
        0xFD, 0xFF, 0xFF, 0xFF, 0xEB, 0x47};

    targets.nvapi_get_display_id_by_name =
        find_nvapi_wrapper(kDisplayPrefix, kDisplaySuffix, 0x44);
    targets.nvapi_hdr_color_control =
        find_nvapi_wrapper(kHdrPrefix, kHdrSuffix, 0x43);
    if (!targets.nvapi_get_display_id_by_name ||
        !targets.nvapi_hdr_color_control) {
        targets.nvapi_get_display_id_by_name = nullptr;
        targets.nvapi_hdr_color_control = nullptr;
        flog("[overlay-v2] [WARN] Elden Ring NVAPI HDR observer signature unavailable; vendor HDR remains unverified");
    }
}

bool resolve_hook_targets(HookTargets& targets) {
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) || !factory)
        return false;

    // Only creation entrypoints are process-wide.  Per-frame interception is
    // installed on the returned swapchain instance, so bootstrap needs no
    // probe HWND, D3D device, command queue, or private swapchain.
    void** vtable = *reinterpret_cast<void***>(factory.Get());
    if (!vtable) return false;
    targets.factory_vtable = vtable;
    targets.create_swapchain = vtable[10];
    targets.create_swapchain_for_hwnd = vtable[15];
    resolve_nvapi_hook_targets(targets);
    return targets.create_swapchain && targets.create_swapchain_for_hwnd;
}

template <typename Fn>
bool queue_hook(void* target, void* detour, Fn& next, const char* name) {
    if (!target || !hooks::create(target, detour,
                                  reinterpret_cast<void**>(&next))) {
        flog("[overlay-v2] [ERROR] failed to create %s hook", name);
        return false;
    }
    return true;
}

bool install_all_hooks(const HookTargets& target) {
    bool ok = true;
    // The MinHook portion of this batch (the NVAPI observer, plus the DXGI
    // code-patch fallback below) is one create->apply transaction and MUST be
    // serialized against any other mod embedding this backend. An interleaved
    // install erases one side's jmp with every call still returning MH_OK --
    // see hooks.hpp. The vtable-slot interception is a different layer and
    // needs no lock, but taking it here covers the whole function uniformly.
    hooks::InstallLock install_lock;

    // Slot interception first (see install_factory_slot for why). Code patching
    // remains the fallback for the case where the vtable page cannot be made
    // writable, which is also the only configuration that can still lose a
    // create/apply race against another MinHook user.
    const bool slots_installed =
        target.factory_vtable &&
        install_factory_slot(target.factory_vtable, 10,
                             reinterpret_cast<void*>(&create_swapchain_detour),
                             reinterpret_cast<void**>(&g_next_create_swapchain)) &&
        install_factory_slot(target.factory_vtable, 15,
                             reinterpret_cast<void*>(&create_swapchain_for_hwnd_detour),
                             reinterpret_cast<void**>(&g_next_create_swapchain_for_hwnd));
    if (slots_installed) {
        flog("[overlay-v2] DXGI creation intercepted via factory vtable slots "
             "(coexists with code-patching overlays)");
    } else {
        flog("[overlay-v2] [WARN] factory vtable interception unavailable; "
             "falling back to code patching");
        ok &= queue_hook(target.create_swapchain,
                         reinterpret_cast<void*>(&create_swapchain_detour),
                         g_next_create_swapchain, "CreateSwapChain");
        ok &= queue_hook(target.create_swapchain_for_hwnd,
                         reinterpret_cast<void*>(&create_swapchain_for_hwnd_detour),
                         g_next_create_swapchain_for_hwnd, "CreateSwapChainForHwnd");
    }

    bool nvapi_targets_present = false;
    bool nvapi_observer_ready = false;
    if (target.nvapi_get_display_id_by_name &&
        target.nvapi_hdr_color_control) {
        nvapi_targets_present = true;
        const bool display_hook_created = hooks::create(
            target.nvapi_get_display_id_by_name,
            reinterpret_cast<void*>(&nvapi_get_display_id_by_name_detour),
            reinterpret_cast<void**>(&g_next_nvapi_get_display_id_by_name));
        const bool hdr_hook_created = hooks::create(
            target.nvapi_hdr_color_control,
            reinterpret_cast<void*>(&nvapi_hdr_color_control_detour),
            reinterpret_cast<void**>(&g_next_nvapi_hdr_color_control));
        nvapi_observer_ready = display_hook_created && hdr_hook_created;
        // A partial optional pair may still be enabled by the transaction, but
        // both detours remain exact pass-through unless this gate is true.
        g_nvapi_observer_enabled.store(nvapi_observer_ready,
                                       std::memory_order_release);
    }

    const bool applied = hooks::apply();
    if (!ok || !applied) {
        // Some hooks may already be active (and queued successes could be
        // applied by another subsystem later).  Leave every callback safely
        // pass-through rather than attempting a chain-destructive rollback.
        g_nvapi_observer_enabled.store(false, std::memory_order_release);
        g_stopping.store(true, std::memory_order_release);
        return false;
    }

    if (nvapi_targets_present) {
        if (nvapi_observer_ready) {
            flog("[overlay-v2] NVIDIA NVAPI HDR observer installed");
        } else {
            flog("[overlay-v2] [WARN] NVIDIA NVAPI HDR observer unavailable; generic DXGI backend remains active");
        }
    }
    return true;
}

} // namespace

bool install_hooks() {
    if (g_installed.load(std::memory_order_acquire))
        return g_renderer_ready.load(std::memory_order_acquire);
    if (!hooks::init()) {
        flog("[overlay-v2] [ERROR] MinHook initialization failed");
        return false;
    }

    // Pin the DLL.  A later injector can legitimately build a trampoline that
    // enters our detour; unloading our code from the middle of that chain is
    // not safely reversible without cooperation from every participant.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&install_hooks), &pinned) ||
        pinned != g_hinst) {
        flog("[overlay-v2] [ERROR] could not pin hook module; refusing interception");
        return false;
    }

    HookTargets targets;
    if (!resolve_hook_targets(targets)) {
        flog("[overlay-v2] [ERROR] DXGI/D3D12 hook target discovery failed");
        return false;
    }
    dxgi_shadow::Callbacks callbacks{};
    callbacks.before_present = &shadow_before_present;
    callbacks.after_present = &shadow_after_present;
    callbacks.before_resize = &shadow_before_resize;
    callbacks.after_resize = &shadow_after_resize;
    callbacks.before_color_space = &shadow_before_color_space;
    callbacks.after_color_space = &shadow_after_color_space;
    if (!dxgi_shadow::configure_callbacks(callbacks)) {
        flog("[overlay-v2] [ERROR] swapchain callback publication failed");
        return false;
    }
    if (!install_all_hooks(targets)) {
        dxgi_shadow::clear_callbacks();
        flog("[overlay-v2] [ERROR] hook transaction failed; backend unavailable");
        return false;
    }

    g_installed.store(true, std::memory_order_release);
    flog("[overlay-v2] DXGI creation hooks installed; Present uses per-instance shadows");

    // Queue discovery is the only part that cannot be recovered after the
    // game creates its swapchain, so hooks go live before shader compilation.
    // Present callbacks remain pass-through until this worker finishes; this
    // avoids both a creation race and shader compilation on a Present thread.
    if (!d3d12::prepare_shaders()) {
        g_renderer_healthy.store(false, std::memory_order_release);
        flog("[overlay-v2] [ERROR] renderer shader preparation failed; interception remains pass-through");
        return false;
    }
    g_renderer_ready.store(true, std::memory_order_release);
    flog("[overlay-v2] renderer shaders prepared; swapchain rendering enabled");
    return true;
}

bool hooks_installed() {
    return g_installed.load(std::memory_order_acquire);
}

Canvas canvas() {
    Canvas result;
    for (;;) {
        const uint64_t before = g_canvas.sequence.load(std::memory_order_acquire);
        if (before & 1u) continue;
        result.hwnd = reinterpret_cast<HWND>(
            g_canvas.hwnd.load(std::memory_order_relaxed));
        result.width = g_canvas.width.load(std::memory_order_relaxed);
        result.height = g_canvas.height.load(std::memory_order_relaxed);
        result.hdr = g_canvas.hdr.load(std::memory_order_relaxed);
        result.ready = g_canvas.ready.load(std::memory_order_relaxed);
        const uint64_t after = g_canvas.sequence.load(std::memory_order_acquire);
        if (before == after) return result;
    }
}

void set_visible(bool value) {
    if (value) {
        // A recoverable allocation/recording failure is sticky while the menu
        // remains open. A deliberate close/reopen is the only user-visible
        // retry boundary, so a bad frame cannot create a per-Present loop.
        const bool was_visible = g_visible.load(std::memory_order_acquire);
        if (!was_visible) g_packet_skip_warning_logged = false;
        if (!was_visible &&
            g_retry_on_next_open.exchange(false, std::memory_order_acq_rel))
            g_renderer_healthy.store(true, std::memory_order_release);
        g_visible.store(true, std::memory_order_release);
    } else {
        g_visible.store(false, std::memory_order_release);
        frame::clear_frame();
    }
}

bool visible() {
    return g_visible.load(std::memory_order_acquire);
}

void publish_draw_data(const ImDrawData* draw_data) {
    const uint64_t font_generation = g_font_generation.load(std::memory_order_acquire);
    const frame::frame_publish_result result = frame::publish_draw_data(
        draw_data, kFontTextureToken, font_generation);
    if (result.status == frame::publish_status::published_with_skips &&
        !g_packet_skip_warning_logged) {
        g_packet_skip_warning_logged = true;
        flog("[overlay-v2] [WARN] frame %llu skipped %u callbacks and %u textures",
             static_cast<unsigned long long>(result.generation),
             result.skipped_user_callbacks, result.skipped_unsupported_textures);
    }
    if (result.was_published() && !result.has_draw_commands &&
        g_visible.load(std::memory_order_acquire)) {
        // Input must never remain captured for a menu that cannot produce even
        // one supported draw command. The control thread observes unhealthy,
        // closes within its bounded grace period, and rearms on an explicit
        // later open after the underlying frontend problem is corrected.
        g_retry_on_next_open.store(true, std::memory_order_release);
        g_renderer_healthy.store(false, std::memory_order_release);
    }
}

void publish_font_atlas(ImFontAtlas* atlas) {
    if (!atlas) return;
    const frame::font_publish_result result = frame::publish_font_atlas(atlas);
    if (result.was_published()) {
        // GetTexDataAsRGBA32 builds a dirty atlas, and ImGui's build path resets
        // TexID to null. Assign the frontend token only after that build. Doing
        // this before publication makes every later draw command look like an
        // unsupported null texture even though the pixels copied correctly.
        atlas->SetTexID(kFontTextureToken);
        g_font_generation.store(result.generation, std::memory_order_release);
    } else {
        flog("[overlay-v2] [ERROR] failed to publish ImGui font atlas");
        // The frontend atlas may already contain new UVs. Never let a later
        // renderer retry pair those frames with the previously published GPU
        // atlas after this publication failed.
        g_font_generation.store(0, std::memory_order_release);
        frame::clear();
        g_renderer_healthy.store(false, std::memory_order_release);
    }
}

void clear_draw_data() { frame::clear_frame(); }

bool renderer_healthy() {
    return g_renderer_healthy.load(std::memory_order_acquire);
}

void shutdown() {
    g_stopping.store(true, std::memory_order_release);
    g_renderer_ready.store(false, std::memory_order_release);
    g_visible.store(false, std::memory_order_release);
    dxgi_shadow::clear_callbacks();
    frame::clear();

    // Callback publication is now null and g_stopping makes a previously
    // captured callback pass-through. Wait for the one possible renderer
    // owner, then either retire or intentionally quarantine its resources.
    // Destruction is deliberately outside both backend mutexes because COM
    // Release on a third-party proxy is foreign, potentially re-entrant code.
    std::unique_ptr<d3d12::Session> retired_session;
    {
        std::lock_guard lock(g_render_mutex);
        publish_canvas(nullptr, 0, 0, false, false);
        retired_session = std::move(g_session);
        g_session_rebuild_pending = false;
        g_primary_generation = 0;
        g_primary_hwnd = nullptr;
        g_color_unknown_logged_generation = 0;
        g_resize_gate = {};
        g_color_gate = {};
        g_pending_composited_buffer = {};
        g_observed_swapchains.clear();
        g_color_observations.clear();
        g_nvapi_color_cache = {};
    }
    g_nvapi_observer_enabled.store(false, std::memory_order_release);
    {
        std::lock_guard lock(g_nvapi_mutex);
        g_nvapi_display_bindings.clear();
        g_nvapi_color_observations.clear();
    }
    g_nvapi_state_revision.store(0, std::memory_order_release);

    if (retired_session && !retired_session->before_resize()) {
        // Shutdown cannot make an unfenced GPU submission safe. The DLL is
        // pinned and hooks are pass-through, so retaining the session until
        // process teardown is the least invasive option.
        (void)retired_session.release();
    }

    std::vector<QueueBinding> retired_bindings;
    {
        std::lock_guard lock(g_registry_mutex);
        retired_bindings.swap(g_queue_bindings);
    }
    // Hooks remain resident and become pass-through via g_stopping.  Do not
    // call hooks::deinit(); it could sever a later-installed detour chain.
}

} // namespace cte::overlay::present
