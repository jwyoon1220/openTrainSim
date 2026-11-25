#include "phx/render/UiRenderer.hpp"

#include "phx/gpu/VulkanContext.hpp"
#include "phx/ui/Font.hpp"

#include <algorithm>
#include <cstring>

namespace phx {

namespace {
constexpr u32 kAtlas = ui::FontAtlas::kSize;

void atlasBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage,
                  VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}
} // namespace

UiRenderer::UiRenderer(VulkanContext& ctx, Bindless& bindless, VkFormat colorFormat, u32 framesInFlight, u32 maxVertices)
    : ctx_(ctx), bindless_(bindless), format_(colorFormat), maxVertices_(maxVertices - maxVertices % 3) {
    GraphicsPipelineDesc d{"phx/ui.vert", "phx/ui.frag", colorFormat};
    d.alphaBlend = true;
    pipe_ = createGraphicsPipeline(ctx, bindless, d);
    for (u32 i = 0; i < framesInFlight; ++i) {
        buffers_.push_back(createBuffer(ctx, VkDeviceSize(maxVertices_) * sizeof(ui::Vertex), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        MemoryUsage::HostUpload));
        staging_.push_back(createBuffer(ctx, VkDeviceSize(kAtlas) * kAtlas, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::HostUpload));
    }
    vertexCounts_.assign(framesInFlight, 0);
    pending_.assign(framesInFlight, {});
    atlas_ = createImage2D(ctx, {kAtlas, kAtlas}, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    atlasSlot_ = bindless.allocateSampled();
    bindless.setSampled(atlasSlot_, atlas_.view); // linear clamp
}

UiRenderer::~UiRenderer() {
    vkDestroyPipeline(ctx_.device, pipe_, nullptr);
    for (Buffer& b : buffers_) destroyBuffer(ctx_, b);
    for (Buffer& b : staging_) destroyBuffer(ctx_, b);
    bindless_.freeSampled(atlasSlot_);
    destroyImage(ctx_, atlas_);
}

void UiRenderer::upload(u32 slot, std::span<const ui::Vertex> vertices) {
    const std::size_t count = std::min<std::size_t>(vertices.size() - vertices.size() % 3, maxVertices_);
    vertexCounts_[slot] = u32(count);
    if (count) {
        std::memcpy(buffers_[slot].mapped, vertices.data(), count * sizeof(ui::Vertex));
        vmaFlushAllocation(ctx_.allocator, buffers_[slot].allocation, 0, VK_WHOLE_SIZE);
    }
    // atlas rows written since the last upload (the whole atlas the first time)
    ui::FontAtlas& fonts = ui::fonts();
    ui::FontAtlas::Band band = fonts.takeDirty();
    if (!atlasReady_) band = {0, kAtlas};
    if (band.empty()) return;
    // copied by prepare() of this same frame; merged in case a frame uploaded without preparing
    ui::FontAtlas::Band& p = pending_[slot];
    p = p.empty() ? band : ui::FontAtlas::Band{std::min(p.y0, band.y0), std::max(p.y1, band.y1)};
    const std::size_t offset = std::size_t(p.y0) * kAtlas;
    std::memcpy(static_cast<u8*>(staging_[slot].mapped) + offset, fonts.pixels() + offset, std::size_t(p.y1 - p.y0) * kAtlas);
    vmaFlushAllocation(ctx_.allocator, staging_[slot].allocation, offset, std::size_t(p.y1 - p.y0) * kAtlas);
}

void UiRenderer::prepare(VkCommandBuffer cmd, u32 slot) {
    ui::FontAtlas::Band& p = pending_[slot];
    if (p.empty()) return;
    // earlier frames on this queue may still sample the atlas: their reads come first
    atlasBarrier(cmd, atlas_.image, atlasReady_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.bufferOffset = VkDeviceSize(p.y0) * kAtlas;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0, i32(p.y0), 0};
    region.imageExtent = {kAtlas, p.y1 - p.y0, 1};
    vkCmdCopyBufferToImage(cmd, staging_[slot].buffer, atlas_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    atlasBarrier(cmd, atlas_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    atlasReady_ = true;
    p = {};
}

void UiRenderer::record(VkCommandBuffer cmd, u32 slot, VkExtent2D target) const {
    if (!vertexCounts_[slot] || !atlasReady_) return;
    struct {
        u64 vertices;
        u32 width, height;
        u32 atlas;
    } pc{buffers_[slot].address, target.width, target.height, atlasSlot_};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bindless_.pipelineLayout, 0, 1, &bindless_.set, 0, nullptr);
    vkCmdPushConstants(cmd, bindless_.pipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, vertexCounts_[slot], 1, 0, 0);
}

} // namespace phx
