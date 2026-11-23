#pragma once
// Vendor-neutral anti-aliasing and upscaling (any GPU), next to DLSS (Upscaler):
//  * TAA   — temporal accumulation at render resolution (jitter + camera motion vectors,
//            variance clipping), followed by FSR RCAS to restore sharpness;
//  * FSR 1 — AMD FidelityFX Super Resolution 1: TAA at render resolution → EASU upscale →
//            RCAS sharpening (AMD's recommended order for a spatial upscaler);
//  * SSAA  — scene rendered at kSsaaScale per axis, filtered down;
//  * FXAA  — post-tonemap edge anti-aliasing (tonemap → LDR image → FXAA → swapchain).
//
// Bindless contract (phx/render/PostSlots.hpp): the caller fills kTexDepth (reverse-Z)
// and kTexMotion (previous − current, UV units) at render resolution for the temporal modes.
#include "phx/gpu/Resources.hpp"
#include "phx/render/Upscaler.hpp"

#include <functional>

namespace phx {

class AntiAliasing {
public:
    AntiAliasing(VulkanContext& ctx, Bindless& bindless);
    ~AntiAliasing();
    AntiAliasing(const AntiAliasing&) = delete;
    AntiAliasing& operator=(const AntiAliasing&) = delete;

    // Render resolution of a non-DLSS mode.
    static VkExtent2D renderExtent(UpscaleMode mode, VkExtent2D display);
    // Texture LOD bias of a non-DLSS mode.
    static f32 mipBias(UpscaleMode mode, VkExtent2D render, VkExtent2D display);
    // Whether the mode writes an output-resolution HDR image (resolve()), and needs jitter.
    static bool resolves(UpscaleMode m) { return isTemporalAA(m) || m == UpscaleMode::SSAA; }

    // (Re)creates targets for a mode; `scene` is the render-resolution HDR image, `out` the
    // output-resolution HDR image resolve() writes (null if the mode does not resolve).
    void resize(UpscaleMode mode, VkExtent2D render, VkExtent2D display, VkFormat ldrFormat, const Image& scene, const Image* out);
    void destroyTargets();

    // scene: SHADER_READ_ONLY. out: output-resolution HDR, written in GENERAL (left GENERAL).
    // Needs the motion vectors (TEX_MOTION) for the temporal modes.
    void resolve(VkCommandBuffer cmd, const Image& out, bool reset);

    // FXAA: tonemap into the LDR image first (call outside a render pass; `drawTonemap`
    // binds the caller's tonemap pipeline + push constants and draws a fullscreen triangle,
    // descriptor set and viewport are set), then draw fxaaPipeline() — a fullscreen triangle,
    // no push constants — instead of the tonemapper into the final target.
    [[nodiscard]] bool fxaa() const { return mode_ == UpscaleMode::FXAA; }
    void recordFxaaInput(VkCommandBuffer cmd, const std::function<void(VkCommandBuffer)>& drawTonemap);
    [[nodiscard]] VkPipeline fxaaPipeline() const { return fxaaPipe_; }

private:
    VulkanContext& ctx_;
    Bindless& bindless_;
    UpscaleMode mode_ = UpscaleMode::Native;
    VkExtent2D render_{}, display_{};
    Image history_[2], easu_, ldr_;
    u32 historySlot_ = 0;
    bool historyInit_ = false;
    VkFormat ldrFormat_ = VK_FORMAT_UNDEFINED;
    u32 easuCon_[4][4]{}, rcasCon_[4]{}; // FSR 1 constants for the current mode / resolution
    VkPipeline taaPipe_ = VK_NULL_HANDLE, easuPipe_ = VK_NULL_HANDLE, rcasPipe_ = VK_NULL_HANDLE, ssaaPipe_ = VK_NULL_HANDLE;
    VkPipeline fxaaPipe_ = VK_NULL_HANDLE;
};

} // namespace phx
