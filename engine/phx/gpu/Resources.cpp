#include "phx/gpu/Resources.hpp"

#include "phx/gpu/VulkanContext.hpp"

#include <cstring>
#include <fstream>
#include <utility>

#ifndef PHX_SHADER_DIR
#define PHX_SHADER_DIR "shaders"
#endif

namespace phx {

Buffer createBuffer(VulkanContext& ctx, VkDeviceSize size, VkBufferUsageFlags usage, MemoryUsage mem) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (mem == MemoryUsage::HostUpload)
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    else if (mem == MemoryUsage::HostReadback)
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    Buffer b;
    VmaAllocationInfo info{};
    VK_CHECK(vmaCreateBuffer(ctx.allocator, &bi, &ai, &b.buffer, &b.allocation, &info));
    b.mapped = info.pMappedData;
    b.size = size;
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = b.buffer;
    b.address = vkGetBufferDeviceAddress(ctx.device, &da);
    return b;
}

void destroyBuffer(VulkanContext& ctx, Buffer& b) {
    if (b.buffer) vmaDestroyBuffer(ctx.allocator, b.buffer, b.allocation);
    b = {};
}

Image createImage2D(VulkanContext& ctx, VkExtent2D extent, VkFormat format, VkImageUsageFlags usage) {
    const bool depth = format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
                       format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D16_UNORM;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {extent.width, extent.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT * (extent.width * extent.height >= 1024 * 1024);
    Image img;
    img.format = format;
    img.extent = extent;
    VK_CHECK(vmaCreateImage(ctx.allocator, &ci, &ai, &img.image, &img.allocation, nullptr));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VkImageAspectFlags(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(ctx.device, &vi, nullptr, &img.view));
    return img;
}

void destroyImage(VulkanContext& ctx, Image& img) {
    if (img.view) vkDestroyImageView(ctx.device, img.view, nullptr);
    if (img.image) vmaDestroyImage(ctx.allocator, img.image, img.allocation);
    img = {};
}

// ------------------------------------------------------------------ immediate submit

ImmediateSubmitter::ImmediateSubmitter(VulkanContext& ctx) : ctx_(ctx) {
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = ctx.graphics.family;
    VK_CHECK(vkCreateCommandPool(ctx.device, &pi, nullptr, &pool_));
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device, &ai, &cmd_));
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(ctx.device, &fi, nullptr, &fence_));
}

ImmediateSubmitter::~ImmediateSubmitter() {
    vkDestroyFence(ctx_.device, fence_, nullptr);
    vkDestroyCommandPool(ctx_.device, pool_, nullptr);
}

void ImmediateSubmitter::begin() {
    VK_CHECK(vkResetCommandBuffer(cmd_, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd_, &bi));
}

