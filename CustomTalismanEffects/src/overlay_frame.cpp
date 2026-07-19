#include "overlay_frame.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

namespace cte::overlay::frame {
namespace {

constexpr std::uint32_t kPublicationSlotCount = 4;
constexpr std::uint32_t kNoPublicationSlot = ~std::uint32_t{0};

static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "Present publication pins must be lock-free");

// Fixed-slot publication keeps all allocation and packet destruction on the
// producer side. A Present reader only pins an index, verifies that the index
// is still current, and later drops the pin. Sequence-consistent ordering is
// intentional here: it makes the stale-index/ABA handshake explicit while the
// operation count remains tiny and fixed.
template <typename Packet>
class publication_ring final {
public:
    bool publish(std::unique_ptr<Packet> packet) noexcept {
        const std::uint32_t current =
            published_.load(std::memory_order_seq_cst);

        for (std::uint32_t slot = 0; slot < kPublicationSlotCount; ++slot) {
            if (slot == current ||
                readers_[slot].load(std::memory_order_seq_cst) != 0)
                continue;

            // A reader that sampled this slot before it stopped being current
            // cannot dereference it until its second published_ load. It will
            // either reject the stale sample or observe this completed write.
            slots_[slot] = std::move(packet);
            published_.store(slot, std::memory_order_seq_cst);
            return true;
        }
        return false;
    }

    const Packet* acquire(std::uint32_t& acquired_slot) noexcept {
        acquired_slot = kNoPublicationSlot;
        for (std::uint32_t attempt = 0; attempt < kPublicationSlotCount;
             ++attempt) {
            const std::uint32_t slot =
                published_.load(std::memory_order_seq_cst);
            if (slot >= kPublicationSlotCount)
                return nullptr;

            readers_[slot].fetch_add(1, std::memory_order_seq_cst);
            if (published_.load(std::memory_order_seq_cst) == slot) {
                const Packet* packet = slots_[slot].get();
                if (packet != nullptr) {
                    acquired_slot = slot;
                    return packet;
                }
            }
            readers_[slot].fetch_sub(1, std::memory_order_seq_cst);
        }
        return nullptr;
    }

    void release(std::uint32_t slot) noexcept {
        if (slot < kPublicationSlotCount)
            readers_[slot].fetch_sub(1, std::memory_order_seq_cst);
    }

