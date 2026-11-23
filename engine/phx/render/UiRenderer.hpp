#pragma once
// GPU side of the UI library (phx/ui): draws phx::ui::Vertex quads (4 vertices per quad,
// pixel coordinates, top-left origin, colours already display-encoded) with src-alpha
// blending into a colour attachment — normally the swapchain image after tonemapping.
// One host-visible vertex buffer per frame in flight.
#include "phx/gpu/Resources.hpp"
#include "phx/ui/Ui.hpp"

#include <span>
#include <vector>

namespace phx {

class UiRenderer {
public:
    UiRenderer(VulkanContext& ctx, Bindless& bindless, VkFormat colorFormat, u32 framesInFlight, u32 maxQuads = 65536);
    ~UiRenderer();
    UiRenderer(const UiRenderer&) = delete;
    UiRenderer& operator=(const UiRenderer&) = delete;

    // Replaces this slot's quads (excess quads beyond maxQuads are dropped).
    void upload(u32 slot, std::span<const ui::Vertex> vertices);
    // Inside a dynamic-rendering pass on a target of `colorFormat`; viewport set by the caller.
    void record(VkCommandBuffer cmd, u32 slot, VkExtent2D target) const;

    [[nodiscard]] u32 quadCount(u32 slot) const { return quadCounts_[slot]; }
    [[nodiscard]] u32 maxQuads() const { return maxQuads_; }
    [[nodiscard]] VkFormat colorFormat() const { return format_; }

private:
    VulkanContext& ctx_;
    Bindless& bindless_;
    VkFormat format_;
    u32 maxQuads_;
    VkPipeline pipe_ = VK_NULL_HANDLE;
    std::vector<Buffer> buffers_;
    std::vector<u32> quadCounts_;
};

} // namespace phx
