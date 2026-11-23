#pragma once
// Temporal upscaling / anti-aliasing (plan §2.8): NVIDIA DLSS Super Resolution and DLAA
// through the NGX SDK (Linux path: NGX directly; Streamline is Windows-only).
//  * "Highest accessible version": the newest DLSS runtime the build ships (SDK
//    PHX_DLSS_VERSION) with the Default render preset, so the driver's/OTA's newest
//    model (transformer presets today) is used; availability is queried at run time
//    and the engine falls back to native rendering.
//  * Inputs: pre-exposed HDR colour, reverse-Z depth and camera motion vectors at render
//    resolution, sub-pixel jitter (Halton 2,3). Output: HDR at display resolution.
#include "phx/core/Types.hpp"
#include "phx/gpu/Resources.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <vector>

namespace phx {
class VulkanContext;

// Anti-aliasing / upscaling modes. DLSS modes need an NVIDIA RTX GPU; the others run on any GPU.
enum class UpscaleMode : u8 {
    Native = 0, DLAA, Quality, Balanced, Performance, UltraPerformance, // 1–5: DLSS
    FXAA, TAA, SSAA,                                                     // anti-aliasing at output resolution
    FsrQuality, FsrBalanced, FsrPerformance, FsrUltraPerformance,        // AMD FSR 1 (after TAA)
    Count
};
const char* upscaleModeName(UpscaleMode m);
bool parseUpscaleMode(const char* text, UpscaleMode& out);
inline bool isDlss(UpscaleMode m) { return m >= UpscaleMode::DLAA && m <= UpscaleMode::UltraPerformance; }
inline bool isFsr(UpscaleMode m) { return m >= UpscaleMode::FsrQuality && m <= UpscaleMode::FsrUltraPerformance; }
inline bool isTemporalAA(UpscaleMode m) { return m == UpscaleMode::TAA || isFsr(m); } // engine TAA (+ FSR)
// Render-resolution divisor of the FSR modes (AMD's presets).
inline f32 fsrScale(UpscaleMode m) {
    switch (m) {
    case UpscaleMode::FsrQuality: return 1.5f;
    case UpscaleMode::FsrBalanced: return 1.7f;
    case UpscaleMode::FsrPerformance: return 2.0f;
    case UpscaleMode::FsrUltraPerformance: return 3.0f;
    default: return 1.0f;
    }
}
inline constexpr f32 kSsaaScale = 1.5f; // per axis (2.25× the pixels)
// Equivalent mode for GPUs without DLSS (DLSS → FSR with the same render scale, DLAA → TAA).
UpscaleMode fallbackWithoutDlss(UpscaleMode m);
// Set once NGX was probed (render thread); read by the supervisor to skip DLSS modes.
bool dlssAvailableHint();
u32 frameGenMaxHint(); // generated frames per rendered frame the GPU supports (0 = no DLSS-G)

struct UpscaleInputs {
    const Image* color = nullptr;  // render resolution, SHADER_READ_ONLY_OPTIMAL
    const Image* depth = nullptr;  // render resolution, D32 reverse-Z
    const Image* motion = nullptr; // render resolution, xy = previous − current (UV units)
    const Image* output = nullptr; // display resolution, GENERAL, storage
    VkFormat colorFormat = VK_FORMAT_UNDEFINED, depthFormat = VK_FORMAT_UNDEFINED, motionFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D render{}, display{};
    glm::vec2 jitterPx{0.0f}; // sample offset from the pixel centre, render pixels (+x right, +y down)
    bool reset = false;
    f32 frameMs = 16.7f;
};

// DLSS Frame Generation inputs (display-ready colour after tonemapping + UI).
struct FrameGenInputs {
    const Image* backbuffer = nullptr; // final colour incl. UI (output resolution, GENERAL)
    const Image* hudless = nullptr;    // final colour without UI
    const Image* ui = nullptr;         // premultiplied UI RGBA (0 where no UI)
    const Image* depth = nullptr;      // render resolution, reverse-Z
    const Image* motion = nullptr;     // render resolution, UV units, previous − current
    const Image* output = nullptr;     // receives the generated frame (output resolution)
    VkFormat colorFormat = VK_FORMAT_UNDEFINED, depthFormat = VK_FORMAT_UNDEFINED, motionFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D render{}, display{};
    // Unjittered camera, GLM convention (column vectors); converted for NGX internally.
    glm::mat4 viewToClip{1.0f}, clipToView{1.0f}, clipToPrevClip{1.0f}, prevClipToClip{1.0f};
    glm::vec3 cameraPos{0.0f}, cameraUp{0, 1, 0}, cameraRight{1, 0, 0}, cameraFwd{0, 0, -1};
    f32 nearZ = 0.1f, farZ = 1.0e5f, fovY = 1.0f, aspect = 1.0f;
    bool reset = false;
    u32 count = 1, index = 1; // generated frames per rendered frame, this frame's index (1-based)
};

class Upscaler {
public:
    // Extensions NGX needs, to be enabled when the instance / device are created.
    static void instanceExtensions(std::vector<const char*>& out);
    static void deviceExtensions(std::vector<const char*>& out);

    explicit Upscaler(VulkanContext& ctx);
    ~Upscaler();
    Upscaler(const Upscaler&) = delete;
    Upscaler& operator=(const Upscaler&) = delete;

    [[nodiscard]] bool dlssAvailable() const { return available_; }
    [[nodiscard]] const std::string& status() const { return status_; }

    // Render resolution for a mode (NGX optimal settings); display for Native.
    [[nodiscard]] VkExtent2D renderExtent(UpscaleMode mode, VkExtent2D display) const;

    // Records DLSS into `cmd` (creates the feature for this mode/resolution on first use).
    // Returns false if DLSS could not run (caller should then present the input directly).
    bool evaluate(VkCommandBuffer cmd, UpscaleMode mode, const UpscaleInputs& in);
    // Drops the feature; call with the GPU idle (resize, mode change).
    void releaseFeature();

    // DLSS Frame Generation (DLSS-G / multi frame generation).
    [[nodiscard]] bool frameGenAvailable() const { return fgAvailable_; }
    [[nodiscard]] u32 frameGenMaxGenerated() const { return fgMaxGenerated_; } // 1 = 2x … 5 = 6x
    bool evaluateFrameGen(VkCommandBuffer cmd, const FrameGenInputs& in);
    void releaseFrameGen();

private:
    struct Ngx;
    VulkanContext& ctx_;
    std::unique_ptr<Ngx> ngx_;
    bool available_ = false;
    bool fgAvailable_ = false;
    u32 fgMaxGenerated_ = 0;
    std::string status_ = "DLSS not compiled in";
};

// Halton(2,3) sub-pixel jitter in [-0.5, 0.5]² for a frame; the phase count grows with
// the upscale factor (≥ 8 samples per output pixel).
glm::vec2 jitterHalton(u64 frame, VkExtent2D render, VkExtent2D display);

} // namespace phx
