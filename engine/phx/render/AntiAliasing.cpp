#include "phx/render/AntiAliasing.hpp"

#include "phx/gpu/VulkanContext.hpp"
#include "phx/render/PostSlots.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>

// AMD FidelityFX FSR 1 constant setup (CPU side of ffx_fsr1.h).
#define A_CPU 1
#include <ffx_a.h>
#include <ffx_fsr1.h>

namespace phx {
namespace {
constexpr VkFormat kHistoryFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr f32 kRcasSharpnessTaa = 0.4f; // stops (0 = strongest)
constexpr f32 kRcasSharpnessFsr = 0.25f;
constexpr auto kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr auto kRW = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;

// Push-constant blocks (mirror the shaders' `Push`).
struct TaaPush { u32 writeSlot, reset; };
struct EasuPush { u32 con[4][4]; u32 historySlot; };
struct RcasPush { u32 con[4]; u32 src; };
struct NoPush {};

template <class P>
void dispatch(VkCommandBuffer cmd, VkPipeline pipe, const Bindless& bl, const P& pc, VkExtent2D size) {
    static_assert(sizeof(P) <= Bindless::kPushConstantBytes);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bl.pipelineLayout, 0, 1, &bl.set, 0, nullptr);
    if constexpr (!std::is_empty_v<P>) vkCmdPushConstants(cmd, bl.pipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (size.width + 7) / 8, (size.height + 7) / 8, 1);
}
} // namespace

AntiAliasing::AntiAliasing(VulkanContext& ctx, Bindless& bindless) : ctx_(ctx), bindless_(bindless) {
    taaPipe_ = createComputePipeline(ctx_, bindless_, "phx/taa.comp");
    easuPipe_ = createComputePipeline(ctx_, bindless_, "phx/fsr_easu.comp");
    rcasPipe_ = createComputePipeline(ctx_, bindless_, "phx/fsr_rcas.comp");
    ssaaPipe_ = createComputePipeline(ctx_, bindless_, "phx/ssaa_down.comp");
}

AntiAliasing::~AntiAliasing() {
    destroyTargets();
    for (VkPipeline p : {taaPipe_, easuPipe_, rcasPipe_, ssaaPipe_, fxaaPipe_})
        if (p) vkDestroyPipeline(ctx_.device, p, nullptr);
}

VkExtent2D AntiAliasing::renderExtent(UpscaleMode mode, VkExtent2D d) {
    auto scaled = [&](f32 s) { return VkExtent2D{std::max(1u, u32(std::lround(d.width * s))), std::max(1u, u32(std::lround(d.height * s)))}; };
    if (isFsr(mode)) return scaled(1.0f / fsrScale(mode));
    if (mode == UpscaleMode::SSAA) return scaled(kSsaaScale);
    return d;
}

f32 AntiAliasing::mipBias(UpscaleMode mode, VkExtent2D render, VkExtent2D display) {
    // Temporal accumulation recovers detail below a render pixel: sharpen mips (AMD's
    // FSR guidance is log2(render/display) − 1; a plain TAA at native gets half of that).
    if (isFsr(mode)) return std::log2(f32(render.width) / f32(display.width)) - 1.0f;
    if (mode == UpscaleMode::TAA) return -0.5f;
    return 0.0f; // SSAA: the higher render resolution already selects finer mips
}

