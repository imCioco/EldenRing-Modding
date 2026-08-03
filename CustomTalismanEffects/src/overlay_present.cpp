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
#include "overlay_coexist.hpp"
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
                          uint32_t evidence_depth,
                          bool replaces_ambiguous_evidence = false) noexcept {
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
                // A later authoritative source (the ResizeBuffers1 queue
                // array, DXGI queue retrieval, or an exact pointer embedded in
                // the live swapchain) resolves an ambiguity introduced by a
                // nested creation proxy. Ordinary creation observations remain
                // conservative and can only add ambiguity.
                binding.proxy_ambiguous = replaces_ambiguous_evidence
                    ? false
                    : binding.proxy_ambiguous || proxy_ambiguous;
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
        pending.proxy_ambiguous =
            replaces_ambiguous_evidence ? false : proxy_ambiguous;
        g_queue_bindings.push_back(std::move(pending));
        if (proxy_ambiguous && !replaces_ambiguous_evidence) {
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
                             evidence_depth, true);
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
// Diagnostics for the otherwise-silent shadow-to-primary path.  All are
// written only under g_render_mutex.
uint64_t g_present_reached_logged_generation = 0;
uint64_t g_describe_reject_logged_generation = 0;
const char* g_describe_reject_reason = nullptr;
const char* g_primary_block_reason = nullptr;
uint64_t g_queue_missing_logged_generation = 0;
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

void record_nvapi_color_observation(uint32_t display_id, int32_t hdr_mode,
                                    const char* observed_via) {
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

    flog("[overlay-v2] observed NvAPI HDR state via %s "
         "(mode=%s, display_id=%u, output=%s)",
         observed_via, nvapi_hdr_mode_name(hdr_mode), display_id,
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

// Every rejection here is a silent per-Present no-op, which makes a chain that
// validates on one loader and not another impossible to diagnose from a log.
// `reason` names the failing check so the caller can report it once.
bool describe_swapchain(IDXGISwapChain* base,
                        ComPtr<IDXGISwapChain3>& swapchain3,
                        ComPtr<ID3D12Device>& device,
                        uint64_t& generation,
                        void*& device_id,
                        HWND& hwnd,
                        DXGI_SWAP_CHAIN_DESC1& desc,
                        const char*& reason) noexcept {
    reason = "renderer_ineligible";
    if (!base || !dxgi_shadow::renderer_eligible(base) ||
        FAILED(base->QueryInterface(IID_PPV_ARGS(&swapchain3))) || !swapchain3)
        return false;
    reason = "no_d3d12_device"; // rejects D3D11 and foreign helper swapchains
    if (FAILED(base->GetDevice(IID_PPV_ARGS(&device))) || !device)
        return false;

    reason = "getdesc1_failed";
    if (FAILED(swapchain3->GetDesc1(&desc)))
        return false;
    reason = "hwnd_not_a_valid_game_window"; // composition/CoreWindow/hidden
    if (FAILED(swapchain3->GetHwnd(&hwnd)) || !valid_game_window(hwnd))
        return false;

    reason = "buffer_count_or_sample_count";
    if (desc.BufferCount < 2 || desc.BufferCount > 8 || desc.SampleDesc.Count != 1)
        return false;
    reason = "unsupported_format";
    if (!supported_backbuffer_format(desc.Format)) return false;
    reason = "not_a_render_target";
    if ((desc.BufferUsage & DXGI_USAGE_RENDER_TARGET_OUTPUT) == 0) return false;
    constexpr UINT kUnsupportedFlags =
        DXGI_SWAP_CHAIN_FLAG_RESTRICTED_CONTENT |
        DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY |
        DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED;
    reason = "restricted_or_protected_flags";
    if ((desc.Flags & kUnsupportedFlags) != 0) return false;
    reason = "not_a_flip_model_swapchain";
    if (desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD &&
        desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
        return false;

    generation = dxgi_shadow::generation_token(base);
    device_id = com_identity(device.Get());
    reason = "no_generation_or_device_identity";
    if (!generation || !device_id) return false;

    ComPtr<ID3D12Resource> backbuffer;
    reason = "getbuffer_failed";
    if (FAILED(swapchain3->GetBuffer(swapchain3->GetCurrentBackBufferIndex(),
                                     IID_PPV_ARGS(&backbuffer))) || !backbuffer)
        return false;
    const D3D12_RESOURCE_DESC resource_desc = backbuffer->GetDesc();
    reason = "backbuffer_shape_mismatch";
    if (resource_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        resource_desc.DepthOrArraySize != 1 || resource_desc.MipLevels != 1 ||
        resource_desc.SampleDesc.Count != 1 ||
        resource_desc.Format != desc.Format ||
        (resource_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0 ||
        resource_desc.Width > UINT_MAX)
        return false;
    if (desc.Width == 0) desc.Width = static_cast<UINT>(resource_desc.Width);
    if (desc.Height == 0) desc.Height = resource_desc.Height;
    reason = "backbuffer_size_mismatch";
    if (resource_desc.Width != desc.Width || resource_desc.Height != desc.Height)
        return false;
    // Tiny video/probe/helper chains are never the Elden Ring presentation
    // surface.  Requiring a plausible game canvas makes initial selection less
    // dependent on whichever injected module happens to Present first.
    reason = "canvas_below_640x360";
    if (desc.Width < 640 || desc.Height < 360) return false;
    reason = nullptr;
    return true;
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
        const HWND foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
        if (observed->consecutive_presents < 8 || foreground != hwnd) {
            // Both gates are otherwise invisible and retried forever, so a
            // chain that presents fine but never gets selected looks exactly
            // like one that never presents at all.  Report the blocking
            // reason once, and again only if it changes.
            const char* blocker = observed->consecutive_presents < 8
                                      ? "fewer than 8 sustained presents"
                                      : "the game window is not foreground";
            if (g_primary_block_reason != blocker) {
                g_primary_block_reason = blocker;
                flog("[overlay-v2] primary selection blocked: %s "
                     "(presents=%u, swapchain hwnd=%p, foreground root=%p)",
                     blocker, observed->consecutive_presents, hwnd, foreground);
            }
            return false;
        }
        g_primary_block_reason = nullptr;
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

    // The single most valuable line in the backend log: it splits "our vtable
    // shadow is never invoked" (line absent) from "our validation rejects the
    // chain" (line present, followed by a reject reason).
    if (g_present_reached_logged_generation != call_generation) {
        g_present_reached_logged_generation = call_generation;
        flog("[overlay-v2] shadowed Present reached CustomTalismanEffects for generation %llu",
             static_cast<unsigned long long>(call_generation));
    }

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

    const char* describe_reason = nullptr;
    if (!describe_swapchain(base, swapchain3, device, generation, device_id,
                            hwnd, desc, describe_reason)) {
        if (g_describe_reject_logged_generation != call_generation ||
            g_describe_reject_reason != describe_reason) {
            g_describe_reject_logged_generation = call_generation;
            g_describe_reject_reason = describe_reason;
            flog("[overlay-v2] swapchain validation rejected generation %llu: %s",
                 static_cast<unsigned long long>(call_generation),
                 describe_reason ? describe_reason : "unknown");
        }
        return;
    }
    if (g_describe_reject_reason) {
        g_describe_reject_reason = nullptr;
        flog("[overlay-v2] swapchain validation now passes for generation %llu",
             static_cast<unsigned long long>(call_generation));
    }

    // Queue identity is part of swapchain identity for D3D12.  Do not publish
    // a usable canvas (and therefore do not capture input) until the exact
    // queue supplied to CreateSwapChain* or ResizeBuffers1 is known.
    queue = queue_for_swapchain(generation, device_id);
    if (!queue) {
        if (g_queue_missing_logged_generation != generation) {
            g_queue_missing_logged_generation = generation;
            flog("[overlay-v2] no exact queue bound for generation %llu; "
                 "canvas stays unavailable",
                 static_cast<unsigned long long>(generation));
        }
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

// ---- late-adoption state (see the adoption section further down) ----
//
// A loader can inject CustomTalismanEffects after the game already created its swapchain
// (Elden Mod Loader delays, manual injectors). The creation detours then never
// run, and without this fallback the canvas could never validate. Adoption
// watches live Present calls through pass-through observer hooks and installs
// the same per-instance shadow on the already-existing swapchain.

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ExecuteCommandListsFn =
    void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT,
                             ID3D12CommandList* const*);

PresentFn g_next_present_observed = nullptr;
Present1Fn g_next_present1_observed = nullptr;
ExecuteCommandListsFn g_next_execute_command_lists = nullptr;

enum class AdoptionState : uint32_t {
    Idle = 0,     // resident observers; collect only until creation succeeds
    Pending,      // observers active, waiting for a candidate + queue evidence
    Adopted,      // a live swapchain was shadowed and bound
    Failed,       // permanently ambiguous or unprovable; stay pass-through
};
std::atomic<uint32_t> g_adoption_state{
    static_cast<uint32_t>(AdoptionState::Idle)};
bool g_adoption_hooks_installed = false; // overlay/control thread only
bool g_adoption_hook_install_failed = false;

// The probe swapchain used to resolve vtable code pointers must never be
// shadowed, bound, or counted as a game creation.
thread_local bool g_adoption_probe_in_progress = false;

// Forensics: how often the creation detours actually ran, and where the
// installed patches point today. Read by log_creation_hook_forensics().
std::atomic<uint32_t> g_creation_detour_calls{0};

// Set only when a creation detour observed a game swapchain AND shadowed and
// queue-bound it.  This is the evidence the discovery watchdog needs: without
// it, a visible game window is mistaken for a late load.
std::atomic<bool> g_observed_creation{false};

void* g_forensics_create_swapchain_target = nullptr;
void* g_forensics_create_swapchain_for_hwnd_target = nullptr;

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
            record_nvapi_color_observation(display_id, hdr_mode,
                                           "the game's successful SET");
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
    if (!g_adoption_probe_in_progress)
        g_creation_detour_calls.fetch_add(1, std::memory_order_relaxed);
    BindingScope binding_scope(g_creation_transaction);
    const HRESULT result =
        g_next_create_swapchain(factory, queue, desc, result_swapchain);
    if (!g_adoption_probe_in_progress &&
        !g_stopping.load(std::memory_order_acquire) && SUCCEEDED(result) &&
        result_swapchain && *result_swapchain &&
        eligible_shadow_candidate(*result_swapchain) &&
        install_swapchain_shadow(*result_swapchain) &&
        bind_swapchain_queue(*result_swapchain, queue,
                             binding_scope.id(), binding_scope.depth())) {
        g_observed_creation.store(true, std::memory_order_release);
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
    if (!g_adoption_probe_in_progress)
        g_creation_detour_calls.fetch_add(1, std::memory_order_relaxed);
    BindingScope binding_scope(g_creation_transaction);
    const HRESULT result = g_next_create_swapchain_for_hwnd(
        factory, queue, hwnd, desc, fullscreen_desc, restrict_to_output,
        result_swapchain);
    if (!g_adoption_probe_in_progress &&
        !g_stopping.load(std::memory_order_acquire) && SUCCEEDED(result) &&
        result_swapchain && *result_swapchain &&
        eligible_shadow_candidate(*result_swapchain) &&
        install_swapchain_shadow(*result_swapchain) &&
        bind_swapchain_queue(*result_swapchain, queue,
                             binding_scope.id(), binding_scope.depth())) {
        g_observed_creation.store(true, std::memory_order_release);
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
};

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

// -------------------------------------------------------------------------
// Late adoption of an already-existing swapchain
//
// The creation detours are the authoritative discovery and queue-association
// path, but they can only ever observe creations that happen after they are
// installed. When CustomTalismanEffects is loaded late the game's swapchain (and its
// direct command queue) already exist. Adoption recovers from that with the
// backend's evidence rules intact:
//
//  * The swapchain INSTANCE is found by pass-through observer hooks on the
//    shared IDXGISwapChain::Present/Present1 implementations. They perform no
//    rendering and are never removed once installed (an unknown multi-mod
//    chain must not lose an edge); after adoption resolves they are a single
//    atomic load and a tail call.
//  * The exact queue is first requested from DXGI itself
//    (GetDevice(ID3D12CommandQueue)); measured on Windows 11 23H2 this
//    returns E_NOINTERFACE, but the call is exact if any DXGI build honors
//    it, so it stays as the first choice.
//  * Next, the hudhook-compatible layout probe accepts an observed direct
//    queue only when that exact interface pointer is uniquely embedded in the
//    live swapchain object. This remains authoritative with multiple queues.
//  * Otherwise an ELDEN-RING-SPECIFIC correlation adapter may promote the
//    device's single active direct queue: presentation requires a direct
//    queue on the swapchain's device, so when exactly one direct queue has
//    ever submitted on that device, the association queue can only be that
//    queue or a queue that never executes -- and a presenting game renders
//    on its association queue every frame. Any second direct queue, or a
//    same-thread submit disagreeing with the candidate, permanently fails
//    adoption closed. Generic (non-Elden-Ring) embeddings of this backend
//    never promote correlation evidence and simply stay pass-through.
//
// Color for an adopted chain cannot use the creation-default rule (the game
// may have called SetColorSpace1 before CustomTalismanEffects loaded), so arming issues
// one direct NVAPI GET to seed the existing vendor color observations. On
// systems without NVAPI, R10/FP16 adopted chains keep failing closed exactly
// like a detected wrapper does today.

constexpr uint32_t kAdoptionMinimumQueueCalls = 128;
constexpr uint32_t kAdoptionMinimumPresents = 32;
constexpr uint32_t kAdoptionMaximumInstallAttempts = 64;

struct AdoptionQueueEvidence {
    // queue_identity doubles as the slot-published flag: it is stored with
    // release order only after every other field is complete.
    std::atomic<void*> queue_identity{nullptr};
    void* device_identity = nullptr;
    ID3D12CommandQueue* queue = nullptr; // AddRef'd; retained for process life
    std::atomic<uint32_t> calls{0};
};
// Injected renderers commonly create their own direct queues during bootstrap.
// Keep enough evidence slots to reach the game's queue even in a busy overlay
// stack; the array is fixed and each entry is retained for process life.
constexpr size_t kMaximumAdoptionQueues = 32;
AdoptionQueueEvidence g_adoption_queues[kMaximumAdoptionQueues];
std::atomic<uint32_t> g_adoption_queue_count{0};
std::atomic<bool> g_adoption_queue_overflow{false};
SRWLOCK g_adoption_queue_insert_lock = SRWLOCK_INIT;

// Per-thread submit correlation. The cached interface pointer avoids a
// QueryInterface per ExecuteCommandLists call on the hot pending path.
thread_local ID3D12CommandQueue* g_tls_ecl_cached_pointer = nullptr;
thread_local AdoptionQueueEvidence* g_tls_ecl_cached_slot = nullptr;
thread_local void* g_tls_last_direct_queue_identity = nullptr;

AdoptionState adoption_state() noexcept {
    return static_cast<AdoptionState>(
        g_adoption_state.load(std::memory_order_acquire));
}

void set_adoption_state(AdoptionState state) noexcept {
    g_adoption_state.store(static_cast<uint32_t>(state),
                           std::memory_order_release);
}

AdoptionQueueEvidence* record_adoption_queue(
    ID3D12CommandQueue* queue) noexcept {
    void* const identity = com_identity(queue);
    if (!identity) return nullptr;
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return nullptr;

    const uint32_t published =
        g_adoption_queue_count.load(std::memory_order_acquire);
    for (uint32_t index = 0; index < published; ++index) {
        if (g_adoption_queues[index].queue_identity.load(
                std::memory_order_acquire) == identity)
            return &g_adoption_queues[index];
    }

    AcquireSRWLockExclusive(&g_adoption_queue_insert_lock);
    AdoptionQueueEvidence* result = nullptr;
    const uint32_t count =
        g_adoption_queue_count.load(std::memory_order_acquire);
    for (uint32_t index = 0; index < count && !result; ++index) {
        if (g_adoption_queues[index].queue_identity.load(
                std::memory_order_acquire) == identity)
            result = &g_adoption_queues[index];
    }
    if (!result) {
        if (count >= kMaximumAdoptionQueues) {
            g_adoption_queue_overflow.store(true, std::memory_order_release);
        } else {
            ComPtr<ID3D12Device> device;
            if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && device) {
                void* const device_identity = com_identity(device.Get());
                if (device_identity) {
                    AdoptionQueueEvidence& slot = g_adoption_queues[count];
                    slot.device_identity = device_identity;
                    slot.queue = queue;
                    queue->AddRef();
                    slot.queue_identity.store(identity,
                                              std::memory_order_release);
                    g_adoption_queue_count.store(count + 1,
                                                 std::memory_order_release);
                    result = &slot;
                }
            }
        }
    }
    ReleaseSRWLockExclusive(&g_adoption_queue_insert_lock);
    return result;
}

// hudhook's DX12 fallback does one thing CustomTalismanEffects's original late-adoption
// adapter did not: it verifies a candidate queue by finding that exact
// interface pointer in the live swapchain object's private storage. This is
// stronger than choosing the first/last queue observed globally and remains
// useful when a wrapper creates multiple direct queues on the same device.
//
// The DXGI layout is private, so the scan is deliberately evidence-only: it
// reads at most the same 512 pointer slots as hudhook, never dereferences a
// discovered value, and accepts a result only when exactly one already
// validated direct queue for this D3D12 device is present. Any unreadable page
// or multiple embedded queues fails closed into the existing correlation path.
constexpr size_t kEmbeddedQueueProbeSlots = 512;

bool readable_queue_probe_protection(DWORD protection) noexcept {
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

size_t readable_queue_probe_slots(const void* object) noexcept {
    if (!object) return 0;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(object);
    const uintptr_t requested_end =
        begin + kEmbeddedQueueProbeSlots * sizeof(void*);
    if (requested_end < begin) return 0;

    uintptr_t cursor = begin;
    while (cursor < requested_end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info,
                         sizeof(info)) != sizeof(info) ||
            info.State != MEM_COMMIT ||
            !readable_queue_probe_protection(info.Protect))
            break;

        const uintptr_t region_begin =
            reinterpret_cast<uintptr_t>(info.BaseAddress);
        const uintptr_t region_end = region_begin + info.RegionSize;
        if (region_end <= cursor || region_end < region_begin) break;
        cursor = std::min(region_end, requested_end);
    }
    return static_cast<size_t>((cursor - begin) / sizeof(void*));
}

struct EmbeddedQueueProbe {
    AdoptionQueueEvidence* match = nullptr;
    size_t byte_offset = 0;
    size_t readable_slots = 0;
    bool ambiguous = false;
};

EmbeddedQueueProbe probe_embedded_swapchain_queue(
    IDXGISwapChain* swapchain, void* device_identity) noexcept {
    EmbeddedQueueProbe result{};
    if (!swapchain || !device_identity) return result;
    result.readable_slots = readable_queue_probe_slots(swapchain);
    if (result.readable_slots == 0) return result;

    const uint32_t published =
        g_adoption_queue_count.load(std::memory_order_acquire);
    void* const* const fields = reinterpret_cast<void* const*>(swapchain);
    __try {
        for (size_t field = 0; field < result.readable_slots; ++field) {
            void* const value = fields[field];
            for (uint32_t index = 0; index < published; ++index) {
                AdoptionQueueEvidence& candidate = g_adoption_queues[index];
                if (candidate.device_identity != device_identity ||
                    !candidate.queue || candidate.queue != value ||
                    !candidate.queue_identity.load(std::memory_order_acquire))
                    continue;

                if (!result.match) {
                    result.match = &candidate;
                    result.byte_offset = field * sizeof(void*);
                } else if (result.match != &candidate) {
                    result.ambiguous = true;
                    return result;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // The private object can disappear only under invalid concurrent host
        // lifetime behavior. Treat the probe as absent and preserve Present.
        result = {};
    }
    return result;
}

void STDMETHODCALLTYPE execute_command_lists_observer(
    ID3D12CommandQueue* queue, UINT count,
    ID3D12CommandList* const* lists) {
    const AdoptionState state = adoption_state();
    const bool collect_for_possible_adoption =
        !g_stopping.load(std::memory_order_acquire) &&
        (state == AdoptionState::Pending ||
         (state == AdoptionState::Idle &&
          !g_observed_creation.load(std::memory_order_acquire)));
    if (collect_for_possible_adoption && queue) {
        if (queue != g_tls_ecl_cached_pointer) {
            g_tls_ecl_cached_pointer = queue;
            g_tls_ecl_cached_slot = record_adoption_queue(queue);
        }
        if (AdoptionQueueEvidence* slot = g_tls_ecl_cached_slot) {
            slot->calls.fetch_add(1, std::memory_order_relaxed);
            g_tls_last_direct_queue_identity =
                slot->queue_identity.load(std::memory_order_relaxed);
        }
    }
    g_next_execute_command_lists(queue, count, lists);
}

// Serializes candidate tracking and the adopt transaction. Contention is
// effectively zero (one game render thread); a losing Present skips a tick.
SRWLOCK g_adoption_try_lock = SRWLOCK_INIT;
struct AdoptionCandidate {
    IDXGISwapChain* instance = nullptr; // continuity key; only dereferenced
                                        // while it is the current caller
    uint32_t consecutive_presents = 0;
    uint64_t last_seen_ms = 0;
    uint32_t install_attempts = 0;
    bool seen_logged = false;
    bool waiting_logged = false;
} g_adoption_candidate;

void adoption_failed_locked(const char* reason) noexcept {
    set_adoption_state(AdoptionState::Failed);
    flog("[overlay-v2] [WARN] swapchain adoption disabled: %s", reason);
}

void try_adopt_swapchain(IDXGISwapChain* swapchain) noexcept {
    if (!TryAcquireSRWLockExclusive(&g_adoption_try_lock)) return;
    try {
        do {
            if (adoption_state() != AdoptionState::Pending ||
                g_stopping.load(std::memory_order_acquire))
                break;
            // A creation wrapper can let CustomTalismanEffects shadow the returned object
            // while preventing an authoritative queue binding (for example,
            // nested proxy layers that expose different queues). The previous
            // adoption path rejected every already-shadowed object, making
            // that otherwise recoverable state permanent. Keep eligible
            // shadows and repair only their missing/ambiguous queue evidence.
            const uint64_t existing_generation =
                dxgi_shadow::generation_token(swapchain);
            if (existing_generation != 0 &&
                !dxgi_shadow::renderer_eligible(swapchain))
                break;
            if (!eligible_shadow_candidate(swapchain)) break;

            AdoptionCandidate& candidate = g_adoption_candidate;
            const uint64_t now = now_ticks();
            if (candidate.instance == swapchain &&
                now - candidate.last_seen_ms <= 500) {
                candidate.consecutive_presents =
                    std::min(candidate.consecutive_presents + 1, 1000u);
            } else {
                candidate.instance = swapchain;
                candidate.consecutive_presents = 1;
                candidate.install_attempts = 0;
            }
            candidate.last_seen_ms = now;
            if (!candidate.seen_logged && candidate.consecutive_presents >= 8) {
                candidate.seen_logged = true;
                flog("[overlay-v2] adoption: found a live game swapchain; "
                     "gathering exact-queue evidence");
            }
            if (candidate.consecutive_presents < kAdoptionMinimumPresents)
                break;

            // First choice: exact retrieval from DXGI. E_NOINTERFACE on every
            // Windows build measured so far, but exact if it ever succeeds.
            ComPtr<ID3D12CommandQueue> queue;
            if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&queue))))
                queue.Reset();
            const char* queue_evidence =
                "DXGI GetDevice(ID3D12CommandQueue)";
            if (!queue) {
                if (g_adoption_queue_overflow.load(std::memory_order_acquire)) {
                    adoption_failed_locked(
                        "too many distinct direct command queues to reason "
                        "about");
                    break;
                }
                ComPtr<ID3D12Device> device;
                if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&device))) ||
                    !device)
                    break;
                void* const device_identity = com_identity(device.Get());
                if (!device_identity) break;

                // Match hudhook's strongest late-injection evidence before
                // applying CustomTalismanEffects's game-specific single-queue rule. This
                // remains exact even when another overlay/proxy has introduced
                // multiple direct queues on the same device.
                const EmbeddedQueueProbe embedded =
                    probe_embedded_swapchain_queue(swapchain,
                                                   device_identity);
                if (embedded.ambiguous) {
                    adoption_failed_locked(
                        "multiple observed command queues are embedded in the "
                        "live swapchain");
                    break;
                }
                if (embedded.match) {
                    queue = embedded.match->queue;
                    queue_evidence =
                        "exact queue pointer embedded in live swapchain";
                    flog("[overlay-v2] adoption: matched the presentation "
                         "queue at swapchain offset +0x%zX (%zu readable "
                         "pointer slots)",
                         embedded.byte_offset, embedded.readable_slots);
                }

                if (!queue && !is_elden_ring_process()) {
                    adoption_failed_locked(
                        "DXGI does not expose the creation queue, no observed "
                        "queue pointer is embedded in the swapchain, and the "
                        "single-queue correlation adapter is Elden Ring "
                        "specific");
                    break;
                }

                AdoptionQueueEvidence* match = nullptr;
                bool ambiguous = false;
                if (!queue) {
                    const uint32_t published =
                        g_adoption_queue_count.load(std::memory_order_acquire);
                    for (uint32_t index = 0; index < published; ++index) {
                        AdoptionQueueEvidence& slot = g_adoption_queues[index];
                        if (!slot.queue_identity.load(
                                std::memory_order_acquire) ||
                            slot.device_identity != device_identity)
                            continue;
                        if (match) {
                            ambiguous = true;
                            break;
                        }
                        match = &slot;
                    }
                }
                if (ambiguous) {
                    adoption_failed_locked(
                        "multiple direct command queues are active on the "
                        "game device; CustomTalismanEffects cannot prove which one "
                        "presents");
                    break;
                }
                if (!queue &&
                    (!match || match->calls.load(std::memory_order_relaxed) <
                                   kAdoptionMinimumQueueCalls)) {
                    if (!candidate.waiting_logged) {
                        candidate.waiting_logged = true;
                        flog("[overlay-v2] adoption: waiting for sustained "
                             "single-queue submit evidence");
                    }
                    break;
                }
                // The presenting thread's own last direct submit must not
                // contradict the single-queue conclusion.
                if (!queue && g_tls_last_direct_queue_identity &&
                    g_tls_last_direct_queue_identity !=
                        match->queue_identity.load(std::memory_order_acquire)) {
                    adoption_failed_locked(
                        "the presenting thread submits on a different direct "
                        "queue than the only observed one");
                    break;
                }
                if (!queue) {
                    queue = match->queue;
                    queue_evidence =
                        "single active direct queue, Elden Ring adapter";
                }
            }
            if (!queue) break;

            BindingScope binding_scope(g_creation_transaction);
            if (existing_generation == 0 &&
                !install_swapchain_shadow(swapchain)) {
                if (++candidate.install_attempts >=
                    kAdoptionMaximumInstallAttempts)
                    adoption_failed_locked(
                        "the live swapchain repeatedly refused a vtable "
                        "shadow");
                break;
            }
            if (!bind_swapchain_queue(swapchain, queue.Get(),
                                       binding_scope.id(),
                                       binding_scope.depth(), true)) {
                adoption_failed_locked(
                    "the recovered queue failed exact-identity validation");
                break;
            }
            set_adoption_state(AdoptionState::Adopted);
            flog("[overlay-v2] adopted the live game swapchain (queue "
                  "evidence: %s)",
                  queue_evidence);
        } while (false);
    } catch (...) {
        // Adoption is strictly optional; never let it disturb the host call.
    }
    ReleaseSRWLockExclusive(&g_adoption_try_lock);
}

