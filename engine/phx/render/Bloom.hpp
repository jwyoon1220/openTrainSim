#pragma once
// Bloom: veiling glare around bright light (sun, specular glints, bright sky through
// leaves) — a mip chain of the output-resolution HDR image, blended into it in the
// tonemapper with a small physical-ish weight (FRAME.post.x).
#include "phx/gpu/Resources.hpp"

// Bindless contract (phx/render/PostSlots.hpp): reads kTexHdr, owns kTexBloom0/kStorBloom0 + level;
// the tonemapper samples kTexBloom0 (half resolution, already blurred).

namespace phx {

class Bloom {
public:
    static constexpr u32 kLevels = 6; // 1/2 … 1/64 of the output resolution
    static constexpr VkFormat kFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    Bloom(VulkanContext& ctx, Bindless& bindless);
    ~Bloom();
    void resize(VkExtent2D display);
    // TEX_HDR must be the output-resolution scene, readable (SHADER_READ_ONLY).
    void record(VkCommandBuffer cmd);

private:
    void destroyTargets();
    VulkanContext& ctx_;
    Bindless& bindless_;
    VkPipeline pipe_ = VK_NULL_HANDLE;
    Image levels_[kLevels];
    bool initialised_ = false;
};

} // namespace phx
