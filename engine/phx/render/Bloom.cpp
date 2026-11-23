#include "phx/render/Bloom.hpp"

#include "phx/gpu/VulkanContext.hpp"
#include "phx/render/PostSlots.hpp"

#include <algorithm>

namespace phx {
namespace {
constexpr auto kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr auto kRW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
} // namespace

Bloom::Bloom(VulkanContext& ctx, Bindless& bindless) : ctx_(ctx), bindless_(bindless) {
    pipe_ = createComputePipeline(ctx_, bindless_, "phx/bloom.comp");
}

Bloom::~Bloom() {
    destroyTargets();
    vkDestroyPipeline(ctx_.device, pipe_, nullptr);
}

void Bloom::destroyTargets() {
    for (Image& l : levels_) destroyImage(ctx_, l);
}

void Bloom::resize(VkExtent2D display) {
    destroyTargets();
    for (u32 i = 0; i < kLevels; ++i) {
        const VkExtent2D e{std::max(1u, display.width >> (i + 1)), std::max(1u, display.height >> (i + 1))};
        levels_[i] = createImage2D(ctx_, e, kFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        bindless_.setSampled(kTexBloom0 + i, levels_[i].view, VK_IMAGE_LAYOUT_GENERAL);
        bindless_.setStorage(kStorBloom0 + i, levels_[i].view);
    }
    initialised_ = false;
}

void Bloom::record(VkCommandBuffer cmd) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bindless_.pipelineLayout, 0, 1, &bindless_.set, 0, nullptr);
    auto dispatch = [&](u32 mode, u32 level) {
        const struct { u32 mode, level; } pc{mode, level};
        vkCmdPushConstants(cmd, bindless_.pipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
        const VkExtent2D e = levels_[level].extent;
        vkCmdDispatch(cmd, (e.width + 7) / 8, (e.height + 7) / 8, 1);
        imageBarrier(cmd, levels_[level].image, kCompute, kRW, kCompute | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, kRW,
                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    };
    for (u32 i = 0; i < kLevels; ++i) // previous frame's tonemap read level 0; contents are rewritten
        imageBarrier(cmd, levels_[i].image, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, kCompute, kRW,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    for (u32 i = 0; i < kLevels; ++i) dispatch(i == 0 ? 0u : 1u, i);
    for (u32 i = kLevels - 1; i-- > 0;) dispatch(2u, i);
    initialised_ = true;
}

} // namespace phx