HRESULT STDMETHODCALLTYPE present_observer_detour(IDXGISwapChain* swapchain,
                                                  UINT sync_interval,
                                                  UINT flags) {
    if (adoption_state() == AdoptionState::Pending && swapchain &&
        (flags & DXGI_PRESENT_TEST) == 0)
        try_adopt_swapchain(swapchain);
    return g_next_present_observed(swapchain, sync_interval, flags);
}

HRESULT STDMETHODCALLTYPE present1_observer_detour(
    IDXGISwapChain1* swapchain, UINT sync_interval, UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters) {
    if (adoption_state() == AdoptionState::Pending && swapchain &&
        (flags & DXGI_PRESENT_TEST) == 0)
        try_adopt_swapchain(swapchain);
    return g_next_present1_observed(swapchain, sync_interval, flags,
                                    parameters);
}

// Resolve the shared Present/Present1/ExecuteCommandLists implementations
// from a throwaway device + hidden swapchain. Nothing is ever presented or
// submitted, and everything is released before the observer hooks install, so
// the probe can never be seen (or adopted) by the observers. The window is a
// 64x64 tool window, which additionally fails both valid_creation_window and
// the minimum-canvas gates.
bool resolve_adoption_targets(void** present_target, void** present1_target,
                              void** execute_target) noexcept {
    g_adoption_probe_in_progress = true;
    HWND hwnd = nullptr;
    bool class_registered = false;
    bool ok = false;
    const wchar_t* const kProbeClass =
        coexist::kAdoptionProbeWindowClass;
    do {
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) || !factory)
            break;
        ComPtr<ID3D12Device> device;
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                     IID_PPV_ARGS(&device))) || !device)
            break;
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue;
        if (FAILED(device->CreateCommandQueue(&queue_desc,
                                              IID_PPV_ARGS(&queue))) || !queue)
            break;

        WNDCLASSW window_class{};
        window_class.lpfnWndProc = DefWindowProcW;
        // The class is PRIVATE to this backend copy. Using the DLL HINSTANCE
        // and its QP_BACKEND_MOD_ID-derived name prevents two delayed mods
        // from racing on one hard-coded process-wide class before hook-install
        // arbitration begins.
        window_class.hInstance = g_hinst;
        window_class.lpszClassName = kProbeClass;
        class_registered = RegisterClassW(&window_class) != 0;
        if (!class_registered) {
            flog("[overlay-v2] [ERROR] adoption probe window-class "
                 "registration failed (err %lu)", GetLastError());
            break;
        }
        hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kProbeClass, L"",
                               WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr,
                               window_class.hInstance, nullptr);
        if (!hwnd) break;

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = 64;
        desc.Height = 64;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> probe;
        if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &desc,
                                                   nullptr, nullptr,
                                                   &probe)) || !probe)
            break;

        void** const swapchain_vtable =
            *reinterpret_cast<void***>(probe.Get());
        void** const queue_vtable = *reinterpret_cast<void***>(queue.Get());
        if (!swapchain_vtable || !queue_vtable) break;
        *present_target = swapchain_vtable[8];    // IDXGISwapChain::Present
        *present1_target = swapchain_vtable[22];  // IDXGISwapChain1::Present1
        *execute_target = queue_vtable[10]; // ID3D12CommandQueue::ExecuteCommandLists

        // Field diagnostic: whether this DXGI build supports exact queue
        // retrieval. Windows 11 23H2 measures E_NOINTERFACE.
        ComPtr<ID3D12CommandQueue> retrieved;
        flog("[overlay-v2] adoption probe: swapchain->GetDevice("
             "ID3D12CommandQueue) is %s on this system",
             SUCCEEDED(probe->GetDevice(IID_PPV_ARGS(&retrieved)))
                 ? "supported" : "unavailable");
        ok = *present_target && *present1_target && *execute_target;
    } while (false);
    if (hwnd) DestroyWindow(hwnd);
    if (class_registered)
        UnregisterClassW(kProbeClass, g_hinst);
    g_adoption_probe_in_progress = false;
    return ok;
}

