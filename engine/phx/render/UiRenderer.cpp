#include "phx/render/UiRenderer.hpp"

#include "phx/gpu/VulkanContext.hpp"

#include <algorithm>
#include <cstring>

namespace phx {

UiRenderer::UiRenderer(VulkanContext& ctx, Bindless& bindless, VkFormat colorFormat, u32 framesInFlight, u32 maxQuads)
    : ctx_(ctx), bindless_(bindless), format_(colorFormat), maxQuads_(maxQuads) {
    GraphicsPipelineDesc d{"phx/ui.vert", "phx/ui.frag", colorFormat};
    d.alphaBlend = true;
    pipe_ = createGraphicsPipeline(ctx, bindless, d);
    for (u32 i = 0; i < framesInFlight; ++i)
        buffers_.push_back(createBuffer(ctx, VkDeviceSize(maxQuads) * 4 * sizeof(ui::Vertex), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        MemoryUsage::HostUpload));
    quadCounts_.assign(framesInFlight, 0);
}

UiRenderer::~UiRenderer() {
    vkDestroyPipeline(ctx_.device, pipe_, nullptr);
    for (Buffer& b : buffers_) destroyBuffer(ctx_, b);
}

void UiRenderer::upload(u32 slot, std::span<const ui::Vertex> vertices) {
    const std::size_t quads = std::min<std::size_t>(vertices.size() / 4, maxQuads_);
    quadCounts_[slot] = u32(quads);
    if (!quads) return;
    std::memcpy(buffers_[slot].mapped, vertices.data(), quads * 4 * sizeof(ui::Vertex));
    vmaFlushAllocation(ctx_.allocator, buffers_[slot].allocation, 0, VK_WHOLE_SIZE);
}

void UiRenderer::record(VkCommandBuffer cmd, u32 slot, VkExtent2D target) const {
    if (!quadCounts_[slot]) return;
    struct {
        u64 vertices;
        u32 width, height;
    } pc{buffers_[slot].address, target.width, target.height};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bindless_.pipelineLayout, 0, 1, &bindless_.set, 0, nullptr);
    vkCmdPushConstants(cmd, bindless_.pipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, quadCounts_[slot] * 6, 1, 0, 0);
}

} // namespace phx