void AntiAliasing::resize(UpscaleMode mode, VkExtent2D render, VkExtent2D display, VkFormat ldrFormat, const Image& scene,
                          const Image* out) {
    destroyTargets();
    mode_ = mode, render_ = render, display_ = display;
    {   // FSR 1 constants (EASU input = TAA history at render resolution).
        AU1 c[4][4], rc[4];
        FsrEasuCon(c[0], c[1], c[2], c[3], AF1(render.width), AF1(render.height), AF1(render.width), AF1(render.height),
                   AF1(display.width), AF1(display.height));
        FsrRcasCon(rc, isFsr(mode) ? kRcasSharpnessFsr : kRcasSharpnessTaa);
        std::memcpy(easuCon_, c, sizeof(easuCon_));
        std::memcpy(rcasCon_, rc, sizeof(rcasCon_));
    }
    bindless_.setSampled(kTexScene, scene.view);
    if (out) bindless_.setStorage(kStorAAOut, out->view);
    if (isTemporalAA(mode)) {
        for (u32 i = 0; i < 2; ++i) {
            history_[i] = createImage2D(ctx_, render, kHistoryFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
            bindless_.setSampled(kTexHistory0 + i, history_[i].view, VK_IMAGE_LAYOUT_GENERAL);
            bindless_.setStorage(kStorHistory0 + i, history_[i].view);
        }
        historyInit_ = false;
    }
    if (isFsr(mode)) {
        easu_ = createImage2D(ctx_, display, kHistoryFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        bindless_.setSampled(kTexEasu, easu_.view, VK_IMAGE_LAYOUT_GENERAL);
        bindless_.setStorage(kStorEasu, easu_.view);
    }
    if (mode == UpscaleMode::FXAA) {
        ldr_ = createImage2D(ctx_, display, ldrFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        bindless_.setSampled(kTexLdr, ldr_.view);
        if (ldrFormat != ldrFormat_) {
            if (fxaaPipe_) vkDestroyPipeline(ctx_.device, fxaaPipe_, nullptr);
            fxaaPipe_ = createGraphicsPipeline(ctx_, bindless_, {"phx/fullscreen.vert", "phx/fxaa.frag", ldrFormat, false});
            ldrFormat_ = ldrFormat;
        }
    }
}

void AntiAliasing::destroyTargets() {
    for (Image& h : history_) destroyImage(ctx_, h);
    destroyImage(ctx_, easu_);
    destroyImage(ctx_, ldr_);
}

void AntiAliasing::resolve(VkCommandBuffer cmd, const Image& out, bool reset) {
    imageBarrier(cmd, out.image, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    if (mode_ == UpscaleMode::SSAA) {
        dispatch(cmd, ssaaPipe_, bindless_, NoPush{}, display_);
        return;
    }
    // TAA into this frame's history slot (reads the other one).
    if (!historyInit_ || reset) {
        for (Image& h : history_)
            imageBarrier(cmd, h.image, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE, kCompute, kRW, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_GENERAL);
        historyInit_ = true;
        reset = true;
    }
    historySlot_ ^= 1u;
    Image& cur = history_[historySlot_];
    imageBarrier(cmd, cur.image, kCompute, kRW, kCompute, kRW, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    dispatch(cmd, taaPipe_, bindless_, TaaPush{historySlot_, reset ? 1u : 0u}, render_);
    imageBarrier(cmd, cur.image, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kCompute, kRW, VK_IMAGE_LAYOUT_GENERAL,
                 VK_IMAGE_LAYOUT_GENERAL);
    u32 sharpenInput = kTexHistory0 + historySlot_;
    if (isFsr(mode_)) {
        imageBarrier(cmd, easu_.image, kCompute, kRW, kCompute, kRW, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        EasuPush ep{};
        std::memcpy(ep.con, easuCon_, sizeof(ep.con));
        ep.historySlot = historySlot_;
        dispatch(cmd, easuPipe_, bindless_, ep, display_);
        imageBarrier(cmd, easu_.image, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kCompute, kRW, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL);
        sharpenInput = kTexEasu;
    }
    RcasPush rp{};
    std::memcpy(rp.con, rcasCon_, sizeof(rp.con));
    rp.src = sharpenInput;
    dispatch(cmd, rcasPipe_, bindless_, rp, display_);
}

void AntiAliasing::recordFxaaInput(VkCommandBuffer cmd, const std::function<void(VkCommandBuffer)>& drawTonemap) {
    imageBarrier(cmd, ldr_.image, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = ldr_.view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, display_};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{0, 0, f32(display_.width), f32(display_.height), 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &ri.renderArea);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bindless_.pipelineLayout, 0, 1, &bindless_.set, 0, nullptr);
    drawTonemap(cmd);
    vkCmdEndRendering(cmd);
    imageBarrier(cmd, ldr_.image, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

} // namespace phx
