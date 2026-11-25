#pragma once
// Scene renderer (Vulkan 1.3, dynamic rendering, bindless), wired as engine/README.md:
//   scene (render resolution HDR + reverse-Z depth + motion vectors, jittered for temporal
//   modes) → DLSS SR/DLAA or AntiAliasing resolve (TAA / FSR 1 / SSAA) or MSAA resolve →
//   output transform (+ FXAA) → UI → optional DLSS frame generation → swapchain.
//  * one draw per (object part, material) with every visible placement as an instance;
//  * camera-relative double → float transforms built only for visible placements;
//  * streaming: objects within the view distance (+ prefetch) are requested, loaded on
//    workers, uploaded within a per-frame budget and evicted when unused.
#include "cab/Cab.hpp"
#include "render/MeshCache.hpp"
#include "sim/World.hpp"

#include "util/Profiler.hpp"

#include "phx/gpu/GpuProfiler.hpp"
#include "phx/gpu/Swapchain.hpp"
#include "phx/gpu/VulkanContext.hpp"
#include "phx/platform/Window.hpp"
#include "phx/render/AntiAliasing.hpp"
#include "phx/render/ShadowCascades.hpp"
#include "phx/render/Bloom.hpp"
#include "phx/render/Ssao.hpp"
#include "phx/render/UiRenderer.hpp"
#include "phx/render/Upscaler.hpp"

#include <optional>
#include <array>
#include <cstdlib>
#include <memory>
#include <span>
#include <unordered_map>

