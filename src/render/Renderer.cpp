#include "render/Renderer.hpp"

#include "bve/MeshBuilder.hpp"
#include "phx/core/Log.hpp"
#include "phx/core/Png.hpp"
#include "phx/render/PostSlots.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <glm/gtc/matrix_transform.hpp>

namespace ots {
namespace {

constexpr f32 kNear = 0.05f;
constexpr VkDeviceSize kFrameHeader = 512;
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
    g.params = glm::vec4(shift, brightness, 0.0f);
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
    const char* name;
    const char* key;
};
constexpr AaName kAaNames[] = {
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
};

} // namespace

const char* aaModeName(AaMode m) {
    for (const AaName& n : kAaNames)
        if (n.mode == m) return n.name;
    return "?";
}
const char* aaModeKey(AaMode m) {
    for (const AaName& n : kAaNames)
        if (n.mode == m) return n.key;
    return "off";
}
bool parseAaMode(std::string_view s, AaMode& out) {
    for (const AaName& n : kAaNames)
        if (s == n.key) return out = n.mode, true;
    return false;
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
    ui_ = std::make_unique<phx::UiRenderer>(*ctx_, *bindless_, kFinalFormat, kFrames, 32768);
    aa_ = std::make_unique<phx::AntiAliasing>(*ctx_, *bindless_);

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
    if (aa_) aa_->destroyTargets();
}

