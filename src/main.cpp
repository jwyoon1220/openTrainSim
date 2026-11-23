// openTrainSim — openBVE-compatible train simulator on the phx Vulkan engine.
//   openTrainSim <route.csv> [--train <folder>] [--station <n>] [--config <file>] [--frames <n>]
#include "game/Game.hpp"
#include "phx/core/Log.hpp"
#include "phx/gpu/Resources.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <unistd.h>

namespace {
// Written next to the executable on first run; every key is optional.
constexpr const char* kDefaultConfig = R"(; openTrainSim settings (INI). Delete a line to get its default back.

[content]
; route = /path/to/Railway/Route/route.csv   (used when no route is given on the command line)
; train = /path/to/Train/KTX                 (overrides the route's Train.Folder)
; Legacy code page for files that are not UTF-8 (CP949 Korean, SHIFT_JIS Japanese, ...)
codepage = CP949

[display]
width = 1600
height = 900
vsync = 1
fov = 55
uiScale = 2

[render]
; anti-aliasing / upscaling: off msaa2 msaa4 msaa8 fxaa taa ssaa
;   fsr-quality fsr-balanced fsr-performance fsr-ultra
;   dlaa dlss-quality dlss-balanced dlss-performance dlss-ultra (NVIDIA RTX; else FSR / TAA)
aa = msaa4
; DLSS frame generation: generated frames per rendered frame (0 = off, 1-3), single-sample modes
frameGeneration = 0
; metres; objects farther away are neither drawn nor kept in memory
viewDistance = 1500
; textures larger than this are downscaled (VRAM)
textureMaxSize = 2048
; BC1/BC3 block compression: 4-8x less texture VRAM
textureCompression = 1
; geometry heaps (MB), shared by every object; LRU-evicted when full
vertexHeapMB = 64
indexHeapMB = 16
; seconds an unused object stays resident
keepSeconds = 30
; upload budget per frame (MB): spreads loading over frames
uploadMBPerFrame = 48
; loader threads (0 = cores - 2)
workers = 0
validation = 0

[sound]
enabled = 1
; master volume (1 = openBVE level)
volume = 1
; real OpenAL voices; quieter sounds keep playing virtually
voices = 64

[simulation]
; run the train's safety-system plugin from ats.cfg (managed openBVE plugins, via Mono)
plugins = 1

[controls]
; openBVE control file. Empty = your openBVE controls.cfg if found, else openBVE defaults.
; file = /home/me/.config/OpenBve/Settings/1.5.0/controls.cfg
mouseSensitivity = 0.003

[game]
timeFactor = 1
showStats = 0
showHelp = 1
)";

std::filesystem::path exeDir() {
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return std::filesystem::current_path();
    buf[n] = 0;
    return std::filesystem::path(buf).parent_path();
}
} // namespace

int main(int argc, char** argv) {
    ots::LaunchOptions opt;
    std::string aaOverride, fgOverride;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (!std::strcmp(a, "--train")) opt.train = next();
        else if (!std::strcmp(a, "--station")) opt.station = std::atol(next());
        else if (!std::strcmp(a, "--config")) opt.configFile = next();
        else if (!std::strcmp(a, "--frames")) opt.maxFrames = std::atol(next());
        else if (!std::strcmp(a, "--screenshot")) opt.screenshot = next();
        else if (!std::strcmp(a, "--camera")) opt.camera = next();
        else if (!std::strcmp(a, "--autopilot")) opt.autopilot = true;
        else if (!std::strcmp(a, "--aa")) aaOverride = next();
        else if (!std::strcmp(a, "--fg")) fgOverride = next();
        else if (!std::strcmp(a, "--pick")) opt.pick = next();
        else if (!std::strcmp(a, "--timefactor")) opt.timeFactor = std::atof(next());
        else if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) {
            std::printf("usage: %s <route.csv> [--train <folder>] [--station <n>] [--config <file>] [--frames <n>]\n", argv[0]);
            return 0;
        } else opt.route = a;
    }
    // shaders next to the executable when deployed, else the build tree (PHX_SHADER_DIR)
    const std::filesystem::path deployed = exeDir() / "shaders";
    if (std::filesystem::exists(deployed / "mesh.vert.spv")) phx::setShaderDirectory(deployed.string());

    ots::Config cfg;
    if (opt.configFile.empty()) opt.configFile = exeDir() / "openTrainSim.cfg";
    if (!cfg.load(opt.configFile)) {
        if (FILE* f = std::fopen(opt.configFile.c_str(), "wb")) {
            std::fputs(kDefaultConfig, f);
            std::fclose(f);
            phx::log::info("Wrote default settings to {}", opt.configFile.string());
            cfg.load(opt.configFile);
        }
    }
    if (!aaOverride.empty()) cfg.set("render", "aa", aaOverride); // e.g. dlss-quality, fsr-performance, taa
    if (!fgOverride.empty()) cfg.set("render", "frameGeneration", fgOverride);
    if (opt.route.empty()) opt.route = cfg.get("content", "route");
    if (opt.route.empty()) {
        phx::log::error("No route given. Usage: openTrainSim <route.csv> [--train <folder>]  (or set [content] route = ... in {})",
                        opt.configFile.string());
        return 1;
    }
    if (opt.train.empty()) opt.train = cfg.get("content", "train");
    return ots::runGame(opt, cfg);
}
