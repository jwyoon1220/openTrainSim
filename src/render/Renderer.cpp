#include "render/Renderer.hpp"

#include "bve/MeshBuilder.hpp"
#include "phx/core/Log.hpp"
#include "phx/core/Png.hpp"
#include "phx/render/PostSlots.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <glm/gtc/matrix_transform.hpp>

namespace ots {
namespace {

constexpr f32 kNear = 0.05f;
constexpr VkDeviceSize kFrameHeader = 1024;
static_assert(sizeof(GpuFrame) <= kFrameHeader);

glm::mat3 axisRot(const glm::vec3& axis, double angle) { return axisRotation(axis, float(angle)); }

phx::Image createTarget(phx::VulkanContext& ctx, VkExtent2D e, VkFormat fmt, VkSampleCountFlagBits samples, VkImageUsageFlags usage, bool depth) {
    phx::Image img;
    img.format = fmt;
    img.extent = e;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {e.width, e.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = samples;
    ci.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    if (usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) ai.preferredFlags = VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
    VK_CHECK(vmaCreateImage(ctx.allocator, &ci, &ai, &img.image, &img.allocation, nullptr));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {VkImageAspectFlags(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(ctx.device, &vi, nullptr, &img.view));
    return img;
}

GpuInstance makeInstance(const glm::dmat3& r, const glm::dvec3& t, glm::vec2 shift, f32 brightness) {
    GpuInstance g;
    g.row0 = glm::vec4(float(r[0][0]), float(r[1][0]), float(r[2][0]), float(t.x));
    g.row1 = glm::vec4(float(r[0][1]), float(r[1][1]), float(r[2][1]), float(t.y));
    g.row2 = glm::vec4(float(r[0][2]), float(r[1][2]), float(r[2][2]), float(t.z));
    // openBVE texture shift scrolls a repeating texture: keep the offset in [0, 1) (precision)
    // and have the shader repeat even when the material samples clamped (UVs inside 0..1, as
    // digit strips are: "Number.csv" shifted by 0.1 per digit)
    const glm::vec2 wrapped = shift - glm::floor(shift);
    g.params = glm::vec4(wrapped, brightness, shift != glm::vec2(0.0f) ? 1.0f : 0.0f);
    g.prev = glm::vec4(0.0f);
    return g;
}

// Generic layout transition (colour or depth by `aspect`).
void barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds,
             VkAccessFlags2 da, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = ss, b.srcAccessMask = sa, b.dstStageMask = ds, b.dstAccessMask = da;
    b.oldLayout = from, b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}
constexpr auto kAll = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
constexpr auto kAnyRead = VK_ACCESS_2_MEMORY_READ_BIT;
constexpr auto kAnyWrite = VK_ACCESS_2_MEMORY_WRITE_BIT;
constexpr auto kColorOut = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
constexpr auto kColorWrite = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
constexpr auto kColorAspect = VK_IMAGE_ASPECT_COLOR_BIT;

struct AaInfo {
    u32 samples;
    phx::UpscaleMode up;
};
AaInfo aaInfo(AaMode m) {
    using U = phx::UpscaleMode;
    switch (m) {
    case AaMode::Msaa2: return {2, U::Native};
    case AaMode::Msaa4: return {4, U::Native};
    case AaMode::Msaa8: return {8, U::Native};
    case AaMode::Fxaa: return {1, U::FXAA};
    case AaMode::Taa: return {1, U::TAA};
    case AaMode::Ssaa: return {1, U::SSAA};
    case AaMode::FsrQuality: return {1, U::FsrQuality};
    case AaMode::FsrBalanced: return {1, U::FsrBalanced};
    case AaMode::FsrPerformance: return {1, U::FsrPerformance};
    case AaMode::FsrUltra: return {1, U::FsrUltraPerformance};
    case AaMode::Dlaa: return {1, U::DLAA};
    case AaMode::DlssQuality: return {1, U::Quality};
    case AaMode::DlssBalanced: return {1, U::Balanced};
    case AaMode::DlssPerformance: return {1, U::Performance};
    case AaMode::DlssUltra: return {1, U::UltraPerformance};
    default: return {1, U::Native};
    }
}

struct AaName {
    AaMode mode;
    std::string_view name, key;
};
constexpr auto kAaNames = std::to_array<AaName>({
    {AaMode::Off, "Off", "off"},
    {AaMode::Msaa2, "MSAA 2x", "msaa2"},
    {AaMode::Msaa4, "MSAA 4x", "msaa4"},
    {AaMode::Msaa8, "MSAA 8x", "msaa8"},
    {AaMode::Fxaa, "FXAA", "fxaa"},
    {AaMode::Taa, "TAA", "taa"},
    {AaMode::Ssaa, "SSAA 2.25x", "ssaa"},
    {AaMode::FsrQuality, "FSR 1 Quality", "fsr-quality"},
    {AaMode::FsrBalanced, "FSR 1 Balanced", "fsr-balanced"},
    {AaMode::FsrPerformance, "FSR 1 Performance", "fsr-performance"},
    {AaMode::FsrUltra, "FSR 1 Ultra Perf.", "fsr-ultra"},
    {AaMode::Dlaa, "DLAA", "dlaa"},
    {AaMode::DlssQuality, "DLSS Quality", "dlss-quality"},
    {AaMode::DlssBalanced, "DLSS Balanced", "dlss-balanced"},
    {AaMode::DlssPerformance, "DLSS Performance", "dlss-performance"},
    {AaMode::DlssUltra, "DLSS Ultra Perf.", "dlss-ultra"},
});

} // namespace

std::string_view aaModeName(AaMode m) {
    const auto it = std::ranges::find(kAaNames, m, &AaName::mode);
    return it != kAaNames.end() ? it->name : "?";
}
std::string_view aaModeKey(AaMode m) {
    const auto it = std::ranges::find(kAaNames, m, &AaName::mode);
    return it != kAaNames.end() ? it->key : "off";
}
std::optional<AaMode> parseAaMode(std::string_view s) {
    const auto it = std::ranges::find(kAaNames, s, &AaName::key);
    return it != kAaNames.end() ? std::optional(it->mode) : std::nullopt;
}

Renderer::Renderer(phx::Window& window, const RenderSettings& s, ObjectLibrary& lib, ThreadPool& pool)
    : settings_(s), lib_(lib), pool_(pool), window_(window) {
    ctx_ = std::make_unique<phx::VulkanContext>(window, s.validation);
    upscaler_ = std::make_unique<phx::Upscaler>(*ctx_);
    swapchainExtra_ = 0;
    swapchain_ = std::make_unique<phx::Swapchain>(*ctx_, window.framebufferSize(), s.vsync ? phx::PresentPolicy::Vsync : phx::PresentPolicy::LowLatency);
    bindless_ = std::make_unique<phx::Bindless>(*ctx_);
    upload_ = std::make_unique<phx::UploadQueue>(*ctx_, kFrames, VkDeviceSize(s.uploadMBPerFrame) * 1024 * 1024);
    textures_ = std::make_unique<TextureCache>(*ctx_, *bindless_, *upload_, pool, s.textures);
    materials_ = std::make_unique<MaterialTable>(*textures_);
    meshes_ = std::make_unique<MeshCache>(*ctx_, *upload_, pool, lib, *materials_, s.geometry);
    ui_ = std::make_unique<phx::UiRenderer>(*ctx_, *bindless_, kFinalFormat, kFrames);
    aa_ = std::make_unique<phx::AntiAliasing>(*ctx_, *bindless_);
    ssao_ = std::make_unique<phx::Ssao>(*ctx_, *bindless_);
    bloom_ = std::make_unique<phx::Bloom>(*ctx_, *bindless_);
    gpuProfiler_ = std::make_unique<phx::GpuProfiler>(*ctx_, kFrames);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(ctx_->physicalDevice, &props);
    supportedSamples_ = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;

    for (FrameData& f : frames_) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = ctx_->graphics.family;
        VK_CHECK(vkCreateCommandPool(ctx_->device, &pci, nullptr, &f.pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(ctx_->device, &ai, &f.cmd));
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(ctx_->device, &fci, nullptr, &f.fence));
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (VkSemaphore& sem : f.acquired) VK_CHECK(vkCreateSemaphore(ctx_->device, &sci, nullptr, &sem));
        ensureFrameCapacity(f, 4096, 16384);
    }
    configure();
}

Renderer::~Renderer() {
    vkDeviceWaitIdle(ctx_->device);
    finishScreenshots();
    phx::destroyBuffer(*ctx_, screenshotBuffer_);
    pool_.cancelQueued(); // quitting: queued prefetch is not worth waiting for
    pool_.waitIdle();
    for (FrameData& f : frames_) {
        phx::destroyBuffer(*ctx_, f.data);
        for (VkSemaphore sem : f.acquired) vkDestroySemaphore(ctx_->device, sem, nullptr);
        vkDestroyFence(ctx_->device, f.fence, nullptr);
        vkDestroyCommandPool(ctx_->device, f.pool, nullptr);
    }
    destroyPipelines();
    destroyTargets();
    aa_.reset();
    ssao_.reset();
    bloom_.reset();
    upscaler_.reset();
    ui_.reset();
    meshes_.reset();
    materials_.reset();
    textures_.reset();
    upload_.reset();
    bindless_.reset();
    swapchain_.reset();
}

void Renderer::finishScreenshots() {
    if (screenshotPending_.empty()) return;
    if (vkGetFenceStatus(ctx_->device, screenshotFence_) != VK_SUCCESS) return;
    vmaInvalidateAllocation(ctx_->allocator, screenshotBuffer_.allocation, 0, VK_WHOLE_SIZE);
    const u32 w = screenshotExtent_.width, h = screenshotExtent_.height;
    std::vector<u8> rgb(std::size_t(w) * h * 3);
    const u8* src = static_cast<const u8*>(screenshotBuffer_.mapped); // final image: RGBA8
    for (std::size_t i = 0; i < std::size_t(w) * h; ++i) {
        rgb[i * 3 + 0] = src[i * 4 + 0];
        rgb[i * 3 + 1] = src[i * 4 + 1];
        rgb[i * 3 + 2] = src[i * 4 + 2];
    }
    if (screenshotPending_ == "#check") {
        // diagnostic: mean brightness of the frame, report sudden drops
        double sum = 0;
        for (std::size_t i = 0; i < rgb.size(); i += 3 * 7) sum += rgb[i] + rgb[i + 1] + rgb[i + 2];
        const double mean = sum / (rgb.size() / 21.0) / 3.0;
        if (checkMean_ > 20.0 && mean < 0.5 * checkMean_) {
            const std::string out = std::format("/tmp/ots_black_{}.png", checkSaved_);
            if (checkSaved_ < 8 && phx::writePngRgb(out, w, h, rgb.data())) ++checkSaved_;
            phx::log::warn("frame {}: dark frame mean {:.1f} (avg {:.1f}) -> {}", frameNumber_, mean, checkMean_, out);
        }
        checkMean_ = checkMean_ <= 0.0 ? mean : 0.9 * checkMean_ + 0.1 * mean;
    } else if (phx::writePngRgb(screenshotPending_, w, h, rgb.data())) phx::log::info("Screenshot: {}", screenshotPending_);
    screenshotPending_.clear();
    phx::destroyBuffer(*ctx_, screenshotBuffer_);
}

void Renderer::destroyTargets() {
    for (phx::Image* img : {&sceneColor_, &sceneResolve_, &depth_, &motionImg_, &output_, &final_, &hudless_, &uiImage_, &fgOut_})
        phx::destroyImage(*ctx_, *img);
    phx::destroyImage(*ctx_, shadowAtlas_);
    phx::destroyImage(*ctx_, depthResolve_);
    shadowAtlasSize_ = 0;
    if (ssao_) ssao_->destroyTargets();
    if (aa_) aa_->destroyTargets();
}

void Renderer::destroyPipelines() {
    for (VkPipeline* p : {&opaque_[0], &opaque_[1], &blend_[0], &blend_[1], &additive_, &background_, &tonemap_, &shadowPipe_}) {
        if (*p) vkDestroyPipeline(ctx_->device, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
}

void Renderer::recreateSwapchain() {
    vkDeviceWaitIdle(ctx_->device);
    swapchain_->recreate(window_.framebufferSize());
    configDirty_ = true;
}

void Renderer::configure() {
    vkDeviceWaitIdle(ctx_->device);
    destroyPipelines();
    destroyTargets();
    upscaler_->releaseFeature();
    upscaler_->releaseFrameGen();

    // mode → samples / upscaler mode (DLSS falls back to FSR / TAA without NGX)
    AaInfo info = aaInfo(settings_.aa);
    if (phx::isDlss(info.up) && !upscaler_->dlssAvailable()) info.up = phx::fallbackWithoutDlss(info.up);
    u32 n = info.samples;
    while (n > 1 && !(supportedSamples_ & n)) n /= 2;
    samples_ = VkSampleCountFlagBits(n);
    upMode_ = info.up;
    motion_ = samples_ == VK_SAMPLE_COUNT_1_BIT;

    // frame generation needs single-sample depth + motion vectors and NGX DLSS-G
    const u32 wantFg = std::min(settings_.frameGen, upscaler_->frameGenMaxGenerated());
    fgCount_ = (motion_ && upscaler_->frameGenAvailable()) ? std::min(wantFg, 3u) : 0u;
    if (fgCount_ != swapchainExtra_ || swapchainDirty_) {
        swapchain_.reset();
        swapchain_ = std::make_unique<phx::Swapchain>(*ctx_, window_.framebufferSize(),
                                                      settings_.vsync ? phx::PresentPolicy::Vsync : phx::PresentPolicy::LowLatency, fgCount_);
        swapchainExtra_ = fgCount_;
        swapchainDirty_ = false;
    }

    displayExt_ = swapchain_->extent;
    renderExt_ = phx::isDlss(upMode_) ? upscaler_->renderExtent(upMode_, displayExt_) : phx::AntiAliasing::renderExtent(upMode_, displayExt_);
    resolves_ = phx::isDlss(upMode_) || phx::AntiAliasing::resolves(upMode_);
    mipBias_ = phx::isDlss(upMode_) ? std::log2(f32(renderExt_.width) / f32(displayExt_.width)) - 1.0f
                                    : phx::AntiAliasing::mipBias(upMode_, renderExt_, displayExt_);

    const VkImageUsageFlags sampled = VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkImageUsageFlags sceneUse = sampled | VK_IMAGE_USAGE_STORAGE_BIT; // SSAO is applied in place
    const bool msaa = samples_ != VK_SAMPLE_COUNT_1_BIT;
    sceneColor_ = createTarget(*ctx_, renderExt_, kHdrFormat, samples_,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (msaa ? VkImageUsageFlags(VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) : sceneUse), false);
    if (msaa) sceneResolve_ = createTarget(*ctx_, renderExt_, kHdrFormat, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | sceneUse, false);
    if (msaa && settings_.ssao)
        depthResolve_ = createTarget(*ctx_, renderExt_, depthFormat_, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | sampled, true);
    depth_ = createTarget(*ctx_, renderExt_, depthFormat_, samples_,
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | (msaa ? VkImageUsageFlags(VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) : sampled), true);
    if (motion_) motionImg_ = createTarget(*ctx_, renderExt_, kMotionFormat, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | sampled, false);
    if (resolves_)
        output_ = createTarget(*ctx_, displayExt_, kHdrFormat, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_STORAGE_BIT | sampled, false);
    const VkImageUsageFlags finalUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | sampled | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    final_ = createTarget(*ctx_, displayExt_, kFinalFormat, VK_SAMPLE_COUNT_1_BIT, finalUsage, false);
    if (fgCount_) {
        hudless_ = createTarget(*ctx_, displayExt_, kFinalFormat, VK_SAMPLE_COUNT_1_BIT, finalUsage, false);
        uiImage_ = createTarget(*ctx_, displayExt_, kFinalFormat, VK_SAMPLE_COUNT_1_BIT, finalUsage, false);
        fgOut_ = createTarget(*ctx_, displayExt_, kFinalFormat, VK_SAMPLE_COUNT_1_BIT, finalUsage, false);
    }

    // bindless inputs of the post-processing modules (engine/README.md)
    const phx::Image& scene = msaa ? sceneResolve_ : sceneColor_;
    if (motion_) {
        bindless_->setSampled(phx::kTexDepth, depth_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        bindless_->setSampled(phx::kTexMotion, motionImg_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    } else if (depthResolve_.image) {
        bindless_->setSampled(phx::kTexDepth, depthResolve_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (settings_.ssao) ssao_->resize(renderExt_, scene);
    bloom_->resize(displayExt_);
    if (resolves_) bindless_->setSampled(phx::kTexHdr, output_.view, VK_IMAGE_LAYOUT_GENERAL);
    else bindless_->setSampled(phx::kTexHdr, scene.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!phx::isDlss(upMode_))
        aa_->resize(upMode_, renderExt_, displayExt_, kFinalFormat, scene, phx::AntiAliasing::resolves(upMode_) ? &output_ : nullptr);

    createPipelines();
    resetHistory_ = true;
    configDirty_ = false;
    activeDescription_ = std::format("{}{} {}x{}", phx::upscaleModeName(upMode_), msaa ? std::format(" + MSAA {}x", n) : std::string(),
                                     renderExt_.width, renderExt_.height);
    if (renderExt_.width != displayExt_.width) activeDescription_ += std::format(" -> {}x{}", displayExt_.width, displayExt_.height);
    if (fgCount_) activeDescription_ += std::format(", frame gen {}x", fgCount_ + 1);
    phx::log::info("Render path: {} (requested {}{})", activeDescription_, aaModeName(settings_.aa),
                   settings_.frameGen && !fgCount_ ? ", frame generation unavailable in this mode" : "");
}

void Renderer::createPipelines() {
    phx::GraphicsPipelineDesc d;
    d.vertex = "mesh.vert";
    d.fragment = "mesh.frag";
    d.colorFormat = kHdrFormat;
    d.colorFormat2 = motion_ ? kMotionFormat : VK_FORMAT_UNDEFINED;
    d.depthFormat = depthFormat_;
    d.depthTest = true;
    d.depthWrite = true;
    d.depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL;
    d.samples = samples_;
    d.alphaToCoverage = samples_ != VK_SAMPLE_COUNT_1_BIT;
    d.frontFace = VK_FRONT_FACE_CLOCKWISE; // openBVE faces are clockwise as seen from the front
    d.cullMode = VK_CULL_MODE_BACK_BIT;
    opaque_[0] = phx::createGraphicsPipeline(*ctx_, *bindless_, d);
    d.cullMode = VK_CULL_MODE_NONE;
    opaque_[1] = phx::createGraphicsPipeline(*ctx_, *bindless_, d);
    d.fragment = "mesh_blend.frag";
    d.alphaToCoverage = false;
    d.depthWrite = false;
    d.blend = phx::BlendMode::Alpha;
    d.colorWriteMask2 = 0; // translucent surfaces keep the opaque motion vectors
    d.cullMode = VK_CULL_MODE_BACK_BIT;
    blend_[0] = phx::createGraphicsPipeline(*ctx_, *bindless_, d);
    d.cullMode = VK_CULL_MODE_NONE;
    blend_[1] = phx::createGraphicsPipeline(*ctx_, *bindless_, d);
    d.blend = phx::BlendMode::Additive;
    additive_ = phx::createGraphicsPipeline(*ctx_, *bindless_, d);
    phx::GraphicsPipelineDesc b;
    b.vertex = "background.vert";
    b.fragment = "background.frag";
    b.colorFormat = kHdrFormat;
    b.colorFormat2 = d.colorFormat2;
    b.depthFormat = depthFormat_;
    b.samples = samples_;
    b.blend = phx::BlendMode::Alpha;
    b.colorWriteMask2 = 0xF;
    background_ = phx::createGraphicsPipeline(*ctx_, *bindless_, b);
    phx::GraphicsPipelineDesc t;
    t.vertex = "phx/fullscreen.vert";
    t.fragment = "tonemap.frag";
    t.colorFormat = kFinalFormat;
    tonemap_ = phx::createGraphicsPipeline(*ctx_, *bindless_, t);
    phx::GraphicsPipelineDesc sh;
    sh.vertex = "mesh_shadow.vert";
    sh.fragment = "mesh_shadow.frag";
    sh.depthFormat = VK_FORMAT_D32_SFLOAT;
    sh.depthTest = true;
    sh.depthWrite = true;
    sh.depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
    sh.cullMode = VK_CULL_MODE_NONE; // openBVE models are often open shells
    sh.depthBiasConstant = 1.0f;
    sh.depthBiasSlope = 2.0f;
    shadowPipe_ = phx::createGraphicsPipeline(*ctx_, *bindless_, sh);
}

void Renderer::ensureShadowAtlas() {
    const u32 size = 2 * std::clamp(settings_.shadowResolution, 512u, 4096u);
    if (shadowAtlasSize_ == size) return;
    vkDeviceWaitIdle(ctx_->device);
    phx::destroyImage(*ctx_, shadowAtlas_);
    shadowAtlas_ = createTarget(*ctx_, {size, size}, VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
                                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, true);
    if (!shadowSlot_) shadowSlot_ = bindless_->allocateSampled();
    bindless_->setSampled(shadowSlot_, shadowAtlas_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    shadowAtlasSize_ = size;
    shadowFresh_ = true;
}

void Renderer::ensureFrameCapacity(FrameData& f, std::size_t materials, std::size_t instances) {
    if (f.data.buffer && materials <= f.materialCapacity && instances <= f.instanceCapacity) return;
    phx::destroyBuffer(*ctx_, f.data);
    f.materialCapacity = std::max(materials * 3 / 2, f.materialCapacity);
    f.instanceCapacity = std::max(instances * 3 / 2, f.instanceCapacity);
    const VkDeviceSize bytes = kFrameHeader + f.materialCapacity * sizeof(GpuMaterial) + 64 + f.instanceCapacity * sizeof(GpuInstance);
    f.data = phx::createBuffer(*ctx_, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, phx::MemoryUsage::HostUpload);
}

void Renderer::pinTrain(const TrainDef& train) {
    for (const CarDef& c : train.cars)
        if (c.exterior != kNoObject) meshes_->pin(c.exterior), pinned_.push_back(c.exterior);
    if (train.cab != kNoObject) meshes_->pin(train.cab), pinned_.push_back(train.cab);
    carAnim_.assign(train.cars.size(), {});
}

void Renderer::pinTrafficTrain(const TrainDef& train) {
    for (const CarDef& c : train.cars)
        if (c.exterior != kNoObject) meshes_->pin(c.exterior), pinned_.push_back(c.exterior);
}

void Renderer::setCab(const Cab* cab) {
    cab_ = cab;
    if (!cab) return;
    for (ObjectId id : cab->bodies()) meshes_->pin(id), pinned_.push_back(id);
    for (std::size_t i = 0; i < cab->elementCount(); ++i) // every state of every element
        for (ObjectId id : cab->elementStates(i))
            if (id != kNoObject) meshes_->pin(id), pinned_.push_back(id);
    cabBodyAnim_.assign(cab->bodies().size(), {});
    cabElementAnim_.assign(cab->elementCount(), {});
}

void Renderer::prefetch(const Route& route, double tp) {
    if (route.blocks.empty()) return;
    const double d = settings_.viewDistance;
    const int b0 = route.blockAt(tp - d - 50.0), b1 = route.blockAt(tp + d + 300.0);
    for (int b = b0; b <= b1; ++b) {
        const RouteBlock& blk = route.blocks[std::size_t(b)];
        const double along = std::abs((b + 0.5) * route.blockLength - tp);
        const JobPriority priority = along < 300.0 ? JobPriority::Near : along < d ? JobPriority::View : JobPriority::Ahead;
        for (u32 i = 0; i < blk.placementCount; ++i) meshes_->request(route.placements[blk.firstPlacement + i].object, priority);
    }
    // Only requests: uploads must be queued between UploadQueue::beginFrame() and record() of
    // the same frame (render() does that), or beginFrame() would discard them.
}

JobPriority Renderer::streamPriority(const glm::dvec3& pos) const {
    const double dist = glm::length(pos - camPos_);
    const JobPriority p = dist < 300.0 ? JobPriority::Near : dist < settings_.viewDistance ? JobPriority::View : JobPriority::Ahead;
    return visible(pos, 30.0f) ? p : lessUrgent(p);
}

bool Renderer::streaming(JobPriority upTo) const {
    if (upTo == JobPriority::Idle) return meshes_->pendingCount() > 0 || textures_->pendingCount() > 0;
    return meshes_->pendingCount(upTo) > 0 || textures_->pendingCount(upTo) > 0;
}

bool Renderer::visible(const glm::dvec3& center, f32 radius) const {
    const glm::vec3 c(center - camPos_);
    const float r = std::max(radius, 0.5f);
    if (glm::length(c) - r > settings_.viewDistance) return false;
    for (const glm::vec4& p : planes_)
        if (glm::dot(glm::vec3(p), c) + p.w < -r) return false;
    return true;
}

bool Renderer::casterVisible(const glm::dvec3& center, f32 radius) const {
    const glm::vec3 c(center - camPos_);
    const f32 r = std::max(radius, 0.5f);
    for (u32 k = 0; k < cascadeCount_; ++k) {
        const phx::ShadowCascade& sc = cascades_[k];
        const f32 z = glm::dot(c - sc.center, sc.lightZ);
        if (z - r <= sc.radius && sc.covers(c, r)) return true; // nothing beyond the slice can shade it
    }
    return false;
}

void Renderer::emitStatic(ObjectId id, const glm::dmat3& rot, const glm::dvec3& pos, f32 brightness, glm::vec2 shift) {
    const GpuMesh* mesh = meshes_->drawable(id, frameNumber_, streamPriority(pos));
    if (!mesh || mesh->draws.empty()) return;
    u8 flags = 0;
    if ((emitMask_ & kItemView) && visible(pos, mesh->radius)) flags |= kItemView;
    if ((emitMask_ & kItemCaster) && cascadeCount_ && casterVisible(pos, mesh->radius + glm::length(mesh->center))) flags |= kItemCaster;
    if (!flags) return;
    const glm::dvec3 rel = pos - camPos_;
    GpuInstance inst = makeInstance(rot, rel, shift, brightness);
    // previous camera-relative position − current: camera motion plus the object's own motion
    inst.prev = glm::vec4(glm::vec3(havePrev_ ? (camPos_ - prevCamPos_) + objectMotion_ : glm::dvec3(0.0)), 0.0f);
    items_.push_back({id, mesh, inst, float(glm::length(rel + rot * glm::dvec3(mesh->center))), 0, flags});
}

void Renderer::emitAnimated(ObjectId id, const glm::dmat3& rot, const glm::dvec3& pos, AnimState& st, ScriptContext& ctx, double dt, f32 brightness) {
    const AnimatedDef* def = meshes_->animated(id, frameNumber_, streamPriority(pos));
    if (!def) return;
    st.lastFrame = frameNumber_;
    if (st.parts.size() != def->parts.size()) st.parts.assign(def->parts.size(), {});
    ctx.position = pos;
    ++stats_.animatedEvaluated;
    for (std::size_t i = 0; i < def->parts.size(); ++i) {
        const AnimatedPart& part = def->parts[i];
        PartState& ps = st.parts[i];
        if (part.isStatic()) {
            if (!ps.init) {
                ps.init = true;
                ps.state = 0;
                ps.offset = part.position;
            }
        } else {
            ps.timer += dt;
            const bool doEval = !ps.init || part.refreshRate <= 0.0f || ps.timer >= part.refreshRate;
            if (doEval) {
                const double edt = !ps.init ? 0.0 : (part.refreshRate > 0.0f ? ps.timer : dt);
                ps.timer = 0.0;
                auto run = [&](const std::shared_ptr<Function>& f, int slot) {
                    f->lastResult = ps.last[slot];
                    ps.last[slot] = f->evaluate(ctx, edt);
                    return ps.last[slot];
                };
                ps.state = part.stateFn ? int(std::lround(run(part.stateFn, 0))) : 0;
                glm::vec3 t = part.position;
                for (int k = 0; k < 3; ++k)
                    if (part.translateFn[k]) t += part.translateDir[k] * float(run(part.translateFn[k], 1 + k));
                ps.offset = t;
                for (int k = 0; k < 3; ++k)
                    if (part.rotateFn[k]) run(part.rotateFn[k], 4 + k);
                glm::vec2 sh(0.0f);
                for (int k = 0; k < 2; ++k)
                    if (part.textureShiftFn[k]) sh += part.textureShiftDir[k] * float(run(part.textureShiftFn[k], 7 + k));
                ps.shift = sh;
            }
            // rotation with optional damping (updated every frame)
            glm::mat3 r(1.0f);
            bool any = false;
            for (int k = 0; k < 3; ++k) {
                if (!part.rotateFn[k]) continue;
                const double target = ps.last[4 + k];
                const DampingSpec& dmp = part.rotateDamping[k];
                if (dmp.enabled() && ps.init) {
                    const double w = dmp.frequency, z = dmp.ratio;
                    double rem = std::min(dt, 0.25);
                    while (rem > 0.0) {
                        const double h = std::min(rem, 1.0 / 240.0);
                        const double acc = w * w * (target - ps.angle[k]) - 2.0 * z * w * ps.angVel[k];
                        ps.angVel[k] += acc * h;
                        ps.angle[k] += ps.angVel[k] * h;
                        rem -= h;
                    }
                } else {
                    ps.angle[k] = target;
                    ps.angVel[k] = 0.0;
                }
                r = axisRot(part.rotateDir[k], ps.angle[k]) * r; // X first, then Y, then Z
                any = true;
            }
            ps.rot = any ? r : glm::mat3(1.0f);
            ps.init = true;
        }
        if (ps.state < 0 || ps.state >= int(part.states.size())) continue;
        const ObjectId obj = part.states[std::size_t(ps.state)];
        if (obj == kNoObject) continue;
        const glm::dmat3 wr = rot * glm::dmat3(ps.rot);
        emitStatic(obj, wr, pos + rot * glm::dvec3(ps.offset), brightness, ps.shift);
    }
}

void Renderer::gatherRoute(const Simulation& sim, const CameraState& cam, ScriptContext& ctx, double dt) {
    const Route& route = sim.route;
    if (route.blocks.empty()) return;
    const double d = settings_.viewDistance;
    const int b0 = route.blockAt(cam.trackPosition - d - 50.0), b1 = route.blockAt(cam.trackPosition + d + 300.0);
    ctx.trainObject = false;
    for (int b = b0; b <= b1; ++b) {
        const RouteBlock& blk = route.blocks[std::size_t(b)];
        const double blockStart = b * route.blockLength;
        const bool far = glm::length(blk.origin - camPos_) > d + 1500.0;
        for (u32 i = 0; i < blk.placementCount; ++i) {
            const u32 pi = blk.firstPlacement + i;
            const Placement& p = route.placements[pi];
            if (far) {
                meshes_->request(p.object, JobPriority::Far);
                continue;
            }
            const ObjectRecord& rec = lib_[p.object];
            if (rec.kind == ObjectKind::Unsupported) continue;
            const glm::dvec3 pos = blk.origin + glm::dvec3(p.pos);
            const glm::dmat3 rot = glm::dmat3(glm::mat3_cast(p.rot));
            if (rec.kind == ObjectKind::Animated) {
                ctx.trackPosition = blockStart + p.trackOffset;
                ctx.section = p.section;
                emitAnimated(p.object, rot, pos, routeAnim_[pi], ctx, dt, 1.0f);
            } else {
                emitStatic(p.object, rot, pos, 1.0f);
            }
            ++stats_.placementsVisible;
        }
    }
    // forget animation state of placements that went out of range
    if (frameNumber_ % 120 == 0)
        for (auto it = routeAnim_.begin(); it != routeAnim_.end();)
            it = it->second.lastFrame + 600 < frameNumber_ ? routeAnim_.erase(it) : std::next(it);
}

void Renderer::gatherTrain(const Simulation& sim, const CameraState& cam, ScriptContext& ctx, double dt) {
    const TrainSim& t = *sim.train;
    const TrainDef& def = t.def();
    if (carAnim_.size() != def.cars.size()) carAnim_.assign(def.cars.size(), {});
    ctx.trainObject = true;
    ctx.section.reset();
    const int driver = def.spec.driverCar;
    if (prevCarPos_.size() != def.cars.size()) prevCarPos_.clear();
    for (std::size_t i = 0; i < def.cars.size(); ++i) {
        const CarPose& c = t.cars[i];
        objectMotion_ = prevCarPos_.empty() ? glm::dvec3(0.0) : prevCarPos_[i] - c.position;
        glm::dmat3 rot(c.X, c.Y, c.Z);
        if (def.cars[i].reversed) rot = glm::dmat3(-c.X, c.Y, -c.Z);
        ctx.car = int(i);
        ctx.trackPosition = 0.5 * (c.frontAxle + c.rearAxle);
        const bool hide = cam.interior && int(i) == driver; // the eye is inside it (cab or not)
        if ((!hide || settings_.cabSunlight) && def.cars[i].exterior != kNoObject) {
            // hidden in the cab view, the body still shades the cab (light through the windows)
            emitMask_ = hide ? kItemCaster : u8(kItemView | kItemCaster);
            if (lib_[def.cars[i].exterior].kind == ObjectKind::Animated) emitAnimated(def.cars[i].exterior, rot, c.position, carAnim_[i], ctx, dt, 1.0f);
            else emitStatic(def.cars[i].exterior, rot, c.position, 1.0f);
            emitMask_ = kItemView | kItemCaster;
        }
        if (cam.interior && int(i) == driver && cab_) {
            const glm::dmat3 crot(c.X, c.Y, c.Z);
            const f32 b = sim.cabBrightness();
            auto emit = [&](ObjectId id, const glm::dmat3& r, const glm::dvec3& p, AnimState& st, f32 br) {
                if (id == kNoObject) return;
                if (lib_[id].kind == ObjectKind::Animated) emitAnimated(id, r, p, st, ctx, dt, br);
                else emitStatic(id, r, p, br);
            };
            for (std::size_t k = 0; k < cab_->bodies().size(); ++k) emit(cab_->bodies()[k], crot, c.position, cabBodyAnim_[k], b);
            for (std::size_t k = 0; k < cab_->elementCount(); ++k) {
                const auto [er, et] = cab_->elementPose(k);
                const f32 hb = int(k) == cab_->hovered() ? b * 1.6f + 0.15f : b; // highlight under the mouse
                emit(cab_->elementObject(k), crot * er, c.position + crot * et, cabElementAnim_[k], hb);
            }
        } else if (cam.interior && int(i) == driver && def.cab != kNoObject) {
            const glm::dmat3 crot(c.X, c.Y, c.Z);
            const f32 b = sim.cabBrightness();
            if (lib_[def.cab].kind == ObjectKind::Animated) emitAnimated(def.cab, crot, c.position, cabAnim_, ctx, dt, b);
            else emitStatic(def.cab, crot, c.position, b);
        }
    }
    objectMotion_ = glm::dvec3(0.0);
    prevCarPos_.resize(def.cars.size());
    for (std::size_t i = 0; i < def.cars.size(); ++i) prevCarPos_[i] = t.cars[i].position;
}

void Renderer::gatherTraffic(const Simulation& sim, ScriptContext& ctx, double dt) {
    if (traffic_.size() != sim.traffic.size()) traffic_.resize(sim.traffic.size());
    ctx.trainObject = true;
    ctx.section.reset();
    for (std::size_t k = 0; k < sim.traffic.size(); ++k) {
        const TrafficTrain& tt = sim.traffic[k];
        TrafficState& ts = traffic_[k];
        if (!tt.active()) {
            ts.prev.clear();
            continue;
        }
        const TrainSim& t = tt.train();
        const TrainDef& def = t.def();
        if (ts.cars.size() != def.cars.size()) ts.cars.assign(def.cars.size(), {});
        if (ts.prev.size() != def.cars.size()) ts.prev.clear();
        ctx.train = &t;
        for (std::size_t i = 0; i < def.cars.size(); ++i) {
            const CarPose& c = t.cars[i];
            if (def.cars[i].exterior == kNoObject) continue;
            objectMotion_ = ts.prev.empty() ? glm::dvec3(0.0) : ts.prev[i] - c.position;
            glm::dmat3 rot(c.X, c.Y, c.Z);
            if (def.cars[i].reversed) rot = glm::dmat3(-c.X, c.Y, -c.Z);
            ctx.car = int(i);
            ctx.trackPosition = 0.5 * (c.frontAxle + c.rearAxle);
            if (lib_[def.cars[i].exterior].kind == ObjectKind::Animated) emitAnimated(def.cars[i].exterior, rot, c.position, ts.cars[i], ctx, dt, 1.0f);
            else emitStatic(def.cars[i].exterior, rot, c.position, 1.0f);
        }
        ts.prev.resize(def.cars.size());
        for (std::size_t i = 0; i < def.cars.size(); ++i) ts.prev[i] = t.cars[i].position;
    }
    ctx.train = nullptr;
    ctx.car = 0;
    objectMotion_ = glm::dvec3(0.0);
}

// With vsync (FIFO) the frame rate is the display's; left alone, the frame is built from
// input and simulation first and then blocks in vkAcquireNextImageKHR until a refresh frees
// an image (~9 ms at 165 Hz with frame generation 2x). Instead, that time is slept at the start
// of the frame: the same frames are built from input sampled that much later (lower latency)
// and the swapchain queue stays short. The delay is steered so the acquire still waits about
// kPaceMarginMs (so a slower frame does not miss its refresh).
void Renderer::pace() {
    if (!settings_.vsync || paceDelayMs_ <= 0.0) return;
    using Clock = std::chrono::steady_clock;
    const auto target = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(paceDelayMs_));
    // sleep most of it, spin the last bit (sleep overshoots by up to ~0.1-0.5 ms)
    if (paceDelayMs_ > 0.7) std::this_thread::sleep_until(target - std::chrono::microseconds(500));
    while (Clock::now() < target) std::this_thread::yield();
}

void Renderer::steerPace(double waitedMs) {
    if (!settings_.vsync) {
        paceDelayMs_ = 0.0;
        return;
    }
    if (frameNumber_ % 120 == 1 || refreshHz_ <= 0) refreshHz_ = window_.refreshRate(); // the window may move to another monitor
    const double periodMs = 1000.0 * double(fgCount_ + 1) / (refreshHz_ > 0 ? refreshHz_ : 60.0);
    paceDelayMs_ = std::clamp(paceDelayMs_ + 0.5 * (waitedMs - kPaceMarginMs), 0.0, std::max(0.0, periodMs - 2.0));
}

void Renderer::render(const Simulation& sim, const CameraState& cam, std::span<const phx::ui::Vertex> hud, double frameSeconds) {
    const auto t0 = std::chrono::steady_clock::now();
    const phx::Extent2D fb = window_.framebufferSize();
    if (fb.empty()) return;
    if (fb != phx::Extent2D{swapchain_->extent.width, swapchain_->extent.height}) recreateSwapchain();
    if (configDirty_ || swapchainDirty_) configure();

    FrameData& f = frames_[slot_];
    const auto waitStart = std::chrono::steady_clock::now();
    VK_CHECK(vkWaitForFences(ctx_->device, 1, &f.fence, VK_TRUE, UINT64_MAX));
    gpuProfiler_->beginFrame(slot_); // this slot's previous frame is done: read its timestamps
    bindless_->retireFrame();
    upload_->beginFrame(slot_);
    ++frameNumber_;

    // swapchain images: one per generated frame, then the rendered frame
    std::array<u32, 4> images{};
    u32 acquiredCount = 0;
    for (u32 k = 0; k <= fgCount_; ++k) {
        const VkResult r = vkAcquireNextImageKHR(ctx_->device, swapchain_->handle, UINT64_MAX, f.acquired[k], VK_NULL_HANDLE, &images[k]);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) break;
        ++acquiredCount;
    }
    if (acquiredCount == 0) {
        recreateSwapchain();
        return;
    }
    const u32 generated = acquiredCount - 1; // images[0 .. generated-1] generated, images[generated] real
    VK_CHECK(vkResetFences(ctx_->device, 1, &f.fence));
    steerPace(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count());
    profiler_.lap(CpuStage::Wait);

    // streaming: textures first (they gate drawing), then geometry
    textures_->update();
    meshes_->update(frameNumber_, frameSeconds);
    profiler_.lap(CpuStage::Streaming);

    // camera: unjittered projection, then the temporal jitter (render pixels)
    const VkExtent2D rext = renderExt_, dext = displayExt_;
    camPos_ = cam.position;
    if (!havePrev_) origin_ = cam.position;
    const bool temporal = phx::isDlss(upMode_) || phx::isTemporalAA(upMode_);
    const float aspect = float(dext.width) / float(std::max(1u, dext.height));
    const float fy = 1.0f / std::tan(0.5f * cam.fovY);
    glm::mat4 proj(0.0f);
    proj[0][0] = fy / aspect;
    proj[1][1] = -fy;
    proj[2][3] = 1.0f;
    proj[3][2] = kNear;
    const glm::vec2 jitter = temporal ? phx::jitterHalton(frameNumber_, rext, dext) : glm::vec2(0.0f);
    glm::mat4 projJ = proj;
    projJ[2][0] = 2.0f * jitter.x / float(rext.width); // samples land at pixel centre + jitter
    projJ[2][1] = 2.0f * jitter.y / float(rext.height);
    const glm::mat3 view = glm::transpose(glm::mat3(glm::vec3(cam.X), glm::vec3(cam.Y), glm::vec3(cam.Z)));
    const glm::mat4 viewProj = proj * glm::mat4(view);
    const glm::mat4 viewProjJ = projJ * glm::mat4(view);
    {
        std::array<glm::vec4, 4> row;
        for (int i = 0; i < 4; ++i) row[i] = glm::vec4(viewProj[0][i], viewProj[1][i], viewProj[2][i], viewProj[3][i]);
        planes_[0] = row[3] + row[0];
        planes_[1] = row[3] - row[0];
        planes_[2] = row[3] + row[1];
        planes_[3] = row[3] - row[1];
        planes_[4] = row[3] - row[2];
        for (glm::vec4& p : planes_) p /= glm::length(glm::vec3(p));
    }
    // history is useless after a jump (camera switch, teleport)
    const bool jump = havePrev_ && glm::length(camPos_ - prevCamPos_) > 50.0;
    const bool reset = resetHistory_ || !havePrev_ || jump;
    if (jump) havePrev_ = false; // no motion vectors across the jump

    // sun shadow cascades (before gathering: they decide which objects cast)
    cascadeCount_ = 0;
    const glm::vec3 toLight = glm::length(sim.route.lightDirection) > 0.0f ? glm::normalize(sim.route.lightDirection) : glm::vec3(0, 1, 0);
    if (settings_.shadows && toLight.y > 0.02f) {
        ensureShadowAtlas();
        phx::ShadowCascadeInput sci;
        sci.camera = cam.position;
        sci.forward = glm::vec3(cam.Z);
        sci.fovY = cam.fovY;
        sci.aspect = aspect;
        sci.nearPlane = kNear;
        sci.distance = std::min(settings_.shadowDistance, settings_.viewDistance);
        sci.toLight = toLight;
        sci.resolution = shadowAtlasSize_ / 2;
        phx::computeShadowCascades(sci, cascades_);
        cascadeCount_ = kCascades;
    }

    // gather instances
    stats_.placementsVisible = stats_.animatedEvaluated = 0;
    items_.clear();
    ScriptContext ctx(sim);
    gatherRoute(sim, cam, ctx, frameSeconds);
    gatherTrain(sim, cam, ctx, frameSeconds);
    gatherTraffic(sim, ctx, frameSeconds);
    profiler_.lap(CpuStage::Gather);

    if (pickRequest_) {
        pickRequest_ = false;
        const glm::dvec3 ray = glm::normalize(cam.Z + cam.X * double(pickU_ * std::tan(0.5f * cam.fovY) * aspect) +
                                              cam.Y * double(pickV_ * std::tan(0.5f * cam.fovY)));
        std::vector<std::pair<double, std::string>> hits;
        for (const Item& it : items_) {
            const glm::dvec3 c(it.instance.row0.w, it.instance.row1.w, it.instance.row2.w);
            const double t = glm::dot(c, ray);
            const double d = glm::length(c - ray * t);
            if (t > 30.0 && d < it.mesh->radius && it.mesh->radius < 80.0) hits.push_back({t, lib_[it.object].path.string()});
        }
        std::sort(hits.begin(), hits.end());
        for (std::size_t i = 0; i < hits.size() && i < 12; ++i) phx::log::info("pick {:.1f} m: {}", hits[i].first, hits[i].second);
    }
    // sort by object so each (object, material) is one instanced draw
    // Stable sorts throughout: gather order is deterministic, so coplanar faces (decals, the same
    // object placed twice, several translucent materials of one object at the same distance)
    // always resolve the same way instead of swapping every frame (flicker). As in openBVE, the
    // later-drawn of two equal-depth faces wins (GREATER_OR_EQUAL on reverse-Z).
    // Groups are ordered by the first appearance of each object (= file / placement order, like
    // openBVE's draw order), instances keep gather order inside a group.
    {
        std::unordered_map<ObjectId, u32> first;
        first.reserve(items_.size());
        for (u32 i = 0; i < items_.size(); ++i) first.try_emplace(items_[i].object, i);
        for (Item& it : items_) it.order = first[it.object];
        std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) { return a.order < b.order; });
    }
    struct Cmd {
        const GpuDraw* draw;
        u32 firstInstance, count;
        f32 distance;
        AlphaMode mode;
    };
    // Instances are written list by list: the main view, then each shadow cascade. Each
    // (object, material) of a list is one instanced draw over a contiguous range.
    std::vector<GpuInstance> instances;
    instances.reserve(items_.size() * 2);
    std::vector<Cmd> opaque, blended, shadowCmds[kCascades];
    std::vector<u32> group;
    for (std::size_t i = 0; i < items_.size();) {
        std::size_t j = i;
        while (j < items_.size() && items_[j].order == items_[i].order) ++j;
        // main view
        group.clear();
        for (std::size_t k = i; k < j; ++k)
            if (items_[k].flags & kItemView) group.push_back(u32(k));
        if (!group.empty()) {
            const u32 first = u32(instances.size());
            for (u32 k : group) instances.push_back(items_[k].instance);
            const u32 count = u32(group.size());
            for (const GpuDraw& d : items_[i].mesh->draws) {
                const AlphaMode m = materials_->mode(d.material);
                if (m == AlphaMode::Opaque || m == AlphaMode::AlphaTest) {
                    opaque.push_back({&d, first, count, 0.0f, m});
                } else {
                    // translucent (not additive): opaque texels first, with depth (openBVE smooth mode)
                    if (m == AlphaMode::Blend) opaque.push_back({&d, first, count, 0.0f, m});
                    for (u32 n = 0; n < count; ++n) blended.push_back({&d, first + n, 1, items_[group[n]].distance, m});
                }
            }
        }
        // shadow cascades
        for (u32 c = 0; c < cascadeCount_; ++c) {
            const u32 first = u32(instances.size());
            for (std::size_t k = i; k < j; ++k) {
                const Item& it = items_[k];
                if (!(it.flags & kItemCaster)) continue;
                const glm::vec3 centre(it.instance.row0.w, it.instance.row1.w, it.instance.row2.w);
                if (cascades_[c].covers(centre, it.mesh->radius + glm::length(it.mesh->center))) instances.push_back(it.instance);
            }
            const u32 count = u32(instances.size()) - first;
            if (!count) continue;
            for (const GpuDraw& d : items_[i].mesh->draws)
                if (materials_->mode(d.material) != AlphaMode::Additive) shadowCmds[c].push_back({&d, first, count, 0.0f, AlphaMode::Opaque});
        }
        i = j;
    }
    // back to front; equal distances keep file / gather order
    std::stable_sort(blended.begin(), blended.end(), [](const Cmd& a, const Cmd& b) { return std::lround(a.distance * 1000.0f) > std::lround(b.distance * 1000.0f); });

    // per-frame data: header, materials, instances
    ensureFrameCapacity(f, materials_->size() + 16, instances.size() + 16);
    auto* base = static_cast<u8*>(f.data.mapped);
    const VkDeviceSize matOffset = kFrameHeader;
    const VkDeviceSize instOffset = (matOffset + f.materialCapacity * sizeof(GpuMaterial) + 63) / 64 * 64;
    materials_->write(reinterpret_cast<GpuMaterial*>(base + matOffset), u32(f.materialCapacity));
    auto* inst = reinterpret_cast<GpuInstance*>(base + instOffset);
    std::ranges::copy(instances, inst);

    GpuFrame fr{};
    fr.viewProj = viewProjJ;
    fr.viewProjNoJitter = viewProj;
    fr.prevViewProj = havePrev_ ? prevViewProj_ : viewProj;
    fr.lightDir = glm::vec4(sim.route.lightDirection, 0.0f);
    fr.ambient = glm::vec4(sim.route.ambient, 1.0f);
    fr.diffuse = glm::vec4(sim.route.diffuse, 1.0f);
    const Fog fog = sim.route.fogAt(cam.trackPosition);
    const glm::vec3 fogColor = glm::vec3(float(fog.color & 0xFF), float((fog.color >> 8) & 0xFF), float((fog.color >> 16) & 0xFF)) / 255.0f;
    fr.fog = glm::vec4(fog.start, fog.end, fog.end > fog.start ? 1.0f : 0.0f, samples_ != VK_SAMPLE_COUNT_1_BIT ? 1.0f : 0.0f);
    fr.fogColor = glm::vec4(fogColor, 1.0f);
    {
        const auto bg = sim.route.backgroundAt(cam.trackPosition);
        std::string path;
        if (bg && *bg < sim.route.backgrounds.size()) path = sim.route.backgrounds[*bg].texture.string();
        if (path != backgroundPath_) {
            const TextureId old = backgroundTexture_;
            backgroundTexture_ = path.empty() ? kNone : textures_->acquire(path, false, 0, 0, 0, JobPriority::Near);
            textures_->release(old);
            backgroundPath_ = path;
        }
        const float radius = 1000.0f;
        const int reps = bg && *bg < sim.route.backgrounds.size() ? sim.route.backgrounds[*bg].repetitions : 6;
        fr.background = glm::vec4(radius, -0.125f * radius, 0.375f * radius, float(reps));
        fr.misc = glm::vec4(float(textures_->slot(backgroundTexture_, false)), 1.0f, float(sim.time), mipBias_);
    }
    for (u32 k = 0; k < cascadeCount_; ++k) {
        fr.shadow[k] = cascades_[k].viewProj;
        fr.shadowCascade[k] = glm::vec4(cascades_[k].texelWorld, cascades_[k].farSplit, 0.0f, 0.0f);
    }
    fr.lighting = glm::vec4(settings_.hemisphere, settings_.specular, 0.0f, 0.0f);
    fr.post = glm::vec4(settings_.bloom, settings_.saturation, settings_.contrast, settings_.vignette);
    fr.shadowParams = glm::vec4(float(cascadeCount_), float(shadowAtlasSize_), float(shadowSlot_), std::clamp(settings_.shadowStrength, 0.0f, 1.0f));
    std::memcpy(base, &fr, sizeof(fr));
    vmaFlushAllocation(ctx_->allocator, f.data.allocation, 0, VK_WHOLE_SIZE);
    ui_->upload(slot_, hud);
    profiler_.lap(CpuStage::Build);

    // ---------------------------------------------------------------- record
    VkCommandBuffer cmd = f.cmd;
    VK_CHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    upload_->record(cmd);
    ui_->prepare(cmd, slot_); // font atlas rows rasterised since the last frame
    // GPU timing: every pass is always opened and closed (empty when skipped) so the scope
    // list keeps the order of kGpuPassNames
    auto gpuPass = [&](std::size_t index) {
        if (index > 0) gpuProfiler_->end(cmd, slot_);
        gpuProfiler_->begin(cmd, slot_, kGpuPassNames[index].data());
    };
    gpuPass(0);

    // 0. sun shadow atlas: one cascade per quadrant
    PushConstants pc{};
    pc.frame = f.data.address;
    pc.materials = f.data.address + matOffset;
    pc.instances = f.data.address + instOffset;
    pc.vertices = meshes_->vertexHeap().buffer().address;
    u32 draws = 0;
    if (cascadeCount_) {
        barrier(cmd, shadowAtlas_.image, VK_IMAGE_ASPECT_DEPTH_BIT, kAll, kAnyRead,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo sd{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        sd.imageView = shadowAtlas_.view;
        sd.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        sd.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        sd.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        sd.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo sri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        sri.renderArea = {{0, 0}, {shadowAtlasSize_, shadowAtlasSize_}};
        sri.layerCount = 1;
        sri.pDepthAttachment = &sd;
        vkCmdBeginRendering(cmd, &sri);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bindless_->pipelineLayout, 0, 1, &bindless_->set, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipe_);
        vkCmdBindIndexBuffer(cmd, meshes_->indexHeap().buffer().buffer, 0, VK_INDEX_TYPE_UINT16);
        const u32 half = shadowAtlasSize_ / 2;
        for (u32 c = 0; c < cascadeCount_; ++c) {
            const VkViewport svp{float((c & 1) * half), float((c >> 1) * half), float(half), float(half), 0, 1};
            const VkRect2D ssc{{i32((c & 1) * half), i32((c >> 1) * half)}, {half, half}};
            vkCmdSetViewport(cmd, 0, 1, &svp);
            vkCmdSetScissor(cmd, 0, 1, &ssc);
            pc.cascade = c;
            for (const Cmd& sc : shadowCmds[c]) {
                pc.material = sc.draw->material;
                vkCmdPushConstants(cmd, bindless_->pipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
                vkCmdDrawIndexed(cmd, sc.draw->indexCount, sc.count, sc.draw->firstIndex, i32(sc.draw->vertexOffset), sc.firstInstance);
                ++draws;
            }
        }
        pc.cascade = 0;
        vkCmdEndRendering(cmd);
        barrier(cmd, shadowAtlas_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    } else if (shadowFresh_ && shadowAtlasSize_) {
        // never rendered yet: give the descriptor a valid layout
        barrier(cmd, shadowAtlas_.image, VK_IMAGE_ASPECT_DEPTH_BIT, kAll, kAnyRead, kAll, kAnyRead, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    shadowFresh_ = false;

    gpuPass(1);
    const bool msaa = samples_ != VK_SAMPLE_COUNT_1_BIT;
    const phx::Image& scene = msaa ? sceneResolve_ : sceneColor_;
    // 1. scene at render resolution: HDR colour (+ MSAA resolve), motion vectors, depth
    barrier(cmd, sceneColor_.image, kColorAspect, kAll, kAnyRead | kAnyWrite, kColorOut, kColorWrite, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (msaa)
        barrier(cmd, sceneResolve_.image, kColorAspect, kAll, kAnyRead | kAnyWrite, kColorOut, kColorWrite, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (motion_)
        barrier(cmd, motionImg_.image, kColorAspect, kAll, kAnyRead | kAnyWrite, kColorOut, kColorWrite, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    barrier(cmd, depth_.image, VK_IMAGE_ASPECT_DEPTH_BIT, kAll, kAnyRead | kAnyWrite,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo colors[2]{{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};
    colors[0].imageView = sceneColor_.view;
    colors[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colors[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colors[0].storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    colors[0].clearValue.color = {{fogColor.r, fogColor.g, fogColor.b, 1.0f}};
    if (msaa) {
        colors[0].resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        colors[0].resolveImageView = sceneResolve_.view;
        colors[0].resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    colors[1].imageView = motionImg_.view;
    colors[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colors[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colors[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colors[1].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    const bool ssaoOn = settings_.ssao && (motion_ || depthResolve_.image);
    if (msaa && ssaoOn)
        barrier(cmd, depthResolve_.image, VK_IMAGE_ASPECT_DEPTH_BIT, kAll, kAnyRead | kAnyWrite,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (msaa && ssaoOn) {
        depth.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        depth.resolveImageView = depthResolve_.view;
        depth.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    }
    depth.imageView = depth_.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = motion_ ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil = {0.0f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, rext};
    ri.layerCount = 1;
    ri.colorAttachmentCount = motion_ ? 2u : 1u;
    ri.pColorAttachments = colors;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{0, 0, float(rext.width), float(rext.height), 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &ri.renderArea);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bindless_->pipelineLayout, 0, 1, &bindless_->set, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, background_);
    vkCmdPushConstants(cmd, bindless_->pipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 32 * 12, 1, 0, 0);
    ++draws;
    vkCmdBindIndexBuffer(cmd, meshes_->indexHeap().buffer().buffer, 0, VK_INDEX_TYPE_UINT16);
    VkPipeline bound = VK_NULL_HANDLE;
    auto bind = [&](VkPipeline p) {
        if (p != bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = p);
    };
    for (const Cmd& c : opaque) {
        bind(opaque_[(c.draw->doubleSided || !backfaceCulling_) ? 1 : 0]);
        pc.material = c.draw->material;
        vkCmdPushConstants(cmd, bindless_->pipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
        vkCmdDrawIndexed(cmd, c.draw->indexCount, c.count, c.draw->firstIndex, i32(c.draw->vertexOffset), c.firstInstance);
        ++draws;
    }
    for (const Cmd& c : blended) {
        bind(c.mode == AlphaMode::Additive ? additive_ : blend_[(c.draw->doubleSided || !backfaceCulling_) ? 1 : 0]);
        pc.material = c.draw->material;
        vkCmdPushConstants(cmd, bindless_->pipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
        vkCmdDrawIndexed(cmd, c.draw->indexCount, 1, c.draw->firstIndex, i32(c.draw->vertexOffset), c.firstInstance);
        ++draws;
    }
    vkCmdEndRendering(cmd);

    // scene inputs → readable (ambient occlusion is multiplied into the scene first)
    gpuPass(2);
    if (ssaoOn) {
        const phx::Image& dimg = msaa ? depthResolve_ : depth_;
        barrier(cmd, dimg.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, kAll, kAnyRead, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        barrier(cmd, scene.image, kColorAspect, kColorOut, kColorWrite, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_GENERAL);
        phx::SsaoParams sp;
        sp.nearPlane = kNear;
        sp.projX = proj[0][0];
        sp.projY = proj[1][1];
        sp.radius = settings_.ssaoRadius;
        sp.intensity = settings_.ssaoStrength;
        sp.maxDistance = std::min(250.0f, settings_.viewDistance);
        sp.frame = u32(frameNumber_);
        ssao_->record(cmd, sp);
        barrier(cmd, scene.image, kColorAspect, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kAll, kAnyRead,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    } else {
        barrier(cmd, scene.image, kColorAspect, kColorOut, kColorWrite, kAll, kAnyRead, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (motion_)
        barrier(cmd, motionImg_.image, kColorAspect, kColorOut, kColorWrite, kAll, kAnyRead, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (motion_ && !ssaoOn) {
        barrier(cmd, depth_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, kAll, kAnyRead, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    // 2. upscaling / temporal AA → output-resolution HDR
    gpuPass(3);
    if (phx::isDlss(upMode_)) {
        barrier(cmd, output_.image, kColorAspect, kAll, kAnyRead, kAll, kAnyRead | kAnyWrite, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        phx::UpscaleInputs in;
        in.color = &scene;
        in.depth = &depth_;
        in.motion = &motionImg_;
        in.output = &output_;
        in.colorFormat = kHdrFormat;
        in.depthFormat = depthFormat_;
        in.motionFormat = kMotionFormat;
        in.render = rext;
        in.display = dext;
        in.jitterPx = jitter;
        in.reset = reset;
        in.frameMs = f32(frameSeconds * 1000.0);
        if (!upscaler_->evaluate(cmd, upMode_, in)) {
            phx::log::warn("DLSS evaluation failed: switching to {}", phx::upscaleModeName(phx::fallbackWithoutDlss(upMode_)));
            configDirty_ = true; // next frame: dlssAvailable() is now false → fallback
        }
        barrier(cmd, output_.image, kColorAspect, kAll, kAnyWrite, kAll, kAnyRead, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    } else if (phx::AntiAliasing::resolves(upMode_)) {
        aa_->resolve(cmd, output_, reset);
        barrier(cmd, output_.image, kColorAspect, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kAll, kAnyRead,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    }

    // 3. output transform (+FXAA) and UI. With frame generation the HUD-less image, the
    //    UI-only image and the final image are all needed.
    const VkRect2D full{{0, 0}, dext};
    const VkViewport dvp{0, 0, float(dext.width), float(dext.height), 0, 1};
    gpuPass(4);
    if (settings_.bloom > 0.0f) bloom_->record(cmd); // reads TEX_HDR (output resolution)
    auto drawTonemap = [&](VkCommandBuffer c) {
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemap_);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, bindless_->pipelineLayout, 0, 1, &bindless_->set, 0, nullptr);
        vkCmdPushConstants(c, bindless_->pipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
        vkCmdDraw(c, 3, 1, 0, 0);
    };
    auto beginPass = [&](const phx::Image& target, bool clear) {
        VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        a.imageView = target.view;
        a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderingInfo r{VK_STRUCTURE_TYPE_RENDERING_INFO};
        r.renderArea = full;
        r.layerCount = 1;
        r.colorAttachmentCount = 1;
        r.pColorAttachments = &a;
        vkCmdBeginRendering(cmd, &r);
        vkCmdSetViewport(cmd, 0, 1, &dvp);
        vkCmdSetScissor(cmd, 0, 1, &full);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bindless_->pipelineLayout, 0, 1, &bindless_->set, 0, nullptr);
    };
    const bool fg = generated > 0;
    const phx::Image& sceneOut = fg ? hudless_ : final_;
    if (upMode_ == phx::UpscaleMode::FXAA) {
        aa_->recordFxaaInput(cmd, drawTonemap);
        barrier(cmd, sceneOut.image, kColorAspect, kAll, kAnyRead | kAnyWrite, kColorOut, kColorWrite, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        beginPass(sceneOut, false);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, aa_->fxaaPipeline());
        vkCmdDraw(cmd, 3, 1, 0, 0);
    } else {
        barrier(cmd, sceneOut.image, kColorAspect, kAll, kAnyRead | kAnyWrite, kColorOut, kColorWrite, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        beginPass(sceneOut, false);
        drawTonemap(cmd);
    }
    if (!fg) {
        vkCmdEndRendering(cmd);
        gpuPass(5); // the UI goes into the same image: own pass so its cost shows
        beginPass(sceneOut, false);
        ui_->record(cmd, slot_, dext);
    }
    vkCmdEndRendering(cmd);
    if (fg) gpuPass(5);
    if (fg) {
        // final = HUD-less + UI; UI alone into a cleared image (premultiplied)
        barrier(cmd, hudless_.image, kColorAspect, kColorOut, kColorWrite, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        barrier(cmd, final_.image, kColorAspect, kAll, kAnyRead | kAnyWrite, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy{};
        copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {dext.width, dext.height, 1};
        vkCmdCopyImage(cmd, hudless_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, final_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(cmd, final_.image, kColorAspect, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, kColorOut,
                kColorWrite | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        beginPass(final_, false);
        ui_->record(cmd, slot_, dext);
        vkCmdEndRendering(cmd);
        barrier(cmd, uiImage_.image, kColorAspect, kAll, kAnyRead | kAnyWrite, kColorOut, kColorWrite, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        beginPass(uiImage_, true);
        ui_->record(cmd, slot_, dext);
        vkCmdEndRendering(cmd);
        barrier(cmd, hudless_.image, kColorAspect, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, kAll, kAnyRead,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        barrier(cmd, uiImage_.image, kColorAspect, kColorOut, kColorWrite, kAll, kAnyRead, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_GENERAL);
        barrier(cmd, final_.image, kColorAspect, kColorOut, kColorWrite, kAll, kAnyRead, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_GENERAL);
    } else {
        barrier(cmd, final_.image, kColorAspect, kColorOut, kColorWrite, kAll, kAnyRead, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_GENERAL);
    }

    // 4. blit to the swapchain: generated frames first (DLSS-G), then the rendered one
    gpuPass(6);
    auto blitToSwap = [&](const phx::Image& src, VkImageLayout srcLayout, u32 image) {
        const VkImage dst = swapchain_->images[image];
        barrier(cmd, dst, kColorAspect, kAll, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit b{};
        b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.srcOffsets[1] = {i32(dext.width), i32(dext.height), 1};
        b.dstOffsets[1] = {i32(swapchain_->extent.width), i32(swapchain_->extent.height), 1};
        vkCmdBlitImage(cmd, src.image, srcLayout, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
        barrier(cmd, dst, kColorAspect, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE,
                VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    };
    if (fg) {
        // camera matrices for NGX: unjittered, camera-relative ("view" = rotation only)
        const glm::mat4 v4(view), pv4(prevView_);
        const glm::dvec3 dc = havePrev_ ? camPos_ - prevCamPos_ : glm::dvec3(0.0);
        const glm::mat4 toPrev = glm::translate(glm::mat4(1.0f), glm::vec3(dc));
        const glm::mat4 clipToPrevClip = (havePrev_ ? prevProj_ * pv4 : proj * v4) * toPrev * glm::inverse(proj * v4);
        phx::FrameGenInputs g;
        g.backbuffer = &final_;
        g.hudless = &hudless_;
        g.ui = &uiImage_;
        g.depth = &depth_;
        g.motion = &motionImg_;
        g.output = &fgOut_;
        g.colorFormat = kFinalFormat;
        g.depthFormat = depthFormat_;
        g.motionFormat = kMotionFormat;
        g.render = rext;
        g.display = dext;
        g.viewToClip = proj;
        g.clipToView = glm::inverse(proj);
        g.clipToPrevClip = clipToPrevClip;
        g.prevClipToClip = glm::inverse(clipToPrevClip);
        g.cameraPos = glm::vec3(camPos_ - origin_);
        g.cameraUp = glm::vec3(cam.Y);
        g.cameraRight = glm::vec3(cam.X);
        g.cameraFwd = glm::vec3(cam.Z);
        g.nearZ = kNear;
        g.farZ = 1.0e5f;
        g.fovY = cam.fovY;
        g.aspect = aspect;
        g.reset = reset;
        g.count = generated;
        for (u32 k = 0; k < generated; ++k) {
            g.index = k + 1;
            barrier(cmd, fgOut_.image, kColorAspect, kAll, kAnyRead, kAll, kAnyRead | kAnyWrite, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
            const bool ok = upscaler_->evaluateFrameGen(cmd, g);
            barrier(cmd, fgOut_.image, kColorAspect, kAll, kAnyWrite, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            blitToSwap(ok ? fgOut_ : final_, VK_IMAGE_LAYOUT_GENERAL, images[k]);
            if (!ok) configDirty_ = true; // frame generation became unavailable
        }
    }
    barrier(cmd, final_.image, kColorAspect, kAll, kAnyRead, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    blitToSwap(final_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images[generated]);
    gpuProfiler_->end(cmd, slot_);

    if (frameCheck_ && screenshotPath_.empty() && screenshotPending_.empty()) screenshotPath_ = "#check";
    if (!screenshotPath_.empty() && screenshotPending_.empty()) {
        phx::destroyBuffer(*ctx_, screenshotBuffer_);
        screenshotExtent_ = dext;
        screenshotBuffer_ = phx::createBuffer(*ctx_, VkDeviceSize(dext.width) * dext.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                              phx::MemoryUsage::HostReadback);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {dext.width, dext.height, 1};
        vkCmdCopyImageToBuffer(cmd, final_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, screenshotBuffer_.buffer, 1, &region);
        screenshotPending_ = std::move(screenshotPath_);
        screenshotPath_.clear();
        screenshotFence_ = f.fence;
    }
    VK_CHECK(vkEndCommandBuffer(cmd));
    profiler_.lap(CpuStage::Record);

    // ---------------------------------------------------------------- submit + present
    std::array<VkSemaphoreSubmitInfo, 4> waits{}, signals{};
    for (u32 k = 0; k < acquiredCount; ++k) {
        waits[k] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        waits[k].semaphore = f.acquired[k];
        waits[k].stageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        signals[k] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signals[k].semaphore = swapchain_->renderFinished[images[k]];
        signals[k].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = acquiredCount;
    si.pWaitSemaphoreInfos = waits.data();
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = acquiredCount;
    si.pSignalSemaphoreInfos = signals.data();
    VK_CHECK(vkQueueSubmit2(ctx_->graphics.queue, 1, &si, f.fence));
    bool outOfDate = false;
    for (u32 k = 0; k < acquiredCount; ++k) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &swapchain_->renderFinished[images[k]];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain_->handle;
        pi.pImageIndices = &images[k];
        const VkResult r = vkQueuePresentKHR(ctx_->graphics.queue, &pi);
        outOfDate |= r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR;
    }
    if (outOfDate) recreateSwapchain();
    profiler_.lap(CpuStage::Submit);
    slot_ = (slot_ + 1) % kFrames;
    finishScreenshots();

    // temporal state for the next frame
    prevViewProj_ = viewProj;
    prevProj_ = proj;
    prevView_ = view;
    prevCamPos_ = camPos_;
    havePrev_ = true;
    resetHistory_ = false;

    // stats
    stats_.drawCalls = draws;
    stats_.instances = u32(instances.size());
    stats_.textureBytes = textures_->residentBytes();
    stats_.textureRawBytes = textures_->uncompressedBytes();
    stats_.textureCount = textures_->residentCount();
    stats_.texturesPending = textures_->pendingCount();
    stats_.vertexBytes = meshes_->vertexHeap().used() * sizeof(GpuVertex);
    stats_.indexBytes = meshes_->indexHeap().used() * sizeof(u16);
    stats_.vertexCapacity = meshes_->vertexHeap().capacity() * sizeof(GpuVertex);
    stats_.indexCapacity = meshes_->indexHeap().capacity() * sizeof(u16);
    stats_.meshesResident = meshes_->residentCount();
    stats_.meshesPending = meshes_->pendingCount();
    stats_.jobs = pool_.load();
    stats_.presentedPerFrame = fgCount_ + 1;
    const auto& gpu = gpuProfiler_->smoothed();
    stats_.gpuTimed = gpu.size() == kGpuPassNames.size();
    for (std::size_t i = 0; i < stats_.gpuMs.size(); ++i) stats_.gpuMs[i] = i < gpu.size() ? f32(gpu[i].second) : 0.0f;
    if (frameNumber_ % 30 == 0) {
        std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets;
        vmaGetHeapBudgets(ctx_->allocator, budgets.data());
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(ctx_->physicalDevice, &mp);
        stats_.vramUsage = stats_.vramBudget = 0;
        for (u32 h = 0; h < mp.memoryHeapCount; ++h)
            if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                stats_.vramUsage += budgets[h].statistics.allocationBytes;
                stats_.vramBudget += budgets[h].budget;
            }
    }
    stats_.cpuMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace ots
