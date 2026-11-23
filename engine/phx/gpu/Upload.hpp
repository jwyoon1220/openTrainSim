#pragma once
// Streaming GPU uploads without stalls:
//  * UploadQueue — one persistently mapped staging arena per frame in flight. Copies are
//    recorded into the frame's command buffer (record()) and the arena is reused once that
//    frame's fence has been waited on (beginFrame()). A per-frame byte budget spreads large
//    loads over several frames instead of hitching.
//  * GpuHeap — one large device-local buffer sub-allocated with a VMA virtual block (vertex /
//    index pools): no per-mesh VkBuffer, no per-mesh allocation, frees deferred until no frame
//    in flight can still read the range.
#include "phx/gpu/BlockCompress.hpp"
#include "phx/gpu/Resources.hpp"

#include <deque>
#include <vector>

namespace phx {

class UploadQueue {
public:
    UploadQueue(VulkanContext& ctx, u32 framesInFlight, VkDeviceSize bytesPerFrame);
    ~UploadQueue();
    UploadQueue(const UploadQueue&) = delete;
    UploadQueue& operator=(const UploadQueue&) = delete;

    // The slot's previous submission has completed: its staging memory can be reused.
    // Anything queued but not yet record()ed is discarded, so queue uploads only between
    // beginFrame() and record() of the same frame.
    void beginFrame(u32 slot);
    // false = no room left in this frame's budget (retry next frame). An item larger than the
    // whole budget is accepted when it is the first of the frame (dedicated staging buffer).
    bool uploadBuffer(VkBuffer dst, VkDeviceSize dstOffset, const void* data, VkDeviceSize size);
    bool uploadImage(VkImage dst, const tex::PreparedTexture& texture);
    [[nodiscard]] bool canFit(VkDeviceSize size) const;
    // Records all copies queued for the current slot (outside a render pass) and the barriers
    // that make them visible to vertex/index fetch and fragment sampling.
    void record(VkCommandBuffer cmd);
    [[nodiscard]] VkDeviceSize bytesThisFrame() const { return frames_[slot_].used; }

private:
    struct BufferCopy {
        VkBuffer src, dst;
        VkBufferCopy region;
    };
    struct ImageCopy {
        VkBuffer src;
        VkImage dst;
        u32 mipCount;
        std::vector<VkBufferImageCopy> regions;
    };
    struct Frame {
        Buffer staging;
        VkDeviceSize used = 0;
        std::vector<Buffer> oversized;
        std::vector<BufferCopy> buffers;
        std::vector<ImageCopy> images;
    };
    bool reserve(VkDeviceSize size, VkDeviceSize align, VkBuffer& buffer, VkDeviceSize& offset, u8*& ptr);
    VulkanContext& ctx_;
    std::vector<Frame> frames_;
    VkDeviceSize capacity_;
    u32 slot_ = 0;
};

// Sizes and offsets are in elements of `elementSize` bytes, so e.g. 24-byte vertices pack
// without padding and an offset is directly a vertexOffset / firstIndex.
class GpuHeap {
public:
    GpuHeap(VulkanContext& ctx, VkDeviceSize elementCount, VkDeviceSize elementSize, VkBufferUsageFlags usage);
    ~GpuHeap();
    GpuHeap(const GpuHeap&) = delete;
    GpuHeap& operator=(const GpuHeap&) = delete;

    struct Range {
        VmaVirtualAllocation handle = VK_NULL_HANDLE;
        VkDeviceSize offset = 0, size = 0;
        [[nodiscard]] bool valid() const { return handle != VK_NULL_HANDLE; }
    };
    // false when the heap is full (caller evicts and retries).
    bool allocate(VkDeviceSize count, Range& out);
    // Released after `delayFrames` calls to retireFrame().
    void free(Range& range, u32 delayFrames = 3);
    void retireFrame();

    [[nodiscard]] const Buffer& buffer() const { return buffer_; }
    [[nodiscard]] VkDeviceSize capacity() const { return capacity_; }      // elements
    [[nodiscard]] VkDeviceSize used() const { return used_; }              // elements
    [[nodiscard]] VkDeviceSize elementSize() const { return elementSize_; }

private:
    VulkanContext& ctx_;
    Buffer buffer_;
    VmaVirtualBlock block_ = VK_NULL_HANDLE;
    VkDeviceSize used_ = 0, capacity_ = 0, elementSize_ = 1;
    struct Pending {
        Range range;
        u32 frames;
    };
    std::vector<Pending> pending_;
};

// Creates a sampled image with a mip chain matching `texture` (format, extent, levels).
Image createTextureImage(VulkanContext& ctx, const tex::PreparedTexture& texture);
// Bytes a texture occupies on the GPU (approximation: its staging size).
inline VkDeviceSize textureBytes(const tex::PreparedTexture& t) { return t.data.size(); }

} // namespace phx
