#include "phx/render/Ssao.hpp"

#include "phx/gpu/VulkanContext.hpp"
#include "phx/render/PostSlots.hpp"

namespace phx {

namespace {
struct Push {
    f32 nearPlane, invX, invY, radius;
    f32 intensity, maxDistance, projX;
    u32 frame, mode;
};
} // namespace

Ssao::Ssao(VulkanContext& ctx, Bindless& bindless) : ctx_(ctx), bindless_(bindless) {
    pipe_ = createComputePipeline(ctx_, bindless_, "phx/ssao.comp");
}

Ssao::~Ssao() {
    destroyTargets();
    if (pipe_) vkDestroyPipeline(ctx_.device, pipe_, nullptr);
}

void Ssao::destroyTargets() {
    destroyImage(ctx_, ao_);
    extent_ = {};
}

void Ssao::resize(VkExtent2D render, const Image& target) {
    destroyTargets();
    ao_ = createImage2D(ctx_, render, kFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    bindless_.setSampled(kTexAo, ao_.view, VK_IMAGE_LAYOUT_GENERAL);
    bindless_.setStorage(kStorAo, ao_.view);
    bindless_.setStorage(kStorAoTarget, target.view);
    extent_ = render;
}

void Ssao::record(VkCommandBuffer cmd, const SsaoParams& p) {
    if (!ao_.image) return;
    imageBarrier(cmd, ao_.image, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bindless_.pipelineLayout, 0, 1, &bindless_.set, 0, nullptr);
    Push pc{p.nearPlane, 1.0f / p.projX, 1.0f / p.projY, p.radius, p.intensity, p.maxDistance, p.projX, p.frame, 0};
    const u32 gx = (extent_.width + 7) / 8, gy = (extent_.height + 7) / 8;
    vkCmdPushConstants(cmd, bindless_.pipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, gx, gy, 1);
    imageBarrier(cmd, ao_.image, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    pc.mode = 1; // depth-aware blur, applied to the scene
    vkCmdPushConstants(cmd, bindless_.pipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, gx, gy, 1);
}

} // namespace phx