// One direct NVAPI GET so an adopted R10/FP16 chain can prove its current
// encoding; the game's own HDR SETs happened before CustomTalismanEffects loaded. Calls
// nvapi64 directly (never through the game's hooked wrappers). Absence of
// NVAPI (AMD/Intel) is not an error: those chains keep failing closed.
void query_nvapi_current_hdr(HWND game_window) noexcept {
    constexpr uint32_t kNvapiIdInitialize = 0x0150E828u;
    constexpr uint32_t kNvapiIdGetDisplayIdByDisplayName = 0xAE457190u;
    constexpr uint32_t kNvapiIdDispHdrColorControl = 0x351DA224u;
    constexpr uint32_t kHdrColorDataV1 = 0x00010028u;

    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (!nvapi) {
        flog("[overlay-v2] adoption: nvapi64 is not loaded; HDR-capable "
             "swapchain encodings stay unverified");
        return;
    }
    using QueryInterfaceFn = void*(__cdecl*)(uint32_t);
    const auto query_interface = reinterpret_cast<QueryInterfaceFn>(
        GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (!query_interface) return;
    using InitializeFn = int(__cdecl*)();
    using GetDisplayIdFn = int(__cdecl*)(const char*, uint32_t*);
    using HdrColorControlFn = int(__cdecl*)(uint32_t, void*);
    const auto initialize =
        reinterpret_cast<InitializeFn>(query_interface(kNvapiIdInitialize));
    const auto get_display_id = reinterpret_cast<GetDisplayIdFn>(
        query_interface(kNvapiIdGetDisplayIdByDisplayName));
    const auto hdr_color_control = reinterpret_cast<HdrColorControlFn>(
        query_interface(kNvapiIdDispHdrColorControl));
    if (!initialize || !get_display_id || !hdr_color_control) return;
    if (initialize() != 0) return; // refcounted; game holds it initialized

    const auto output_name = monitor_output_name(
        MonitorFromWindow(game_window, MONITOR_DEFAULTTOPRIMARY));
    if (output_name[0] == '\0') return;
    uint32_t display_id = 0;
    if (get_display_id(output_name.data(), &display_id) != 0) {
        flog("[overlay-v2] adoption: NvAPI display lookup failed for %s",
             output_name.data());
        return;
    }
    alignas(8) unsigned char color_data[40] = {};
    auto* header = reinterpret_cast<NvapiHdrColorDataHeader*>(color_data);
    header->version = kHdrColorDataV1;
    header->command = static_cast<int32_t>(NvapiHdrCommand::Get);
    if (hdr_color_control(display_id, header) != 0) {
        flog("[overlay-v2] adoption: NvAPI HDR state query failed; encoding "
             "stays unverified");
        return;
    }
    try {
        record_nvapi_display_binding(output_name, display_id);
        record_nvapi_color_observation(display_id, header->hdr_mode,
                                       "CustomTalismanEffects's direct query");
    } catch (...) {
        // Optional evidence only.
    }
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
    // The whole batch is one create->apply transaction and MUST be serialized
    // against any other mod embedding this backend. An interleaved install
    // erases one side's jmp with every call still returning MH_OK. Resident
    // observers can now recover many such misses, but only the creation call
    // carries API-defined queue evidence, so preserving both hooks remains the
    // preferred and most compatible path. See hooks.hpp for the interleaving.
    hooks::InstallLock install_lock;
    bool ok = true;
    ok &= queue_hook(target.create_swapchain,
                     reinterpret_cast<void*>(&create_swapchain_detour),
                     g_next_create_swapchain, "CreateSwapChain");
    ok &= queue_hook(target.create_swapchain_for_hwnd,
                     reinterpret_cast<void*>(&create_swapchain_for_hwnd_detour),
                     g_next_create_swapchain_for_hwnd, "CreateSwapChainForHwnd");
    g_forensics_create_swapchain_target = target.create_swapchain;
    g_forensics_create_swapchain_for_hwnd_target =
        target.create_swapchain_for_hwnd;

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

bool install_adoption_observer_hooks(void* present_target,
                                     void* present1_target,
                                     void* execute_target) {
    if (g_adoption_hooks_installed) return true;
    if (g_adoption_hook_install_failed) return false;

    // hudhook installs the shared Present and ExecuteCommandLists hooks during
    // bootstrap. Keeping CustomTalismanEffects's equivalents resident from the same early
    // point avoids hot-patching a mature multi-overlay chain ten seconds into
    // gameplay. They are observers only: normal rendering remains on the
    // per-instance vtable shadow, and Idle is a pass-through atomic check.
    hooks::InstallLock install_lock;
    bool ok = true;
    ok &= queue_hook(present_target,
                     reinterpret_cast<void*>(&present_observer_detour),
                     g_next_present_observed, "Present observer");
    ok &= queue_hook(present1_target,
                     reinterpret_cast<void*>(&present1_observer_detour),
                     g_next_present1_observed, "Present1 observer");
    ok &= queue_hook(
        execute_target,
        reinterpret_cast<void*>(&execute_command_lists_observer),
        g_next_execute_command_lists, "ExecuteCommandLists observer");
    const bool applied = hooks::apply();
    if (!ok || !applied) {
        // Some links may already be active. Idle keeps them exact
        // pass-through; never retry an uncertain partial MinHook transaction.
        g_adoption_hook_install_failed = true;
        return false;
    }
    g_adoption_hooks_installed = true;
    return true;
}

// -------------------------------------------------------------------------
// Creation-hook forensics

// SEH-guarded copy in a function with no unwindable objects. The inspected
// addresses are hooked code and normally readable; a peer unmapping its
// module is the case this guards against.
bool safe_read_bytes(const void* source, void* destination,
                     size_t size) noexcept {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// MinHook does not always write a jump straight to the detour.  When the
// target and the detour are more than +-2 GB apart it writes the rel32 jump to
// a VirtualAlloc'd relay page, which then does `jmp qword ptr [rip+0]` to the
// real detour.  That page belongs to no module, so naively reporting the rel32
// destination accuses a peer mod of overwriting a hook that is in fact intact
// -- and which loader placed CustomTalismanEffects.dll near dxgi.dll decides whether it
// happens at all.  Follow the chain a couple of hops before concluding
// anything.
const void* follow_jump_chain(const void* destination) noexcept {
    for (int hop = 0; hop < 4 && destination; ++hop) {
        uint8_t bytes[16]{};
        if (!safe_read_bytes(destination, bytes, sizeof(bytes))) break;
        const auto* code = static_cast<const uint8_t*>(destination);
        if (bytes[0] == 0xE9) {
            int32_t displacement = 0;
            std::memcpy(&displacement, bytes + 1, sizeof(displacement));
            destination = code + 5 + displacement;
            continue;
        }
        if (bytes[0] == 0xFF && bytes[1] == 0x25) {
            int32_t displacement = 0;
            std::memcpy(&displacement, bytes + 2, sizeof(displacement));
            const void* slot = code + 6 + displacement;
            void* next = nullptr;
            if (!safe_read_bytes(slot, &next, sizeof(next))) break;
            destination = next;
            continue;
        }
        break;
    }
    return destination;
}

void log_patch_owner(const char* name, const char* patch_kind,
                     const void* destination) noexcept {
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH]{};
    const wchar_t* base_name = L"unmapped memory";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(destination), &module) &&
        module && GetModuleFileNameW(module, path, MAX_PATH)) {
        base_name = path;
        for (const wchar_t* cursor = path; *cursor != L'\0'; ++cursor) {
            if (*cursor == L'\\' || *cursor == L'/') base_name = cursor + 1;
        }
    }
    flog("[overlay-v2] forensics: %s prologue is a %s into %ls at %p -- "
         "a mod that hooked after CustomTalismanEffects (legitimate if it chains back) "
         "or one that overwrote CustomTalismanEffects's hook",
         name, patch_kind, base_name, destination);
}