namespace ots {

// Anti-aliasing / upscaling choice of the settings window.
enum class AaMode : u8 {
    Off, Msaa2, Msaa4, Msaa8, Fxaa, Taa, Ssaa,
    FsrQuality, FsrBalanced, FsrPerformance, FsrUltra,
    Dlaa, DlssQuality, DlssBalanced, DlssPerformance, DlssUltra,
    Count
};
std::string_view aaModeName(AaMode m);
std::string_view aaModeKey(AaMode m); // config value
std::optional<AaMode> parseAaMode(std::string_view s);
inline bool isDlssMode(AaMode m) { return m >= AaMode::Dlaa; }

struct RenderSettings {
    AaMode aa = AaMode::Msaa4;
    u32 frameGen = 0;         // generated frames per rendered frame (DLSS-G), 0 = off
    bool vsync = true;
    bool validation = false;
    f32 viewDistance = 1500.0f;
    u32 uploadMBPerFrame = 48;
    TextureSettings textures;
    GeometrySettings geometry;
    // sun shadows (cascaded shadow maps)
    bool shadows = true;
    u32 shadowResolution = 2048; // per cascade (4 cascades in a 2×2 atlas)
    f32 shadowDistance = 250.0f; // m
    f32 shadowStrength = 1.0f;   // 0..1
    bool cabSunlight = true;     // the hidden driver car body shades the cab (light through the windows)
    // screen-space ambient occlusion
    bool ssao = true;
    f32 ssaoRadius = 1.2f;   // m
    f32 ssaoStrength = 1.0f;
    // lighting and output grade (0 = off)
    f32 hemisphere = 1.0f;   // sky / ground ambient
    f32 specular = 0.15f;    // sun highlight
    f32 bloom = 0.6f;
    f32 saturation = 1.1f;   // 1 = unchanged
    f32 contrast = 0.2f;     // S-curve amount
    f32 vignette = 0.12f;
};

struct CameraState {
    glm::dvec3 position{0.0};
    glm::dvec3 X{1, 0, 0}, Y{0, 1, 0}, Z{0, 0, 1}; // right, up, forward
    f32 fovY = 0.87f;                              // radians
    double trackPosition = 0.0;                    // for streaming
    bool interior = true;                          // draw the cab, hide the driver car exterior
};

struct RenderStats {
    u32 drawCalls = 0, instances = 0, placementsVisible = 0, animatedEvaluated = 0;
    f32 cpuMs = 0.0f;
    std::size_t textureBytes = 0, textureRawBytes = 0, textureCount = 0, texturesPending = 0;
    std::size_t vertexBytes = 0, indexBytes = 0, vertexCapacity = 0, indexCapacity = 0;
    std::size_t meshesResident = 0, meshesPending = 0;
    std::size_t vramUsage = 0, vramBudget = 0;
    std::array<f32, 7> gpuMs{}; // smoothed per pass, see kGpuPassNames
    bool gpuTimed = false;      // timestamps supported and read back
    ThreadPool::Load jobs;      // streaming workers
    u32 presentedPerFrame = 1;  // 1 + DLSS-G generated frames
};

inline constexpr std::array<std::string_view, 7> kGpuPassNames{"shadow", "scene", "ssao", "aa", "post", "ui", "present"};

class Renderer {
public:
    Renderer(phx::Window& window, const RenderSettings& settings, ObjectLibrary& lib, ThreadPool& pool);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Pins the train's objects (exterior cars, cab) so they are never evicted.
    void pinTrain(const TrainDef& train);
    // Pins the exterior car objects of an AI train type (see Simulation::traffic).
    void pinTrafficTrain(const TrainDef& train);
    // Scripted cab (cab.lua): replaces the openBVE cab while set; its objects are pinned.
    void setCab(const Cab* cab);
    // Requests everything around a camera without drawing (loading screen).
    void prefetch(const Route& route, double trackPosition);
    // Anything still loading at `upTo` or more urgent (Idle = anything at all).
    [[nodiscard]] bool streaming(JobPriority upTo = JobPriority::Idle) const;
    void render(const Simulation& sim, const CameraState& cam, std::span<const phx::ui::Vertex> hud, double frameSeconds);
    // Vsync: sleeps the time the previous frames spent blocked on the swapchain, so the frame
    // is built from fresher input. Call before reading input. No-op without vsync.
    void pace();
    // Images presented per rendered frame (1 + frames generated by DLSS-G).
    [[nodiscard]] u32 presentedPerFrame() const { return fgCount_ + 1; }
    void setViewDistance(f32 d) { settings_.viewDistance = d; }
    // lighting / effects (take effect next frame; SSAO needs its targets: reconfigure)
    void setShadows(bool on) { settings_.shadows = on; }
    void setSsao(bool on) { settings_.ssao = on, configDirty_ = true; }
    void setBloom(f32 v) { settings_.bloom = v; }
    void setGrade(f32 saturation, f32 contrast, f32 vignette) { settings_.saturation = saturation, settings_.contrast = contrast, settings_.vignette = vignette; }
    [[nodiscard]] const RenderSettings& settings() const { return settings_; }
    // Applied at the start of the next frame (targets / pipelines are rebuilt).
    void setAaMode(AaMode m) { settings_.aa = m, configDirty_ = true; }
    void setFrameGen(u32 generated) { settings_.frameGen = generated, configDirty_ = true; }
    void setVsync(bool on) { settings_.vsync = on, swapchainDirty_ = true; }
    [[nodiscard]] AaMode aaMode() const { return settings_.aa; }
    [[nodiscard]] u32 frameGen() const { return settings_.frameGen; }
    [[nodiscard]] bool vsync() const { return settings_.vsync; }
    [[nodiscard]] bool dlssAvailable() const { return upscaler_ && upscaler_->dlssAvailable(); }
    [[nodiscard]] u32 frameGenMax() const { return upscaler_ ? upscaler_->frameGenMaxGenerated() : 0; }
    [[nodiscard]] const std::string& dlssStatus() const { return upscaler_->status(); }
    // What actually runs (after DLSS fallback / MSAA limits), e.g. "FSR 1 Quality 1067x600".
    [[nodiscard]] const std::string& activeDescription() const { return activeDescription_; }
    // Saves the next presented frame as a PNG (written after the GPU finished it).
    void requestScreenshot(std::string path) { screenshotPath_ = std::move(path); }
    void finishScreenshots();
    [[nodiscard]] f32 viewDistance() const { return settings_.viewDistance; }
    // Debug: logs the objects whose bounding spheres the ray through screen point (u, v ∈ [-1, 1]) hits.
    void pick(f32 u, f32 v) { pickRequest_ = true, pickU_ = u, pickV_ = v; }
    void toggleBackfaceCulling() { backfaceCulling_ = !backfaceCulling_; } // openBVE F11
    [[nodiscard]] bool backfaceCulling() const { return backfaceCulling_; }
    [[nodiscard]] const RenderStats& stats() const { return stats_; }
    // Main-thread frame timing: the game laps its own stages, render() laps the renderer's.
    [[nodiscard]] FrameProfiler& profiler() { return profiler_; }
    [[nodiscard]] const FrameProfiler& profiler() const { return profiler_; }
    [[nodiscard]] phx::Extent2D extent() const { return {swapchain_->extent.width, swapchain_->extent.height}; }
    [[nodiscard]] const std::string& gpuName() const { return ctx_->caps.deviceName; }

private:
    struct PartState {
        int state = 0;
        double timer = 0.0;
        std::array<double, 10> last{};          // per-function last results ("value")
        std::array<double, 3> angle{}, angVel{};
        bool init = false;
        glm::mat3 rot{1.0f};
        glm::vec3 offset{0.0f};
        glm::vec2 shift{0.0f};
    };
    struct AnimState {
        std::vector<PartState> parts;
        u64 lastFrame = 0;
    };
    struct Item {
        ObjectId object;
        const GpuMesh* mesh;
        GpuInstance instance;
        f32 distance;
        u32 order = 0; // first gather index of this object (draw order)
        u8 flags = 0;  // kItemView | kItemCaster
    };
    static constexpr u8 kItemView = 1, kItemCaster = 2;
    struct FrameData {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        std::array<VkSemaphore, 4> acquired{};
        phx::Buffer data;
        std::size_t materialCapacity = 0, instanceCapacity = 0;
    };

