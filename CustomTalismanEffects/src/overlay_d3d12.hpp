#pragma once

#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>

namespace cte::overlay::frame {
struct frame_packet;
struct font_atlas_packet;
}

namespace cte::overlay::d3d12 {

// Compile and validate the tiny renderer shaders on the bootstrap worker, not
// on the game's first visible Present.
bool prepare_shaders();

enum class ColorMode : uint8_t {
    Sdr,
    ScRgb,
    Hdr10,
    Unknown = 0xff,
};

enum class ColorEvidence : uint8_t {
    Dxgi,
    NvidiaNvapi,
};

enum class RenderResult : uint8_t {
    Submitted,
    Skipped,
    RecoverableFailure,
    DeviceLost,
};

// One session belongs to one canonical swapchain/device/queue tuple.  It owns
// only device children; it never owns, resizes, presents, or changes the color
// state of the game's swapchain.
class Session final {
public:
    Session();
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool initialize(IDXGISwapChain3* swapchain, ID3D12CommandQueue* queue,
                    ColorMode color_mode, ColorEvidence color_evidence);
    bool matches(IDXGISwapChain3* swapchain, ID3D12CommandQueue* queue,
                 ColorMode color_mode) const;

    RenderResult render(
        IDXGISwapChain3* swapchain,
        const frame::frame_packet* packet,
        const frame::font_atlas_packet* font,
        ColorMode color_mode);

    // Called before forwarding ResizeBuffers/ResizeBuffers1.  A true result
    // means every CustomTalismanEffects submission is complete and all backbuffer-derived
    // references were released.  A false result deliberately retains them;
    // freeing GPU-live D3D12 objects would be worse than letting resize fail.
    bool before_resize();
    bool gpu_idle() const;

    HRESULT device_removed_reason() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cte::overlay::d3d12
