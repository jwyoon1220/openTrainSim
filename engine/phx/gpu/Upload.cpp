#include "phx/gpu/Upload.hpp"

#include "phx/gpu/VulkanContext.hpp"

#include <algorithm>
#include <cstring>

namespace phx {

UploadQueue::UploadQueue(VulkanContext& ctx, u32 framesInFlight, VkDeviceSize bytesPerFrame)
    : ctx_(ctx), capacity_(bytesPerFrame) {
    frames_.resize(framesInFlight);
    for (Frame& f : frames_)
        f.staging = createBuffer(ctx, bytesPerFrame, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::HostUpload);
}

UploadQueue::~UploadQueue() {
    for (Frame& f : frames_) {
        destroyBuffer(ctx_, f.staging);
        for (Buffer& b : f.oversized) destroyBuffer(ctx_, b);
    }
}

void UploadQueue::beginFrame(u32 slot) {
    slot_ = slot;
    Frame& f = frames_[slot];
    f.used = 0;
    for (Buffer& b : f.oversized) destroyBuffer(ctx_, b);
    f.oversized.clear();
    f.buffers.clear();
    f.images.clear();
}

bool UploadQueue::canFit(VkDeviceSize size) const {
    const Frame& f = frames_[slot_];
    return f.used + size + 16 <= capacity_ || (f.used == 0);
}

bool UploadQueue::reserve(VkDeviceSize size, VkDeviceSize align, VkBuffer& buffer, VkDeviceSize& offset, u8*& ptr) {
    Frame& f = frames_[slot_];
    const VkDeviceSize start = (f.used + align - 1) / align * align;
    if (start + size <= capacity_) {
        f.used = start + size;
        buffer = f.staging.buffer;
        offset = start;
        ptr = static_cast<u8*>(f.staging.mapped) + start;
        return true;
    }
    if (f.used != 0) return false;
    // Larger than a whole frame's budget: give it its own staging buffer, alone in the frame.
    Buffer b = createBuffer(ctx_, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::HostUpload);
    f.oversized.push_back(b);
    f.used = capacity_; // nothing else this frame
    buffer = b.buffer;
    offset = 0;
    ptr = static_cast<u8*>(b.mapped);
    return true;
}

bool UploadQueue::uploadBuffer(VkBuffer dst, VkDeviceSize dstOffset, const void* data, VkDeviceSize size) {
    if (size == 0) return true;
    VkBuffer src;
    VkDeviceSize off;
    u8* ptr;
    if (!reserve(size, 16, src, off, ptr)) return false;
    std::memcpy(ptr, data, size);
    frames_[slot_].buffers.push_back({src, dst, {off, dstOffset, size}});
    return true;
}

bool UploadQueue::uploadImage(VkImage dst, const tex::PreparedTexture& t) {
    VkBuffer src;
    VkDeviceSize off;
    u8* ptr;
    if (!reserve(t.data.size(), 16, src, off, ptr)) return false;
    std::memcpy(ptr, t.data.data(), t.data.size());
    ImageCopy c{src, dst, u32(t.levels.size()), {}};
    for (u32 m = 0; m < t.levels.size(); ++m) {
        VkBufferImageCopy r{};
        r.bufferOffset = off + t.levels[m].offset;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
        r.imageExtent = {t.levels[m].width, t.levels[m].height, 1};
        c.regions.push_back(r);
    }
    frames_[slot_].images.push_back(std::move(c));
    return true;
}

void UploadQueue::record(VkCommandBuffer cmd) {
    Frame& f = frames_[slot_];
    if (f.buffers.empty() && f.images.empty()) return;

    std::vector<VkImageMemoryBarrier2> toDst, toRead;
    for (const ImageCopy& c : f.images) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = c.dst;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, c.mipCount, 0, 1};
        toDst.push_back(b);
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.push_back(b);
    }
    if (!toDst.empty()) {
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = u32(toDst.size());
        dep.pImageMemoryBarriers = toDst.data();
        vkCmdPipelineBarrier2(cmd, &dep);
    }
    for (const BufferCopy& c : f.buffers) vkCmdCopyBuffer(cmd, c.src, c.dst, 1, &c.region);
    for (const ImageCopy& c : f.images)
        vkCmdCopyBufferToImage(cmd, c.src, c.dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, u32(c.regions.size()), c.regions.data());

    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = f.buffers.empty() ? 0u : 1u;
    dep.pMemoryBarriers = &mb;
    dep.imageMemoryBarrierCount = u32(toRead.size());
    dep.pImageMemoryBarriers = toRead.data();
    vkCmdPipelineBarrier2(cmd, &dep);
    f.buffers.clear();
    f.images.clear();
}

// ------------------------------------------------------------------ GpuHeap

GpuHeap::GpuHeap(VulkanContext& ctx, VkDeviceSize elementCount, VkDeviceSize elementSize, VkBufferUsageFlags usage)
    : ctx_(ctx), capacity_(elementCount), elementSize_(elementSize) {
    buffer_ = createBuffer(ctx, elementCount * elementSize, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::DeviceLocal);
    VmaVirtualBlockCreateInfo ci{};
    ci.size = elementCount;
    VK_CHECK(vmaCreateVirtualBlock(&ci, &block_));
}

GpuHeap::~GpuHeap() {
    for (Pending& p : pending_) vmaVirtualFree(block_, p.range.handle);
    vmaClearVirtualBlock(block_);
    vmaDestroyVirtualBlock(block_);
    destroyBuffer(ctx_, buffer_);
}

bool GpuHeap::allocate(VkDeviceSize size, Range& out) {
    VmaVirtualAllocationCreateInfo ai{};
    ai.size = size;
    ai.alignment = 1;
    if (vmaVirtualAllocate(block_, &ai, &out.handle, &out.offset) != VK_SUCCESS) {
        out = {};
        return false;
    }
    out.size = size;
    used_ += size;
    return true;
}

void GpuHeap::free(Range& range, u32 delayFrames) {
    if (!range.valid()) return;
    pending_.push_back({range, delayFrames});
    range = {};
}

void GpuHeap::retireFrame() {
    for (std::size_t i = 0; i < pending_.size();) {
        if (pending_[i].frames-- == 0) {
            vmaVirtualFree(block_, pending_[i].range.handle);
            used_ -= pending_[i].range.size;
            pending_[i] = pending_.back();
            pending_.pop_back();
        } else {
            ++i;
        }
    }
}

Image createTextureImage(VulkanContext& ctx, const tex::PreparedTexture& t) {
    Image img;
    img.format = t.format;
    img.extent = {t.width, t.height};
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = t.format;
    ci.extent = {t.width, t.height, 1};
    ci.mipLevels = u32(t.levels.size());
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_CHECK(vmaCreateImage(ctx.allocator, &ci, &ai, &img.image, &img.allocation, nullptr));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = t.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, u32(t.levels.size()), 0, 1};
    VK_CHECK(vkCreateImageView(ctx.device, &vi, nullptr, &img.view));
    return img;
}

} // namespace phx