    void configure();          // targets + pipelines for the current mode / extent
    void destroyTargets();
    void destroyPipelines();
    void createPipelines();
    void recreateSwapchain();
    void ensureFrameCapacity(FrameData& f, std::size_t materials, std::size_t instances);
    void gatherRoute(const Simulation& sim, const CameraState& cam, ScriptContext& ctx, double dt);
    void gatherTrain(const Simulation& sim, const CameraState& cam, ScriptContext& ctx, double dt);
    void gatherTraffic(const Simulation& sim, ScriptContext& ctx, double dt);
    // Streaming priority of an object at `pos` that is not loaded yet: by distance to the
    // camera, one level lower outside the view frustum.
    [[nodiscard]] JobPriority streamPriority(const glm::dvec3& pos) const;
    void emitStatic(ObjectId id, const glm::dmat3& rot, const glm::dvec3& pos, f32 brightness, glm::vec2 shift = {});
    void emitAnimated(ObjectId id, const glm::dmat3& rot, const glm::dvec3& pos, AnimState& st, ScriptContext& ctx, double dt, f32 brightness);
    bool visible(const glm::dvec3& center, f32 radius) const;
    bool casterVisible(const glm::dvec3& center, f32 radius) const;
    void ensureShadowAtlas();

    RenderSettings settings_;
    ObjectLibrary& lib_;
    ThreadPool& pool_;
    phx::Window& window_;
    std::unique_ptr<phx::VulkanContext> ctx_;
    std::unique_ptr<phx::Swapchain> swapchain_;
    std::unique_ptr<phx::Bindless> bindless_;
    std::unique_ptr<phx::UploadQueue> upload_;
    std::unique_ptr<TextureCache> textures_;
    std::unique_ptr<MaterialTable> materials_;
    std::unique_ptr<MeshCache> meshes_;
    std::unique_ptr<phx::UiRenderer> ui_;
    std::unique_ptr<phx::Upscaler> upscaler_;
    std::unique_ptr<phx::AntiAliasing> aa_;
    std::unique_ptr<phx::Ssao> ssao_;
    std::unique_ptr<phx::Bloom> bloom_;
    std::unique_ptr<phx::GpuProfiler> gpuProfiler_;
    FrameProfiler profiler_;
    void steerPace(double waitedMs);
    static constexpr double kPaceMarginMs = 1.5;
    double paceDelayMs_ = 0.0; // pace(): sleep before the frame (vsync)
    int refreshHz_ = 0;
    static constexpr u32 kFrames = 2;
    std::array<FrameData, kFrames> frames_;
    u32 slot_ = 0;
    u64 frameNumber_ = 0;