void log_creation_target_forensics(const char* name, void* target,
                                   const void* detour) noexcept {
    if (!target) return;
    uint8_t bytes[16]{};
    if (!safe_read_bytes(target, bytes, sizeof(bytes))) {
        flog("[overlay-v2] forensics: %s target %p is unreadable", name,
             target);
        return;
    }
    const auto* code = static_cast<const uint8_t*>(target);
    if (bytes[0] == 0xE9) { // rel32 jmp -- what MinHook writes
        int32_t displacement = 0;
        std::memcpy(&displacement, bytes + 1, sizeof(displacement));
        const void* destination = code + 5 + displacement;
        if (destination != detour && follow_jump_chain(destination) == detour) {
            flog("[overlay-v2] forensics: %s patch is intact (through "
                 "MinHook's relay at %p) -- the chain still enters "
                 "CustomTalismanEffects's detour",
                 name, destination);
            return;
        }
        if (destination == detour) {
            flog("[overlay-v2] forensics: %s patch is intact -- the jump "
                 "still enters CustomTalismanEffects's detour",
                 name);
        } else {
            log_patch_owner(name, "relative jump", destination);
        }
        return;
    }
    if (bytes[0] == 0xFF && bytes[1] == 0x25) { // jmp [rip+disp32]
        int32_t displacement = 0;
        std::memcpy(&displacement, bytes + 2, sizeof(displacement));
        const void* slot = code + 6 + displacement;
        void* destination = nullptr;
        if (safe_read_bytes(slot, &destination, sizeof(destination))) {
            if (destination == detour ||
                follow_jump_chain(destination) == detour)
                flog("[overlay-v2] forensics: %s patch is intact -- the "
                     "indirect jump still enters CustomTalismanEffects's detour",
                     name);
            else
                log_patch_owner(name, "absolute indirect jump", destination);
        } else
            flog("[overlay-v2] forensics: %s prologue is an indirect jump "
                 "through unreadable memory", name);
        return;
    }
    flog("[overlay-v2] forensics: %s prologue bytes %02X %02X %02X %02X "
         "%02X are not a recognized detour patch -- CustomTalismanEffects's hook was "
         "overwritten or removed",
         name, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);
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
    // Resolve the standard shared method addresses before CustomTalismanEffects publishes
    // any creation detour or per-instance shadow. This mirrors hudhook's dummy
    // device/swapchain bootstrap and prevents our own hooks from contaminating
    // the compatibility probe.
    void* present_target = nullptr;
    void* present1_target = nullptr;
    void* execute_target = nullptr;
    const bool observer_targets_resolved = resolve_adoption_targets(
        &present_target, &present1_target, &execute_target);
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

    if (observer_targets_resolved) {
        if (install_adoption_observer_hooks(present_target, present1_target,
                                            execute_target)) {
            flog("[overlay-v2] resident Present/ExecuteCommandLists compatibility observers installed");
        } else {
            flog("[overlay-v2] [WARN] compatibility observer hook transaction failed; creation-path rendering remains active");
        }
    } else {
        // Creation interception is still authoritative. A later arm_adoption()
        // retries the probe in case device creation was only transiently
        // unavailable during bootstrap.
        flog("[overlay-v2] [WARN] compatibility observer probe unavailable at bootstrap; creation-path rendering remains active");
    }

    g_installed.store(true, std::memory_order_release);
    flog("[overlay-v2] DXGI creation hooks installed; rendering uses per-instance Present shadows");

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

bool observed_swapchain_creation() {
    return g_observed_creation.load(std::memory_order_acquire);
}

void log_creation_hook_forensics() {
    if (!g_installed.load(std::memory_order_acquire)) {
        flog("[overlay-v2] forensics: creation hooks were never installed");
        return;
    }
    flog("[overlay-v2] forensics: CustomTalismanEffects's DXGI creation detours have run "
         "%u time(s) since load",
         g_creation_detour_calls.load(std::memory_order_relaxed));
    log_creation_target_forensics(
        "CreateSwapChain", g_forensics_create_swapchain_target,
        reinterpret_cast<const void*>(&create_swapchain_detour));
    log_creation_target_forensics(
        "CreateSwapChainForHwnd", g_forensics_create_swapchain_for_hwnd_target,
        reinterpret_cast<const void*>(&create_swapchain_for_hwnd_detour));
}

void arm_adoption(HWND game_window_hint) {
    // Overlay/control thread only (matches g_adoption_hooks_installed use).
    if (!g_installed.load(std::memory_order_acquire) ||
        !g_renderer_ready.load(std::memory_order_acquire) ||
        g_stopping.load(std::memory_order_acquire))
        return;
    if (adoption_state() == AdoptionState::Pending) return;

    if (g_adoption_hooks_installed) {
        // Arm/re-arm: the observers are resident from bootstrap; only the
        // search state changes. This also covers a replacement swapchain being
        // missed while the creation hook is dead.
        AcquireSRWLockExclusive(&g_adoption_try_lock);
        g_adoption_candidate = {};
        ReleaseSRWLockExclusive(&g_adoption_try_lock);
        set_adoption_state(AdoptionState::Pending);
        query_nvapi_current_hdr(game_window_hint); // HDR may have changed
        flog("[overlay-v2] swapchain adoption armed using resident observers");
        return;
    }
    if (g_adoption_hook_install_failed) {
        set_adoption_state(AdoptionState::Failed);
        flog("[overlay-v2] [ERROR] compatibility observer installation was "
             "previously incomplete; late adoption unavailable");
        return;
    }

    void* present_target = nullptr;
    void* present1_target = nullptr;
    void* execute_target = nullptr;
    if (!resolve_adoption_targets(&present_target, &present1_target,
                                  &execute_target)) {
        set_adoption_state(AdoptionState::Failed);
        flog("[overlay-v2] [ERROR] adoption probe could not resolve the "
             "Present/ExecuteCommandLists implementations; late adoption "
             "unavailable");
        return;
    }
    if (!install_adoption_observer_hooks(
            present_target, present1_target, execute_target)) {
        set_adoption_state(AdoptionState::Failed);
        flog("[overlay-v2] [ERROR] adoption hook transaction failed; "
             "late adoption unavailable");
        return;
    }
    set_adoption_state(AdoptionState::Pending);
    query_nvapi_current_hdr(game_window_hint);
    flog("[overlay-v2] swapchain adoption armed: watching live presents for "
         "the game swapchain");
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
        g_present_reached_logged_generation = 0;
        g_describe_reject_logged_generation = 0;
        g_describe_reject_reason = nullptr;
        g_primary_block_reason = nullptr;
        g_queue_missing_logged_generation = 0;
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