void ImmediateSubmitter::end() {
    VK_CHECK(vkEndCommandBuffer(cmd_));
    VkCommandBufferSubmitInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    ci.commandBuffer = cmd_;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &ci;
    VK_CHECK(vkQueueSubmit2(ctx_.graphics.queue, 1, &si, fence_));
    VK_CHECK(vkWaitForFences(ctx_.device, 1, &fence_, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(ctx_.device, 1, &fence_));
}

void ImmediateSubmitter::uploadBuffer(const Buffer& dst, const void* data, VkDeviceSize size) {
    Buffer staging = createBuffer(ctx_, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::HostUpload);
    std::memcpy(staging.mapped, data, size);
    vmaFlushAllocation(ctx_.allocator, staging.allocation, 0, size);
    run([&](VkCommandBuffer cmd) {
        VkBufferCopy region{0, 0, size};
        vkCmdCopyBuffer(cmd, staging.buffer, dst.buffer, 1, &region);
    });
    destroyBuffer(ctx_, staging);
}

// ------------------------------------------------------------------ bindless

Bindless::Bindless(VulkanContext& ctx) : ctx_(ctx) {
    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = kMaxSampled;
    bindings[0].stageFlags = VK_SHADER_STAGE_ALL;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = kMaxStorage;
    bindings[1].stageFlags = VK_SHADER_STAGE_ALL;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = kMaxStorageR32;
    bindings[2].stageFlags = VK_SHADER_STAGE_ALL;

    const VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                           VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorBindingFlags bindingFlags[3] = {flags, flags, flags};
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bf.bindingCount = 3;
    bf.pBindingFlags = bindingFlags;

    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.pNext = &bf;
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    li.bindingCount = 3;
    li.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &layout));

    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxSampled},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxStorage + kMaxStorageR32}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pi.maxSets = 1;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(ctx.device, &pi, nullptr, &pool_));

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VK_CHECK(vkAllocateDescriptorSets(ctx.device, &ai, &set));

    VkPushConstantRange pc{VK_SHADER_STAGE_ALL, 0, kPushConstantBytes};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pc;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &pl, nullptr, &pipelineLayout));

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(ctx.device, &si, nullptr, &linearClamp));
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.anisotropyEnable = VK_TRUE;
    si.maxAnisotropy = 16.0f;
    VK_CHECK(vkCreateSampler(ctx.device, &si, nullptr, &anisoRepeat));
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(ctx.device, &si, nullptr, &anisoClamp_));
}

u32 Bindless::allocateSampled() {
    u32 slot;
    if (!freeSampled_.empty()) {
        slot = freeSampled_.back();
        freeSampled_.pop_back();
    } else {
        if (nextSampled_ == 1000) nextSampled_ = 1024; // engine post-processing range
        if (nextSampled_ >= kMaxSampled) PHX_FATAL("Bindless: out of sampled slots ({})", kMaxSampled);
        slot = nextSampled_++;
    }
    ++sampledInUse_;
    return slot;
}

void Bindless::freeSampled(u32 index) {
    if (index == 0 || index >= kMaxSampled) return;
    retiring_.push_back({index, 0});
    --sampledInUse_;
}

void Bindless::retireFrame(u32 retireFrames) {
    for (std::size_t i = 0; i < retiring_.size();) {
        if (++retiring_[i].second > retireFrames) {
            freeSampled_.push_back(retiring_[i].first);
            retiring_[i] = retiring_.back();
            retiring_.pop_back();
        } else {
            ++i;
        }
    }
}

void Bindless::setMaterialClamped(u32 index, VkImageView view) {
    VkDescriptorImageInfo ii{anisoClamp_, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = 0;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(ctx_.device, 1, &w, 0, nullptr);
}

Bindless::~Bindless() {
    vkDestroySampler(ctx_.device, linearClamp, nullptr);
    vkDestroySampler(ctx_.device, anisoRepeat, nullptr);
    vkDestroySampler(ctx_.device, anisoClamp_, nullptr);
    vkDestroyPipelineLayout(ctx_.device, pipelineLayout, nullptr);
    vkDestroyDescriptorPool(ctx_.device, pool_, nullptr);
    vkDestroyDescriptorSetLayout(ctx_.device, layout, nullptr);
}

void Bindless::setSampled(u32 index, VkImageView view, VkImageLayout imageLayout) {
    VkDescriptorImageInfo ii{linearClamp, view, imageLayout};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = 0;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(ctx_.device, 1, &w, 0, nullptr);
}

void Bindless::setMaterial(u32 index, VkImageView view) {
    VkDescriptorImageInfo ii{anisoRepeat, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = 0;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(ctx_.device, 1, &w, 0, nullptr);
}

void Bindless::setStorageR32(u32 index, VkImageView view) {
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = 2;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(ctx_.device, 1, &w, 0, nullptr);
}

void Bindless::setStorage(u32 index, VkImageView view) {
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = 1;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(ctx_.device, 1, &w, 0, nullptr);
}

// ------------------------------------------------------------------ pipelines

namespace {
std::string& shaderDir() {
    static std::string dir = PHX_SHADER_DIR;
    return dir;
}
} // namespace

void setShaderDirectory(std::string dir) { shaderDir() = std::move(dir); }
const std::string& shaderDirectory() { return shaderDir(); }

std::vector<u32> loadSpirv(const std::string& name) {
    const std::string path = shaderDir() + "/" + name + ".spv";
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) PHX_FATAL("missing shader {}", path);
    const auto size = std::size_t(f.tellg());
    std::vector<u32> words(size / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(words.data()), std::streamsize(size));
    return words;
}

namespace {
VkShaderModule makeModule(VulkanContext& ctx, const std::string& name) {
    const auto code = loadSpirv(name);
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size() * 4;
    ci.pCode = code.data();
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(ctx.device, &ci, nullptr, &m));
    return m;
}
} // namespace