    // configuration (configure())
    bool configDirty_ = true, swapchainDirty_ = false;
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
    VkSampleCountFlags supportedSamples_ = VK_SAMPLE_COUNT_1_BIT;
    phx::UpscaleMode upMode_ = phx::UpscaleMode::Native;
    VkExtent2D renderExt_{}, displayExt_{};
    bool motion_ = false;       // motion vector attachment (single-sample modes)
    bool resolves_ = false;     // output_ written by DLSS / AntiAliasing
    u32 fgCount_ = 0;           // generated frames per rendered frame
    u32 swapchainExtra_ = 0;
    f32 mipBias_ = 0.0f;
    std::string activeDescription_;
    phx::Image depthResolve_; // MSAA: sample 0 of the depth, for SSAO
    phx::Image sceneColor_, sceneResolve_, depth_, motionImg_, output_, final_, hudless_, uiImage_, fgOut_;
    static constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kMotionFormat = VK_FORMAT_R16G16_SFLOAT;
    static constexpr VkFormat kFinalFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat depthFormat_ = VK_FORMAT_D32_SFLOAT;
    std::array<VkPipeline, 2> opaque_{}, blend_{}; // [double sided]
    VkPipeline additive_ = VK_NULL_HANDLE, background_ = VK_NULL_HANDLE, tonemap_ = VK_NULL_HANDLE;
    VkPipeline shadowPipe_ = VK_NULL_HANDLE;
    // sun shadows
    static constexpr u32 kCascades = 4;
    phx::Image shadowAtlas_;
    u32 shadowSlot_ = 0, shadowAtlasSize_ = 0;
    bool shadowFresh_ = false; // atlas created, not yet in a sampled layout
    std::array<phx::ShadowCascade, kCascades> cascades_;
    u32 cascadeCount_ = 0; // this frame (0 = no shadows: off, or the sun is down)
    u8 emitMask_ = kItemView | kItemCaster;
    // temporal state
    bool havePrev_ = false, resetHistory_ = true;
    glm::dvec3 prevCamPos_{0.0}, origin_{0.0};
    glm::mat4 prevViewProj_{1.0f}, prevProj_{1.0f};
    glm::mat3 prevView_{1.0f};
    std::vector<glm::dvec3> prevCarPos_;
    struct TrafficState {
        std::vector<AnimState> cars;
        std::vector<glm::dvec3> prev;
    };
    std::vector<TrafficState> traffic_; // per Simulation::traffic entry
    glm::dvec3 objectMotion_{0.0}; // previous − current world position of the object being emitted

    // per-frame scene
    glm::dvec3 camPos_{0.0};
    std::array<glm::vec4, 5> planes_;
    std::vector<Item> items_;
    std::unordered_map<u32, AnimState> routeAnim_;
    std::vector<AnimState> carAnim_;
    AnimState cabAnim_;
    const Cab* cab_ = nullptr;
    std::vector<AnimState> cabBodyAnim_, cabElementAnim_;
    TextureId backgroundTexture_ = kNone;
    std::string backgroundPath_;
    std::vector<ObjectId> pinned_;
    std::string screenshotPath_, screenshotPending_;
    phx::Buffer screenshotBuffer_;
    VkExtent2D screenshotExtent_{};
    VkFence screenshotFence_ = VK_NULL_HANDLE;
    RenderStats stats_;
    bool backfaceCulling_ = true;
    bool frameCheck_ = std::getenv("OTS_FRAMECHECK") != nullptr;
    double checkMean_ = 0.0;
    int checkSaved_ = 0;
    bool pickRequest_ = false;
    f32 pickU_ = 0, pickV_ = 0;
};

} // namespace ots
