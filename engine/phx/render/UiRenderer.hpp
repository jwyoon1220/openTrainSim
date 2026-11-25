#pragma once
// GPU side of the UI library (phx/ui): draws phx::ui::Vertex triangle lists (pixel
// coordinates, top-left origin, colours already display-encoded) with src-alpha blending into
// a colour attachment — normally the swapchain image after tonemapping. Owns the GPU copy of
// the font atlas (ui::fonts()): rows the CPU rasterised since the last frame are copied in
// prepare(). One host-visible vertex buffer and atlas staging buffer per frame in flight.
#include "phx/gpu/Resources.hpp"
#include "phx/ui/Font.hpp"
#include "phx/ui/Ui.hpp"

#include <span>
#include <vector>

namespace phx {

class UiRenderer {
public:
    UiRenderer(VulkanContext& ctx, Bindless& bindless, VkFormat colorFormat, u32 framesInFlight, u32 maxVertices = 393216);
    ~UiRenderer();
    UiRenderer(const UiRenderer&) = delete;
    UiRenderer& operator=(const UiRenderer&) = delete;

    // Replaces this slot's triangles (vertices beyond maxVertices are dropped, whole
    // triangles only) and stages the font atlas rows that changed.
    void upload(u32 slot, std::span<const ui::Vertex> vertices);
    // Outside any rendering pass, before record(): copies the staged atlas rows.
    void prepare(VkCommandBuffer cmd, u32 slot);
    // Inside a dynamic-rendering pass on a target of `colorFormat`; viewport set by the caller.
    void record(VkCommandBuffer cmd, u32 slot, VkExtent2D target) const;

    [[nodiscard]] u32 vertexCount(u32 slot) const { return vertexCounts_[slot]; }
    [[nodiscard]] u32 maxVertices() const { return maxVertices_; }
    [[nodiscard]] VkFormat colorFormat() const { return format_; }
    [[nodiscard]] u32 atlasSlot() const { return atlasSlot_; }

private:
    VulkanContext& ctx_;
    Bindless& bindless_;
    VkFormat format_;
    u32 maxVertices_;
    VkPipeline pipe_ = VK_NULL_HANDLE;
    std::vector<Buffer> buffers_;
    std::vector<u32> vertexCounts_;
    // font atlas
    Image atlas_;
    u32 atlasSlot_ = 0;
    bool atlasReady_ = false; // transitioned out of UNDEFINED
    std::vector<Buffer> staging_;
    std::vector<ui::FontAtlas::Band> pending_; // per slot: rows staged, not yet copied
};

} // namespace phx