VkPipeline createComputePipeline(VulkanContext& ctx, const Bindless& bl, const std::string& shader) {
    VkShaderModule m = makeModule(ctx, shader);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = bl.pipelineLayout;
    VkPipeline p;
    VK_CHECK(vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &p));
    vkDestroyShaderModule(ctx.device, m, nullptr);
    return p;
}

VkPipeline createGraphicsPipeline(VulkanContext& ctx, const Bindless& bl, const GraphicsPipelineDesc& d) {
    VkShaderModule vs = makeModule(ctx, d.vertex), fs = makeModule(ctx, d.fragment);
    VkPipelineShaderStageCreateInfo stages[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                              {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = d.cullMode;
    rs.frontFace = d.frontFace;
    rs.depthBiasEnable = d.depthBiasConstant != 0.0f || d.depthBiasSlope != 0.0f;
    rs.depthBiasConstantFactor = d.depthBiasConstant;
    rs.depthBiasSlopeFactor = d.depthBiasSlope;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = d.samples;
    ms.alphaToCoverageEnable = d.alphaToCoverage;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = d.depthTest;
    ds.depthWriteEnable = d.depthWrite;
    ds.depthCompareOp = d.depthCompare;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xF;
    if (d.additiveBlend) {
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    BlendMode mode = d.blend;
    if (d.additiveBlend) mode = BlendMode::Additive;
    if (d.alphaBlend) mode = BlendMode::Alpha;
    if (mode == BlendMode::Premultiplied) {
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    if (mode == BlendMode::Additive && !d.additiveBlend) {
        // src·α + dst: openBVE "additive" (glow) materials keep their alpha fade.
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    if (mode == BlendMode::Alpha) {
        // Colour: src·α + dst·(1−α). Alpha accumulates as coverage, so drawing into a
        // cleared target yields premultiplied RGBA (frame generation's UI input).
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendAttachmentState blend2 = blend;
    blend2.colorWriteMask = d.colorWriteMask2;
    if (d.colorWriteMask2 != 0xF) blend2.blendEnable = VK_FALSE;
    const VkPipelineColorBlendAttachmentState blends[2] = {blend, blend2};
    const u32 colorCount = d.colorFormat == VK_FORMAT_UNDEFINED ? 0u : (d.colorFormat2 != VK_FORMAT_UNDEFINED ? 2u : 1u);
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = colorCount;
    cb.pAttachments = blends;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    const VkFormat colorFormats[2] = {d.colorFormat, d.colorFormat2};
    rendering.colorAttachmentCount = colorCount;
    rendering.pColorAttachmentFormats = colorFormats;
    rendering.depthAttachmentFormat = d.depthFormat;

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &rendering;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = bl.pipelineLayout;
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &p));
    vkDestroyShaderModule(ctx.device, vs, nullptr);
    vkDestroyShaderModule(ctx.device, fs, nullptr);
    return p;
}

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    const bool depth = to == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL || from == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    b.subresourceRange = {VkImageAspectFlags(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace phx
