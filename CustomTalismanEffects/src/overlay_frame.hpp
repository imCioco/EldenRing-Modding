#pragma once

#include <cstdint>
#include <vector>

#include <imgui.h>

namespace cte::overlay::frame {

// Draw packets intentionally support only the frontend's current font atlas.
// This keeps renderer-owned GPU objects out of the UI/producer thread and
// prevents arbitrary ImTextureID values from crossing DLL/thread boundaries.
enum class texture_kind : std::uint8_t {
    font_atlas,
};

enum class draw_command_kind : std::uint8_t {
    draw,
    reset_render_state,
};

struct draw_command {
    draw_command_kind kind{draw_command_kind::draw};
    texture_kind texture{texture_kind::font_atlas};
    ImVec4 clip_rect{};
    std::uint32_t elem_count{};

    // Absolute offsets into frame_packet::indices and ::vertices. They no
    // longer depend on an ImDrawList and are ready for DrawIndexed calls.
    std::uint32_t idx_offset{};
    std::uint32_t vtx_offset{};
};

// The producer finishes every vector before publication. Present sees only an
// immutable, pinned snapshot, so it needs neither an ImGui context nor access
// to live ImDrawList data.
struct frame_packet {
    std::uint64_t generation{};
    std::uint64_t font_generation{};

    ImVec2 display_pos{};
    ImVec2 display_size{};
    ImVec2 framebuffer_scale{1.0f, 1.0f};

    std::vector<ImDrawVert> vertices;
    std::vector<ImDrawIdx> indices;
    std::vector<draw_command> commands;

    // Diagnostics for content deliberately omitted from this packet.
    std::uint32_t skipped_user_callbacks{};
    std::uint32_t skipped_unsupported_textures{};
};

struct font_atlas_packet {
    std::uint64_t generation{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba32;
};

// Move-only snapshot pins. Acquiring, moving, and releasing these objects use
// only a bounded number of lock-free integer atomic operations. In particular,
// their destruction cannot free a packet or run a vector destructor on the
// Present thread; slot reclamation belongs to the serialized producer side.
class frame_view final {
public:
    frame_view() noexcept = default;
    ~frame_view() noexcept;

    frame_view(frame_view&& other) noexcept;
    frame_view& operator=(frame_view&& other) noexcept;

    frame_view(const frame_view&) = delete;
    frame_view& operator=(const frame_view&) = delete;

    [[nodiscard]] const frame_packet* get() const noexcept { return packet_; }
    [[nodiscard]] const frame_packet& operator*() const noexcept { return *packet_; }
    [[nodiscard]] const frame_packet* operator->() const noexcept { return packet_; }
    [[nodiscard]] explicit operator bool() const noexcept { return packet_ != nullptr; }

private:
    friend frame_view acquire_frame() noexcept;

    frame_view(const frame_packet* packet, std::uint32_t slot) noexcept
        : packet_(packet), slot_(slot) {}
    void release() noexcept;

    const frame_packet* packet_{};
    std::uint32_t slot_{~std::uint32_t{0}};
};

class font_atlas_view final {
public:
    font_atlas_view() noexcept = default;
    ~font_atlas_view() noexcept;

    font_atlas_view(font_atlas_view&& other) noexcept;
    font_atlas_view& operator=(font_atlas_view&& other) noexcept;

    font_atlas_view(const font_atlas_view&) = delete;
    font_atlas_view& operator=(const font_atlas_view&) = delete;

    [[nodiscard]] const font_atlas_packet* get() const noexcept { return packet_; }
    [[nodiscard]] const font_atlas_packet& operator*() const noexcept { return *packet_; }
    [[nodiscard]] const font_atlas_packet* operator->() const noexcept { return packet_; }
    [[nodiscard]] explicit operator bool() const noexcept { return packet_ != nullptr; }

private:
    friend font_atlas_view acquire_font_atlas() noexcept;

    font_atlas_view(const font_atlas_packet* packet, std::uint32_t slot) noexcept
        : packet_(packet), slot_(slot) {}
    void release() noexcept;

    const font_atlas_packet* packet_{};
    std::uint32_t slot_{~std::uint32_t{0}};
};

enum class publish_status : std::uint8_t {
    published,
    published_with_skips,
    no_data,
    invalid_data,
    missing_font_atlas,
    font_generation_mismatch,
    allocation_failed,
    publication_busy,
};

struct frame_publish_result {
    publish_status status{publish_status::no_data};
    std::uint64_t generation{};
    std::uint32_t skipped_user_callbacks{};
    std::uint32_t skipped_unsupported_textures{};
    bool has_draw_commands{};

    [[nodiscard]] bool was_published() const noexcept {
        return status == publish_status::published ||
               status == publish_status::published_with_skips;
    }
};

struct font_publish_result {
    publish_status status{publish_status::no_data};
    std::uint64_t generation{};

    [[nodiscard]] bool was_published() const noexcept {
        return status == publish_status::published;
    }
};

// Producer-thread API. GetTexDataAsRGBA32() may build the atlas, so the caller
// must own the atlas while this function runs. The returned generation must be
// supplied to publish_draw_data() for frames built with this atlas.
[[nodiscard]] font_publish_result publish_font_atlas(ImFontAtlas* atlas) noexcept;

// Deep-copies live ImGui draw data. Commands using frontend_font_texture are
// mapped to texture_kind::font_atlas. ImDrawCallback_ResetRenderState is kept
// as an explicit command. Other callbacks and texture IDs are never executed
// or dereferenced: their commands are skipped and counted in the result.
[[nodiscard]] frame_publish_result publish_draw_data(
    const ImDrawData* draw_data,
    ImTextureID frontend_font_texture,
    std::uint64_t font_generation) noexcept;

// Bounded lock-free reader API. Acquisition either pins the current immutable
// slot within a fixed number of attempts or returns an empty view. A consumer
// must compare the frame and atlas generations and skip if they differ.
[[nodiscard]] frame_view acquire_frame() noexcept;
[[nodiscard]] font_atlas_view acquire_font_atlas() noexcept;

// Clearing does not reset generation counters, avoiding ABA after reinit.
void clear_frame() noexcept;
void clear_font_atlas() noexcept;
void clear() noexcept;

} // namespace cte::overlay::frame