void Renderer::destroyPipelines() {
    for (VkPipeline* p : {&opaque_[0], &opaque_[1], &blend_[0], &blend_[1], &additive_, &background_, &tonemap_}) {
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
    const bool msaa = samples_ != VK_SAMPLE_COUNT_1_BIT;
    sceneColor_ = createTarget(*ctx_, renderExt_, kHdrFormat, samples_,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (msaa ? VkImageUsageFlags(VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) : sampled), false);
    if (msaa) sceneResolve_ = createTarget(*ctx_, renderExt_, kHdrFormat, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | sampled, false);
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
    }
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

void Renderer::prefetch(const Route& route, double tp) {
    if (route.blocks.empty()) return;
    const double d = settings_.viewDistance;
    const int b0 = route.blockAt(tp - d - 50.0), b1 = route.blockAt(tp + d + 300.0);
    for (int b = b0; b <= b1; ++b) {
        const RouteBlock& blk = route.blocks[std::size_t(b)];
        for (u32 i = 0; i < blk.placementCount; ++i) meshes_->request(route.placements[blk.firstPlacement + i].object, true);
    }
    // Only requests: uploads must be queued between UploadQueue::beginFrame() and record() of
    // the same frame (render() does that), or beginFrame() would discard them.
}

bool Renderer::streaming() const { return meshes_->pendingCount() > 0 || textures_->pendingCount() > 0; }

bool Renderer::visible(const glm::dvec3& center, f32 radius) const {
    const glm::vec3 c(center - camPos_);
    const float r = std::max(radius, 0.5f);
    if (glm::length(c) - r > settings_.viewDistance) return false;
    for (const glm::vec4& p : planes_)
        if (glm::dot(glm::vec3(p), c) + p.w < -r) return false;
    return true;
}

void Renderer::emitStatic(ObjectId id, const glm::dmat3& rot, const glm::dvec3& pos, f32 brightness, glm::vec2 shift) {
    const GpuMesh* mesh = meshes_->drawable(id, frameNumber_);
    if (!mesh || mesh->draws.empty()) return;
    if (!visible(pos, mesh->radius)) return;
    const glm::dvec3 rel = pos - camPos_;
    GpuInstance inst = makeInstance(rot, rel, shift, brightness);
    // previous camera-relative position − current: camera motion plus the object's own motion
    inst.prev = glm::vec4(glm::vec3(havePrev_ ? (camPos_ - prevCamPos_) + objectMotion_ : glm::dvec3(0.0)), 0.0f);
    items_.push_back({id, mesh, inst, float(glm::length(rel + rot * glm::dvec3(mesh->center)))});
}

void Renderer::emitAnimated(ObjectId id, const glm::dmat3& rot, const glm::dvec3& pos, AnimState& st, ScriptContext& ctx, double dt, f32 brightness) {
    const AnimatedDef* def = meshes_->animated(id, frameNumber_);
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
                meshes_->request(p.object);
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
    ctx.section = -1;
    const int driver = def.spec.driverCar;
    if (prevCarPos_.size() != def.cars.size()) prevCarPos_.clear();
    for (std::size_t i = 0; i < def.cars.size(); ++i) {
        const CarPose& c = t.cars[i];
        objectMotion_ = prevCarPos_.empty() ? glm::dvec3(0.0) : prevCarPos_[i] - c.position;
        glm::dmat3 rot(c.X, c.Y, c.Z);
        if (def.cars[i].reversed) rot = glm::dmat3(-c.X, c.Y, -c.Z);
        ctx.car = int(i);
        ctx.trackPosition = 0.5 * (c.frontAxle + c.rearAxle);
        const bool hide = cam.interior && int(i) == driver && def.cab != kNoObject;
        if (!hide && def.cars[i].exterior != kNoObject) {
            if (lib_[def.cars[i].exterior].kind == ObjectKind::Animated) emitAnimated(def.cars[i].exterior, rot, c.position, carAnim_[i], ctx, dt, 1.0f);
            else emitStatic(def.cars[i].exterior, rot, c.position, 1.0f);
        }
        if (cam.interior && int(i) == driver && def.cab != kNoObject) {
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

void Renderer::render(const Simulation& sim, const CameraState& cam, std::span<const phx::ui::Vertex> hud, double frameSeconds) {
    const auto t0 = std::chrono::steady_clock::now();
    const phx::Extent2D fb = window_.framebufferSize();
    if (fb.empty()) return;
    if (fb != phx::Extent2D{swapchain_->extent.width, swapchain_->extent.height}) recreateSwapchain();
    if (configDirty_ || swapchainDirty_) configure();

    FrameData& f = frames_[slot_];
    VK_CHECK(vkWaitForFences(ctx_->device, 1, &f.fence, VK_TRUE, UINT64_MAX));
    bindless_->retireFrame();
    upload_->beginFrame(slot_);
    ++frameNumber_;

    // swapchain images: one per generated frame, then the rendered frame
    u32 images[4] = {};
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

    // streaming: textures first (they gate drawing), then geometry
    textures_->update();
    meshes_->update(frameNumber_, frameSeconds);

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
        glm::vec4 row[4];
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

    // gather instances
    stats_.placementsVisible = stats_.animatedEvaluated = 0;
    items_.clear();
    ScriptContext ctx(sim);
    gatherRoute(sim, cam, ctx, frameSeconds);
    gatherTrain(sim, cam, ctx, frameSeconds);

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
    std::vector<Cmd> opaque, blended;
    for (std::size_t i = 0; i < items_.size();) {
        std::size_t j = i;
        while (j < items_.size() && items_[j].order == items_[i].order) ++j;
        for (const GpuDraw& d : items_[i].mesh->draws) {
            const AlphaMode m = materials_->mode(d.material);
            if (m == AlphaMode::Opaque || m == AlphaMode::AlphaTest) {
                opaque.push_back({&d, u32(i), u32(j - i), 0.0f, m});
            } else {
                // translucent (not additive): opaque texels first, with depth (openBVE smooth mode)
                if (m == AlphaMode::Blend) opaque.push_back({&d, u32(i), u32(j - i), 0.0f, m});
                for (std::size_t k = i; k < j; ++k) blended.push_back({&d, u32(k), 1, items_[k].distance, m});
            }
        }
        i = j;
    }
    // back to front; equal distances keep file / gather order
    std::stable_sort(blended.begin(), blended.end(), [](const Cmd& a, const Cmd& b) { return std::lround(a.distance * 1000.0f) > std::lround(b.distance * 1000.0f); });

    // per-frame data: header, materials, instances
    ensureFrameCapacity(f, materials_->size() + 16, items_.size() + 16);
    auto* base = static_cast<u8*>(f.data.mapped);
    const VkDeviceSize matOffset = kFrameHeader;
    const VkDeviceSize instOffset = (matOffset + f.materialCapacity * sizeof(GpuMaterial) + 63) / 64 * 64;
    materials_->write(reinterpret_cast<GpuMaterial*>(base + matOffset), u32(f.materialCapacity));
    auto* inst = reinterpret_cast<GpuInstance*>(base + instOffset);
    for (std::size_t i = 0; i < items_.size(); ++i) inst[i] = items_[i].instance;

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
        const int bg = sim.route.backgroundAt(cam.trackPosition);
        std::string path;
        if (bg >= 0 && std::size_t(bg) < sim.route.backgrounds.size()) path = sim.route.backgrounds[std::size_t(bg)].texture.string();
        if (path != backgroundPath_) {
            const TextureId old = backgroundTexture_;
            backgroundTexture_ = path.empty() ? kNone : textures_->acquire(path, false, 0, 0, 0);
            textures_->release(old);
            backgroundPath_ = path;
        }
        const float radius = 1000.0f;
        const int reps = bg >= 0 && std::size_t(bg) < sim.route.backgrounds.size() ? sim.route.backgrounds[std::size_t(bg)].repetitions : 6;
        fr.background = glm::vec4(radius, -0.125f * radius, 0.375f * radius, float(reps));
        fr.misc = glm::vec4(float(textures_->slot(backgroundTexture_, false)), 1.0f, float(sim.time), mipBias_);
    }
    std::memcpy(base, &fr, sizeof(fr));
    vmaFlushAllocation(ctx_->allocator, f.data.allocation, 0, VK_WHOLE_SIZE);
    ui_->upload(slot_, hud);

    // ---------------------------------------------------------------- record
    VkCommandBuffer cmd = f.cmd;
    VK_CHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    upload_->record(cmd);

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
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
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
    PushConstants pc{};
    pc.frame = f.data.address;
    pc.materials = f.data.address + matOffset;
    pc.instances = f.data.address + instOffset;
    pc.vertices = meshes_->vertexHeap().buffer().address;
    u32 draws = 0;
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

    // scene inputs → readable
    barrier(cmd, scene.image, kColorAspect, kColorOut, kColorWrite, kAll, kAnyRead, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (motion_) {
        barrier(cmd, motionImg_.image, kColorAspect, kColorOut, kColorWrite, kAll, kAnyRead, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        barrier(cmd, depth_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, kAll, kAnyRead, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    // 2. upscaling / temporal AA → output-resolution HDR
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
    auto drawTonemap = [&](VkCommandBuffer c) {
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemap_);
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
    if (!fg) ui_->record(cmd, slot_, dext);
    vkCmdEndRendering(cmd);
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

    // ---------------------------------------------------------------- submit + present
    VkSemaphoreSubmitInfo waits[4]{}, signals[4]{};
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
    si.pWaitSemaphoreInfos = waits;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = acquiredCount;
    si.pSignalSemaphoreInfos = signals;
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
    stats_.instances = u32(items_.size());
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
    if (frameNumber_ % 30 == 0) {
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
        vmaGetHeapBudgets(ctx_->allocator, budgets);
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
