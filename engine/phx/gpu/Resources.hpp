#pragma once
// GPU resource helpers: VMA-backed buffers/images, the global bindless descriptor
// set (plan §2.4), SPIR-V pipelines and one-shot uploads.
#include "phx/core/Types.hpp"
#include "phx/gpu/Vk.hpp"

#include <span>
#include <string>
#include <vector>

namespace phx {
class VulkanContext;

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    void* mapped = nullptr; // host-visible buffers only
    VkDeviceSize size = 0;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
};

enum class MemoryUsage { DeviceLocal, HostUpload /* mapped, sequential write */, HostReadback /* mapped, cached */ };

Buffer createBuffer(VulkanContext& ctx, VkDeviceSize size, VkBufferUsageFlags usage, MemoryUsage mem);
void destroyBuffer(VulkanContext& ctx, Buffer& b);
Image createImage2D(VulkanContext& ctx, VkExtent2D extent, VkFormat format, VkImageUsageFlags usage);
void destroyImage(VulkanContext& ctx, Image& img);

// Blocking one-shot submission on the graphics queue (startup uploads only; streaming
// goes through the transfer queue in M4).
class ImmediateSubmitter {
public:
    explicit ImmediateSubmitter(VulkanContext& ctx);
    ~ImmediateSubmitter();
    template <class F> void run(F&& record) {
        begin();
        record(cmd_);
        end();
    }
    void uploadBuffer(const Buffer& dst, const void* data, VkDeviceSize size);

private:
    void begin();
    void end();
    VulkanContext& ctx_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
};

// One descriptor set for the whole frame: arrays of sampled and storage images,
// indexed from shaders. Buffers are reached through buffer device addresses.
class Bindless {
public:
    // Sampled slots: [0, 1000) and [1024, kMaxSampled) belong to the game; [1000, 1024) are the
    // engine's post-processing slots (PostSlots.hpp). allocateSampled() hands out game slots.
    static constexpr u32 kMaxSampled = 16384;
    static constexpr u32 kMaxStorage = 64;
    static constexpr u32 kMaxStorageR32 = 32; // binding 2: r32f storage images (depth pyramids)
    static constexpr u32 kPushConstantBytes = 128;

    explicit Bindless(VulkanContext& ctx);
    ~Bindless();

    // Caller guarantees the GPU is not using the slot being rewritten.
    void setSampled(u32 index, VkImageView view, VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // Repeat-addressed, 16× anisotropic: material textures.
    void setMaterial(u32 index, VkImageView view);
    void setStorage(u32 index, VkImageView view);
    void setStorageR32(u32 index, VkImageView view);

    // Game sampled-slot allocator (skips the engine range). A freed slot is reused only after
    // `retireFrames` further calls to retireFrame(), so a pending frame never sees it rewritten.
    [[nodiscard]] u32 allocateSampled();
    void freeSampled(u32 index);
    void retireFrame(u32 retireFrames = 3);
    [[nodiscard]] u32 sampledInUse() const { return sampledInUse_; }
    // Same as setMaterial() but with clamp-to-edge addressing (UI / panel textures).
    void setMaterialClamped(u32 index, VkImageView view);

    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkSampler linearClamp = VK_NULL_HANDLE;
    VkSampler anisoRepeat = VK_NULL_HANDLE;

private:
    VulkanContext& ctx_;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::vector<u32> freeSampled_;
    std::vector<std::pair<u32, u32>> retiring_; // slot, frames left
    u32 nextSampled_ = 1; // slot 0 is left for the game's fallback texture
    u32 sampledInUse_ = 0;
    VkSampler anisoClamp_ = VK_NULL_HANDLE;
};

// SPIR-V lookup: <shader directory>/<name>.spv. Engine shaders are named "phx/<file>"; the
// default directory is the build's (PHX_SHADER_DIR), shared by the engine and the game.
void setShaderDirectory(std::string dir);
[[nodiscard]] const std::string& shaderDirectory();
std::vector<u32> loadSpirv(const std::string& name);
VkPipeline createComputePipeline(VulkanContext& ctx, const Bindless& bl, const std::string& shader);

enum class BlendMode : u8 { Opaque, Alpha, Additive, Premultiplied };

struct GraphicsPipelineDesc {
    std::string vertex;
    std::string fragment;
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    bool additiveBlend = false;
    bool alphaBlend = false; // premultiplied-free src-alpha blending (overlays)
    VkFormat colorFormat2 = VK_FORMAT_UNDEFINED; // optional second colour attachment (impostor bake)
    VkFormat depthFormat = VK_FORMAT_UNDEFINED; // UNDEFINED = no depth attachment
    VkCompareOp depthCompare = VK_COMPARE_OP_ALWAYS;
    bool depthTest = false;
    bool depthWrite = false;
    VkCullModeFlags cullMode = VK_CULL_MODE_NONE;
    f32 depthBiasConstant = 0.0f; // shadow maps
    f32 depthBiasSlope = 0.0f;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT; // MSAA
    bool alphaToCoverage = false;
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    BlendMode blend = BlendMode::Opaque; // used when neither additiveBlend nor alphaBlend is set
    VkColorComponentFlags colorWriteMask2 = 0xF; // second attachment (e.g. 0 = keep motion vectors)
};
VkPipeline createGraphicsPipeline(VulkanContext& ctx, const Bindless& bl, const GraphicsPipelineDesc& desc);

// Synchronization2 image barrier, whole colour image.
void imageBarrier(VkCommandBuffer cmd, VkImage image, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess, VkImageLayout from, VkImageLayout to);

} // namespace phx
