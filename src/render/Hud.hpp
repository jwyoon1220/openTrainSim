#pragma once
// Driving overlay drawn with the engine's UI painter: clock, speed / limit, handles, next
// station, signal ahead, the profiler box in the top-right corner (frame-time graph, CPU
// stages, GPU passes, streaming queue) and optional renderer statistics (VRAM, draws, streaming).
#include "render/Renderer.hpp"

#include "phx/ui/Ui.hpp"

namespace ots {

struct HudOptions {
    bool visible = true;
    bool stats = false;
    bool profiler = true;                    // box in the top-right corner
    const FrameProfiler* frames = nullptr;   // its CPU timings
    bool help = true;
    f32 fps = 0.0f;
    std::string cameraName;
    std::string message; // transient message (e.g. "Paused")
    std::string audio;   // sound statistics line (stats panel)
    std::string tooltip; // cab element under the mouse
    f32 tooltipX = 0, tooltipY = 0;
};

void buildHud(std::vector<phx::ui::Vertex>& out, const Simulation& sim, const RenderStats& stats, const HudOptions& opt, f32 width, f32 height,
              const phx::ui::Theme& theme);

std::string displayName(const std::string& stationName); // "seoul[서울역]" → "서울역" (or "seoul" without a Hangul font)

} // namespace ots
