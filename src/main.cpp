// openTrainSim — openBVE-compatible train simulator on the phx Vulkan engine.
//   openTrainSim <route.csv> [--train <folder>] [--station <n>] [--config <file>] [--frames <n>]
#include "game/Game.hpp"
#include "phx/core/Log.hpp"
#include "phx/gpu/Resources.hpp"
#include "sim/Traffic.hpp"
#include "util/Platform.hpp"

#include "util/Text.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
// Written next to the executable on first run; every key is optional.
constexpr std::string_view kDefaultConfig = R"(; openTrainSim settings (INI). Delete a line to get its default back.

[content]
; route = /path/to/Railway/Route/route.csv   (used when no route is given on the command line)
; train = /path/to/Train/KTX                 (overrides the route's Train.Folder)
; Legacy code page for files that are not UTF-8 (CP949 Korean, SHIFT_JIS Japanese, ...)
codepage = CP949

[display]
width = 1600
height = 900
vsync = 1
; borderless fullscreen (toggle: Ctrl+F, openBVE MISC_FULLSCREEN)
fullscreen = 0
; start with a maximized window
maximized = 0
fov = 55
uiScale = 2
; TrueType UI font (.ttf/.otf/.ttc). Empty = system fonts (Noto Sans / DejaVu, then Noto Sans CJK KR / Malgun Gothic)
font =

[render]
; anti-aliasing / upscaling: off msaa2 msaa4 msaa8 fxaa taa ssaa
;   fsr-quality fsr-balanced fsr-performance fsr-ultra
;   dlaa dlss-quality dlss-balanced dlss-performance dlss-ultra (NVIDIA RTX; else FSR / TAA)
aa = msaa4
; sun shadows (cascaded shadow maps): on/off, texels per cascade, range (m), darkness 0..1
shadows = 1
shadowResolution = 2048
shadowDistance = 250
shadowStrength = 1
; the driver car body shades the cab: sunlight only through the windows
cabSunlight = 1
; ambient occlusion (darkens creases and contact areas): on/off, radius (m), strength
ssao = 1
ssaoRadius = 1.2
ssaoStrength = 1
; lighting: sky/ground ambient 0..1, sun highlight
hemisphereLight = 1
specular = 0.15
; output grade (0 = off): glow around bright areas, saturation (1 = unchanged), S-curve, vignette
bloom = 0.6
saturation = 1.1
contrast = 0.2
vignette = 0.12
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
; synthesised VVVF inverter sound (docs/vvvf.md). Empty = <train>/vvvf.json if present, none = off
; vvvf = /path/to/preset.json

[simulation]
; run the train's safety-system plugin from ats.cfg (managed openBVE plugins, via Mono)
plugins = 1
; train protection: auto (built-in Korean ATS/TVM430/ATP when the train has safety.json, else the plugin), builtin, plugin
safety = auto
; AI trains: Route.RunInterval and <route>.traffic.json (docs/traffic.md)
traffic = 1

[terrain]
; DEM terrain from <route>.terrain.json (docs/terrain.md)
enabled = 1

[controls]
; openBVE control file. Empty = your openBVE controls.cfg if found, else openBVE defaults.
; file = /home/me/.config/OpenBve/Settings/1.5.0/controls.cfg
mouseSensitivity = 0.003

[game]
timeFactor = 1
showStats = 0
showHelp = 1
)";

std::filesystem::path exeDir() { return ots::executableDir(); }
} // namespace

int main(int argc, char** argv) {
    ots::initConsole();
    ots::LaunchOptions opt;
    std::string aaOverride, fgOverride;
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    constexpr std::string_view usage = "usage: openTrainSim <route.csv> [--train <folder>] [--station <n>] [--time <HH:MM[:SS]>] [--safety <safety.json|none>] [--config <file>] [--frames <n>] "
                                       "[--cab <cab.json|none>] [--vvvf <preset.json|none>] [--traffic <file.json|none>] [--terrain <file.json|none>]\n";
    for (auto it = args.begin(); it != args.end(); ++it) {
        const std::string_view a = *it;
        auto value = [&]() -> std::string_view { return std::next(it) != args.end() ? *++it : std::string_view(); };
        if (a == "--train") opt.train = value();
        else if (a == "--station") {
            if (const auto n = ots::parseIntVb6(value()); n && *n >= 0) opt.station = std::size_t(*n);
        } else if (a == "--config") opt.configFile = value();
        else if (a == "--frames") opt.maxFrames = ots::parseIntVb6(value()).value_or(0);
        else if (a == "--screenshot") opt.screenshot = value();
        else if (a == "--camera") opt.camera = value();
        else if (a == "--autopilot") opt.autopilot = true;
        else if (a == "--fullscreen") opt.fullscreen = true;
        else if (a == "--maximized") opt.maximized = true;
        else if (a == "--cab") opt.cab = value();
        else if (a == "--cab-click") opt.cabClick = value();
        else if (a == "--vvvf") opt.vvvf = value();
        else if (a == "--traffic") opt.traffic = value();
        else if (a == "--terrain") opt.terrain = value();
        else if (a == "--aa") aaOverride = value();
        else if (a == "--fg") fgOverride = value();
        else if (a == "--pick") opt.pick = value();
        else if (a == "--timefactor") opt.timeFactor = ots::parseDouble(value()).value_or(0.0);
        else if (a == "--safety") opt.safety = value();
        else if (a == "--time") {
            const std::string_view v = value();
            if (!(opt.startTime = ots::parseClock(v))) return std::cerr << "--time: expected HH:MM[:SS], got '" << v << "'\n", 1;
        }
        else if (a == "--help" || a == "-h") {
            std::cout << usage;
            return 0;
        } else opt.route = a;
    }
    // shaders next to the executable when deployed, else the build tree (PHX_SHADER_DIR)
    const std::filesystem::path deployed = exeDir() / "shaders";
    if (std::filesystem::exists(deployed / "mesh.vert.spv")) phx::setShaderDirectory(deployed.string());

    ots::Config cfg;
    if (opt.configFile.empty()) opt.configFile = exeDir() / "openTrainSim.cfg";
    if (!cfg.load(opt.configFile)) {
        if (std::ofstream f(opt.configFile, std::ios::binary); f << kDefaultConfig) {
            f.close();
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
