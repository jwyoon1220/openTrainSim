#pragma once
// Screen-space ambient occlusion (obscurance from the depth buffer alone, normals
// reconstructed from depth): darkens creases, contact areas and the ground under objects. Works
// on any content since it needs no normals or material data.
//
// Bindless contract (phx/render/PostSlots.hpp): reads kTexDepth (render-resolution, reverse-Z,
// single sample, SHADER_READ_ONLY), owns kTexAo / kStorAo, and multiplies the occlusion into the
// image bound to kStorAoTarget (render-resolution HDR scene, GENERAL layout during record()).
#include "phx/gpu/Resources.hpp"

namespace phx {

struct SsaoParams {
    f32 nearPlane = 0.1f;     // reverse-Z infinite projection: depth = near / view z
    f32 projX = 1.0f, projY = 1.0f; // projection [0][0], [1][1]
    f32 radius = 1.0f;        // world radius (m)
    f32 intensity = 1.0f;
    f32 maxDistance = 200.0f; // fades out towards this view distance
    u32 frame = 0;            // noise rotation (temporal AA smooths it further)
};

class Ssao {
public:
    static constexpr VkFormat kFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    Ssao(VulkanContext& ctx, Bindless& bindless);
    ~Ssao();
    // Render resolution; `target` must allow storage use (it is bound to kStorAoTarget).
    void resize(VkExtent2D render, const Image& target);
    void record(VkCommandBuffer cmd, const SsaoParams& p);
    void destroyTargets();

private:
    VulkanContext& ctx_;
    Bindless& bindless_;
    VkPipeline pipe_ = VK_NULL_HANDLE;
    Image ao_;
    VkExtent2D extent_{};
};

} // namespace phx