    void clear() noexcept {
        published_.store(kNoPublicationSlot, std::memory_order_seq_cst);
        for (std::uint32_t slot = 0; slot < kPublicationSlotCount; ++slot) {
            if (readers_[slot].load(std::memory_order_seq_cst) == 0)
                slots_[slot].reset();
        }
    }

private:
    std::array<std::unique_ptr<Packet>, kPublicationSlotCount> slots_{};
    std::array<std::atomic<std::uint32_t>, kPublicationSlotCount> readers_{};
    std::atomic<std::uint32_t> published_{kNoPublicationSlot};
};

publication_ring<frame_packet> g_frames;
publication_ring<font_atlas_packet> g_fonts;

// Publication is deliberately serialized. Readers never take this lock. It
// guarantees that generation order and atomic-store order cannot be inverted
// even if a future frontend publishes from more than one thread.
std::mutex g_publish_mutex;
std::uint64_t g_next_frame_generation = 1;
std::uint64_t g_next_font_generation = 1;
std::uint64_t g_published_font_generation = 0;

bool finite(const ImVec2& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

bool finite(const ImVec4& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}

bool checked_add(std::size_t lhs, std::size_t rhs, std::size_t& result) noexcept {
    if (rhs > std::numeric_limits<std::size_t>::max() - lhs)
        return false;
    result = lhs + rhs;
    return true;
}

bool fits_flat_offset(std::size_t value) noexcept {
    return value <= std::numeric_limits<std::uint32_t>::max();
}

std::uint64_t take_generation(std::uint64_t& next) noexcept {
    // Generation zero is reserved for "not published". Wrapping is only
    // theoretical, but skipping zero keeps that invariant exact.
    const std::uint64_t result = next++;
    if (next == 0)
        next = 1;
    return result == 0 ? next++ : result;
}

bool validate_draw_data_header(const ImDrawData& data) noexcept {
    if (!data.Valid || data.CmdListsCount < 0 || data.TotalIdxCount < 0 ||
        data.TotalVtxCount < 0)
        return false;
    if (data.CmdListsCount != data.CmdLists.Size ||
        (data.CmdListsCount > 0 && data.CmdLists.Data == nullptr))
        return false;
    if (!finite(data.DisplayPos) || !finite(data.DisplaySize) ||
        !finite(data.FramebufferScale))
        return false;
    if (data.DisplaySize.x < 0.0f || data.DisplaySize.y < 0.0f ||
        data.FramebufferScale.x <= 0.0f || data.FramebufferScale.y <= 0.0f)
        return false;
    return true;
}

} // namespace

font_publish_result publish_font_atlas(ImFontAtlas* atlas) noexcept {
    if (atlas == nullptr)
        return {publish_status::no_data, 0};

    std::lock_guard lock(g_publish_mutex);
    try {
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        int bytes_per_pixel = 0;
        atlas->GetTexDataAsRGBA32(&pixels, &width, &height, &bytes_per_pixel);

        if (pixels == nullptr || width <= 0 || height <= 0 ||
            bytes_per_pixel != 4)
            return {publish_status::invalid_data, 0};

        const auto unsigned_width = static_cast<std::size_t>(width);
        const auto unsigned_height = static_cast<std::size_t>(height);
        if (unsigned_width > std::numeric_limits<std::uint32_t>::max() ||
            unsigned_height > std::numeric_limits<std::uint32_t>::max())
            return {publish_status::invalid_data, 0};

        if (unsigned_width >
            std::numeric_limits<std::size_t>::max() / unsigned_height)
            return {publish_status::invalid_data, 0};
        const std::size_t pixel_count = unsigned_width * unsigned_height;
        if (pixel_count > std::numeric_limits<std::size_t>::max() / 4)
            return {publish_status::invalid_data, 0};
        const std::size_t byte_count = pixel_count * 4;

        auto packet = std::make_unique<font_atlas_packet>();
        packet->generation = take_generation(g_next_font_generation);
        packet->width = static_cast<std::uint32_t>(width);
        packet->height = static_cast<std::uint32_t>(height);
        packet->rgba32.assign(pixels, pixels + byte_count);

        const std::uint64_t generation = packet->generation;
        if (!g_fonts.publish(std::move(packet)))
            return {publish_status::publication_busy, 0};
        g_published_font_generation = generation;
        return {publish_status::published, generation};
    } catch (const std::bad_alloc&) {
        return {publish_status::allocation_failed, 0};
    } catch (...) {
        // Never allow frontend/allocator exceptions to cross an injected DLL
        // boundary. The previously published immutable atlas remains valid.
        return {publish_status::invalid_data, 0};
    }
}

frame_publish_result publish_draw_data(const ImDrawData* draw_data,
                                       ImTextureID frontend_font_texture,
                                       std::uint64_t font_generation) noexcept {
    if (draw_data == nullptr)
        return {publish_status::no_data, 0, 0, 0};

    std::lock_guard lock(g_publish_mutex);
    if (!validate_draw_data_header(*draw_data))
        return {publish_status::invalid_data, 0, 0, 0};

    if (g_published_font_generation == 0)
        return {publish_status::missing_font_atlas, 0, 0, 0};
    if (font_generation == 0 ||
        g_published_font_generation != font_generation)
        return {publish_status::font_generation_mismatch, 0, 0, 0};

    try {
        std::size_t total_vertices = 0;
        std::size_t total_indices = 0;
        std::size_t total_commands = 0;

        for (int list_index = 0; list_index < draw_data->CmdListsCount; ++list_index) {
            const ImDrawList* list = draw_data->CmdLists[list_index];
            if (list == nullptr || list->VtxBuffer.Size < 0 ||
                list->IdxBuffer.Size < 0 || list->CmdBuffer.Size < 0)
                return {publish_status::invalid_data, 0, 0, 0};
            if ((list->VtxBuffer.Size > 0 && list->VtxBuffer.Data == nullptr) ||
                (list->IdxBuffer.Size > 0 && list->IdxBuffer.Data == nullptr) ||
                (list->CmdBuffer.Size > 0 && list->CmdBuffer.Data == nullptr))
                return {publish_status::invalid_data, 0, 0, 0};

            if (!checked_add(total_vertices,
                             static_cast<std::size_t>(list->VtxBuffer.Size),
                             total_vertices) ||
                !checked_add(total_indices,
                             static_cast<std::size_t>(list->IdxBuffer.Size),
                             total_indices) ||
                !checked_add(total_commands,
                             static_cast<std::size_t>(list->CmdBuffer.Size),
                             total_commands) ||
                !fits_flat_offset(total_vertices) || !fits_flat_offset(total_indices))
                return {publish_status::invalid_data, 0, 0, 0};
        }

        if (total_vertices != static_cast<std::size_t>(draw_data->TotalVtxCount) ||
            total_indices != static_cast<std::size_t>(draw_data->TotalIdxCount))
            return {publish_status::invalid_data, 0, 0, 0};

        auto packet = std::make_unique<frame_packet>();
        packet->font_generation = font_generation;
        packet->display_pos = draw_data->DisplayPos;
        packet->display_size = draw_data->DisplaySize;
        packet->framebuffer_scale = draw_data->FramebufferScale;
        packet->vertices.reserve(total_vertices);
        packet->indices.reserve(total_indices);
        packet->commands.reserve(total_commands);

        std::size_t global_vtx_base = 0;
        std::size_t global_idx_base = 0;
        bool has_draw_commands = false;
        for (int list_index = 0; list_index < draw_data->CmdListsCount; ++list_index) {
            const ImDrawList& list = *draw_data->CmdLists[list_index];

            if (list.VtxBuffer.Size > 0) {
                packet->vertices.insert(packet->vertices.end(), list.VtxBuffer.Data,
                                        list.VtxBuffer.Data + list.VtxBuffer.Size);
            }
            if (list.IdxBuffer.Size > 0) {
                packet->indices.insert(packet->indices.end(), list.IdxBuffer.Data,
                                       list.IdxBuffer.Data + list.IdxBuffer.Size);
            }

            for (int command_index = 0; command_index < list.CmdBuffer.Size;
                 ++command_index) {
                const ImDrawCmd& source = list.CmdBuffer[command_index];

                if (source.UserCallback != nullptr) {
                    if (source.UserCallback == ImDrawCallback_ResetRenderState) {
                        draw_command command{};
                        command.kind = draw_command_kind::reset_render_state;
                        command.clip_rect = source.ClipRect;
                        packet->commands.push_back(command);
                    } else {
                        ++packet->skipped_user_callbacks;
                    }
                    continue;
                }

                if (source.GetTexID() != frontend_font_texture) {
                    ++packet->skipped_unsupported_textures;
                    continue;
                }
                if (!finite(source.ClipRect))
                    return {publish_status::invalid_data, 0, 0, 0};
                if (source.ElemCount == 0)
                    continue;

                const std::size_t local_idx_offset = source.IdxOffset;
                const std::size_t local_vtx_offset = source.VtxOffset;
                const std::size_t elem_count = source.ElemCount;
                const std::size_t list_idx_count =
                    static_cast<std::size_t>(list.IdxBuffer.Size);
                const std::size_t list_vtx_count =
                    static_cast<std::size_t>(list.VtxBuffer.Size);

                if (local_idx_offset > list_idx_count ||
                    elem_count > list_idx_count - local_idx_offset ||
                    local_vtx_offset > list_vtx_count)
                    return {publish_status::invalid_data, 0, 0, 0};

                // Validate the eventual BaseVertexLocation + index accesses so
                // malformed packets cannot make a Present renderer read past
                // the uploaded vertex buffer.
                for (std::size_t element = 0; element < elem_count; ++element) {
                    const std::size_t vertex_index = static_cast<std::size_t>(
                        list.IdxBuffer.Data[local_idx_offset + element]);
                    if (vertex_index > list_vtx_count - local_vtx_offset ||
                        vertex_index + local_vtx_offset >= list_vtx_count)
                        return {publish_status::invalid_data, 0, 0, 0};
                }

                const std::size_t flat_idx_offset = global_idx_base + local_idx_offset;
                const std::size_t flat_vtx_offset = global_vtx_base + local_vtx_offset;
                if (!fits_flat_offset(flat_idx_offset) ||
                    !fits_flat_offset(flat_vtx_offset))
                    return {publish_status::invalid_data, 0, 0, 0};

                draw_command command{};
                command.kind = draw_command_kind::draw;
                command.texture = texture_kind::font_atlas;
                command.clip_rect = source.ClipRect;
                command.elem_count = source.ElemCount;
                command.idx_offset = static_cast<std::uint32_t>(flat_idx_offset);
                command.vtx_offset = static_cast<std::uint32_t>(flat_vtx_offset);
                packet->commands.push_back(command);
                has_draw_commands = true;
            }

            global_vtx_base += static_cast<std::size_t>(list.VtxBuffer.Size);
            global_idx_base += static_cast<std::size_t>(list.IdxBuffer.Size);
        }

        packet->generation = take_generation(g_next_frame_generation);
        const std::uint64_t generation = packet->generation;
        const std::uint32_t skipped_callbacks = packet->skipped_user_callbacks;
        const std::uint32_t skipped_textures = packet->skipped_unsupported_textures;
        const publish_status status =
            (skipped_callbacks != 0 || skipped_textures != 0)
                ? publish_status::published_with_skips
                : publish_status::published;

        if (!g_frames.publish(std::move(packet)))
            return {publish_status::publication_busy, 0, skipped_callbacks,
                    skipped_textures};
        return {status, generation, skipped_callbacks, skipped_textures,
                has_draw_commands};
    } catch (const std::bad_alloc&) {
        return {publish_status::allocation_failed, 0, 0, 0};
    } catch (...) {
        // Preserve the last known-good frame on any producer-side failure.
        return {publish_status::invalid_data, 0, 0, 0};
    }
}

frame_view::~frame_view() noexcept { release(); }

frame_view::frame_view(frame_view&& other) noexcept
    : packet_(std::exchange(other.packet_, nullptr)),
      slot_(std::exchange(other.slot_, kNoPublicationSlot)) {}

frame_view& frame_view::operator=(frame_view&& other) noexcept {
    if (this != &other) {
        release();
        packet_ = std::exchange(other.packet_, nullptr);
        slot_ = std::exchange(other.slot_, kNoPublicationSlot);
    }
    return *this;
}

void frame_view::release() noexcept {
    if (packet_ != nullptr)
        g_frames.release(slot_);
    packet_ = nullptr;
    slot_ = kNoPublicationSlot;
}

font_atlas_view::~font_atlas_view() noexcept { release(); }

font_atlas_view::font_atlas_view(font_atlas_view&& other) noexcept
    : packet_(std::exchange(other.packet_, nullptr)),
      slot_(std::exchange(other.slot_, kNoPublicationSlot)) {}

font_atlas_view& font_atlas_view::operator=(font_atlas_view&& other) noexcept {
    if (this != &other) {
        release();
        packet_ = std::exchange(other.packet_, nullptr);
        slot_ = std::exchange(other.slot_, kNoPublicationSlot);
    }
    return *this;
}

void font_atlas_view::release() noexcept {
    if (packet_ != nullptr)
        g_fonts.release(slot_);
    packet_ = nullptr;
    slot_ = kNoPublicationSlot;
}

frame_view acquire_frame() noexcept {
    std::uint32_t slot = kNoPublicationSlot;
    const frame_packet* packet = g_frames.acquire(slot);
    return frame_view(packet, slot);
}

font_atlas_view acquire_font_atlas() noexcept {
    std::uint32_t slot = kNoPublicationSlot;
    const font_atlas_packet* packet = g_fonts.acquire(slot);
    return font_atlas_view(packet, slot);
}

void clear_frame() noexcept {
    std::lock_guard lock(g_publish_mutex);
    g_frames.clear();
}

void clear_font_atlas() noexcept {
    std::lock_guard lock(g_publish_mutex);
    // A frame is unusable without its referenced atlas. Clearing both in this
    // order makes readers fail closed on a generation/null check.
    g_frames.clear();
    g_fonts.clear();
    g_published_font_generation = 0;
}

void clear() noexcept {
    std::lock_guard lock(g_publish_mutex);
    g_frames.clear();
    g_fonts.clear();
    g_published_font_generation = 0;
}

} // namespace cte::overlay::frame
