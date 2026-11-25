#include "game/Game.hpp"

#include "audio/SoundManager.hpp"
#include "bve/Route.hpp"
#include "bve/Train.hpp"
#include "cab/Cab.hpp"
#include "phx/core/Log.hpp"
#include "phx/platform/UiInput.hpp"
#include "phx/platform/Window.hpp"
#include "phx/ui/Font.hpp"
#include "phx/ui/Ui.hpp"
#include "render/Hud.hpp"
#include "render/Renderer.hpp"
#include "route/NativeRoute.hpp"
#include "sim/Traffic.hpp"
#include "terrain/Terrain.hpp"
#include "sim/World.hpp"
#include "util/Paths.hpp"
#include "util/Text.hpp"
#include "util/ThreadPool.hpp"

#include <GLFW/glfw3.h>

#include <array>
#include <deque>
#include <map>
#include <cstdlib>
#include <optional>
#include <cstring>
#include <charconv>
#include <chrono>
#include <cmath>
#include <format>
#include <thread>

namespace ots {
namespace {

enum class CamMode { Interior, Exterior, Track, Free };
std::string_view camName(CamMode m) {
    switch (m) {
    case CamMode::Interior: return "cab";
    case CamMode::Exterior: return "exterior";
    case CamMode::Track: return "track";
    case CamMode::Free: return "free";
    }
    return "";
}

struct CameraControl {
    CamMode mode = CamMode::Interior;
    double yaw = 0.0, pitch = 0.0;         // interior head
    glm::dvec3 offset{0.0};                // interior head offset (car frame)
    double fovDeg = 55.0;
    double extYaw = 2.6, extPitch = 0.25, extDist = 45.0;
    double trackCam = -1.0;
    glm::dvec3 freePos{0.0};
    double freeYaw = 0.0, freePitch = 0.0, freeSpeed = 30.0;
    double freeTrackGuess = 0.0;
};

glm::dvec3 lookDir(const glm::dvec3& X, const glm::dvec3& Y, const glm::dvec3& Z, double yaw, double pitch) {
    return glm::normalize(std::cos(pitch) * (std::sin(yaw) * X + std::cos(yaw) * Z) + std::sin(pitch) * Y);
}

void basisFrom(const glm::dvec3& dir, const glm::dvec3& up, CameraState& c) {
    c.Z = glm::normalize(dir);
    glm::dvec3 x = glm::cross(up, c.Z);
    if (glm::length(x) < 1e-6) x = glm::cross(glm::dvec3(0, 0, 1), c.Z);
    c.X = glm::normalize(x);
    c.Y = glm::cross(c.Z, c.X);
}

double nearestTrackPosition(const Route& route, const glm::dvec3& p, double guess) {
    if (route.blocks.empty()) return 0.0;
    const int g = route.blockAt(guess);
    int best = g;
    double bestD = 1e300;
    for (int b = std::max(0, g - 400); b < std::min(int(route.blocks.size()), g + 400); ++b) {
        const double d = glm::length(route.blocks[std::size_t(b)].origin - p);
        if (d < bestD) bestD = d, best = b;
    }
    const RouteBlock& blk = route.blocks[std::size_t(best)];
    const glm::dvec3 dir(blk.startDir.x, 0.0, blk.startDir.y);
    const double t = std::clamp(glm::dot(p - blk.origin, dir), -route.blockLength, 2.0 * route.blockLength);
    return std::clamp(best * route.blockLength + t, 0.0, route.endPosition() - 1.0);
}

// Ray through framebuffer pixel (x, y) in the driver car's coordinates.
void cabRay(const CameraState& cam, const CarPose& car, double x, double y, Extent2D fb, glm::dvec3& o, glm::dvec3& d) {
    const double aspect = double(fb.width) / double(std::max(1u, fb.height));
    const double u = 2.0 * x / double(std::max(1u, fb.width)) - 1.0, v = 1.0 - 2.0 * y / double(std::max(1u, fb.height));
    const double ty = std::tan(0.5 * double(cam.fovY));
    const glm::dvec3 w = glm::normalize(cam.Z + cam.X * (u * ty * aspect) + cam.Y * (v * ty));
    const glm::dvec3 rel = cam.position - car.position;
    o = {glm::dot(rel, car.X), glm::dot(rel, car.Y), glm::dot(rel, car.Z)};
    d = {glm::dot(w, car.X), glm::dot(w, car.Y), glm::dot(w, car.Z)};
}

// A content file given on the command line ("none" = off), else the one next to the route.
std::filesystem::path contentFile(const std::filesystem::path& given, const std::filesystem::path& route,
                                  std::filesystem::path (*besideRoute)(const std::filesystem::path&)) {
    if (given == "none") return {};
    return given.empty() ? besideRoute(route) : given;
}

// "1.5,-2,x" → {1.5, -2, nullopt}
std::vector<std::optional<double>> numberList(std::string_view csv) {
    std::vector<std::optional<double>> out;
    for (std::string_view part : split(csv, ',')) out.push_back(parseDouble(part));
    return out;
}

std::optional<double> envNumber(const char* name) {
    const char* v = std::getenv(name);
    return v ? parseDouble(v) : std::nullopt;
}

void addTerrain(Route& route, ObjectLibrary& lib, const std::filesystem::path& file) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto settings = loadTerrainSettings(file, route);
    const auto built = settings ? buildTerrain(route, lib, *settings, file.parent_path()) : Expected<TerrainStats>(Unexpected(settings.error()));
    if (!built) {
        log::error("Terrain {}: {}", pathString(file), built.error());
        return;
    }
    const TerrainStats& st = *built;
    log::info("Terrain: {} tiles, {} generated tunnels ({:.1f} km), route tunnels {:.1f} km, {} bridges, DEM {} tiles ({} downloaded, {} missing), {:.1f} s",
              st.tiles, st.tunnels, st.tunnelLength / 1000.0, st.routeTunnelLength / 1000.0, st.bridges, st.demTiles, st.demDownloaded, st.demMissing,
              std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
}

// <route>.traffic.json (if any) plus the route's Route.RunInterval trains.
TrafficConfig loadTraffic(const std::filesystem::path& file, const Route& route) {
    TrafficConfig tc;
    if (!file.empty()) {
        if (auto loaded = loadTrafficConfig(file)) {
            tc = std::move(*loaded);
            log::info("Traffic: {} ({} trains)", pathString(file), tc.trains.size());
        } else {
            log::error("Traffic {}: {}", pathString(file), loaded.error());
        }
    }
    if (tc.routeIntervals)
        for (std::size_t i = 0; i < route.runIntervals.size(); ++i) {
            TrafficEntry e;
            e.options.interval = route.runIntervals[i];
            e.options.name = std::format("RunInterval {}", i + 1);
            tc.trains.push_back(std::move(e));
        }
    return tc;
}

// Train types of the AI trains: the player's, or others loaded once per folder (stable
// addresses: TrainSim keeps a reference).
class TrafficFleet {
public:
    TrafficFleet(ObjectLibrary& lib, const TrainDef& player, std::filesystem::path playerFolder)
        : lib_(lib), player_(player), playerFolder_(std::move(playerFolder)) {}
    const TrainDef* get(const std::filesystem::path& folder, Renderer& renderer) {
        if (folder.empty() || folder == playerFolder_) return &player_;
        if (const auto it = byFolder_.find(folder); it != byFolder_.end()) return it->second;
        auto loaded = loadTrain(folder, lib_);
        if (!loaded) {
            log::error("Traffic train {}: {}", pathString(folder), loaded.error());
            return byFolder_[folder] = nullptr;
        }
        TrainDef& def = defs_.emplace_back(std::move(*loaded));
        renderer.pinTrafficTrain(def);
        return byFolder_[folder] = &def;
    }

private:
    ObjectLibrary& lib_;
    const TrainDef& player_;
    std::filesystem::path playerFolder_;
    std::deque<TrainDef> defs_;
    std::map<std::filesystem::path, const TrainDef*> byFolder_;
};

} // namespace

int runGame(const LaunchOptions& opt, Config& cfg) {
    setDefaultCodepage(cfg.get("content", "codepage", "CP949"));
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    const int workersCfg = cfg.getInt("render", "workers", 0);
    ThreadPool pool(workersCfg > 0 ? static_cast<unsigned>(workersCfg) : std::max(1u, hw - 2));
    ObjectLibrary lib;

    // ---------------------------------------------------------------- route + train
    log::info("Loading route {}", opt.route.string());
    auto loadedRoute = loadAnyRoute(opt.route, lib);
    if (!loadedRoute) {
        log::error("{}", loadedRoute.error());
        return 1;
    }
    Route& route = loadedRoute->route; // stable: the simulation and renderer keep references
    const RouteLoadStats& rs = loadedRoute->stats;
    log::info("Route: {:.1f} km, {} placements, {} objects, {} stations ({:.2f} s)", route.endPosition() / 1000.0, rs.placements, lib.size(),
                   route.stations.size(), rs.parseSeconds);
    // ---------------------------------------------------------------- terrain (DEM)
    if (cfg.getBool("terrain", "enabled", true))
        if (const auto file = contentFile(opt.terrain, opt.route, findTerrainFile); !file.empty()) addTerrain(route, lib, file);

    std::filesystem::path trainFolder = opt.train;
    if (trainFolder.empty()) trainFolder = findTrainFolder(route.file, route.trainFolderHint, route.trainFolder);
    if (trainFolder.empty()) {
        log::error("Train folder '{}' not found (use --train <folder>)", route.trainFolder);
        return 1;
    }
    auto loadedTrain = loadTrain(trainFolder, lib);
    if (!loadedTrain) {
        log::error("{}", loadedTrain.error());
        return 1;
    }
    const TrainDef& train = *loadedTrain; // stable: TrainSim keeps a reference
    for (const auto& w : train.warnings) log::warn("{}", w);
    log::info("Train {}: {} cars, {:.0f} m, {:.0f} t, cab: {}", train.name, train.cars.size(), train.length(), train.totalMass() / 1000.0,
                   train.cab != kNoObject ? "3D (panel.animated)" : (train.hasPanel2D ? "2D panel (not rendered yet)" : "none"));

    // ---------------------------------------------------------------- controls
    Controls controls;
    controls.loadDefaults();
    std::filesystem::path controlsFile = cfg.get("controls", "file");
    if (controlsFile.empty()) controlsFile = Controls::findOpenBveControls();
    if (!controlsFile.empty()) {
        if (controls.loadFile(controlsFile)) log::info("Controls: {}", controlsFile.string());
        else log::warn("Controls file {} unreadable, using openBVE defaults", controlsFile.string());
    } else {
        log::info("Controls: openBVE defaults");
    }
    // route / train specific additions (e.g. extra safety-system keys) on top of the above
    for (const auto& f : Controls::findContentControls(trainFolder, opt.route))
        if (const int n = controls.mergeFile(f)) log::info("Controls: +{} bindings from {}", n, f.string());

    // ---------------------------------------------------------------- window + renderer
    const u32 width = u32(cfg.getInt("display", "width", 1600)), height = u32(cfg.getInt("display", "height", 900));
    Window window(width, height, "openTrainSim");
    window.setTitle(std::format("openTrainSim — {} / {}", opt.route.filename().string(), train.name));
    if (opt.fullscreen || cfg.getBool("display", "fullscreen", false)) window.setFullscreen(true);
    else if (opt.maximized || cfg.getBool("display", "maximized", false)) window.maximize();
    RenderSettings rset;
    {   // anti-aliasing / upscaling (older configs: [display] msaa = N)
        const int msaa = cfg.getInt("display", "msaa", 4);
        rset.aa = msaa >= 8 ? AaMode::Msaa8 : msaa >= 4 ? AaMode::Msaa4 : msaa >= 2 ? AaMode::Msaa2 : AaMode::Off;
        rset.aa = parseAaMode(cfg.get("render", "aa")).value_or(rset.aa);
        rset.frameGen = u32(std::clamp(cfg.getInt("render", "frameGeneration", 0), 0, 3));
    }
    rset.vsync = cfg.getBool("display", "vsync", true);
    rset.validation = cfg.getBool("render", "validation", false);
    rset.viewDistance = float(cfg.getNumber("render", "viewDistance", 1500.0));
    rset.uploadMBPerFrame = u32(cfg.getInt("render", "uploadMBPerFrame", 48));
    rset.textures.maxSize = u32(cfg.getInt("render", "textureMaxSize", 2048));
    rset.textures.compress = cfg.getBool("render", "textureCompression", true);
    rset.geometry.vertexHeapMB = u32(cfg.getInt("render", "vertexHeapMB", 64));
    rset.geometry.indexHeapMB = u32(cfg.getInt("render", "indexHeapMB", 16));
    rset.geometry.keepSeconds = float(cfg.getNumber("render", "keepSeconds", 30.0));
    rset.shadows = cfg.getBool("render", "shadows", true);
    rset.shadowResolution = u32(std::clamp(cfg.getInt("render", "shadowResolution", 2048), 512, 4096));
    rset.shadowDistance = float(std::clamp(cfg.getNumber("render", "shadowDistance", 250.0), 20.0, 2000.0));
    rset.shadowStrength = float(std::clamp(cfg.getNumber("render", "shadowStrength", 1.0), 0.0, 1.0));
    rset.cabSunlight = cfg.getBool("render", "cabSunlight", true);
    rset.ssao = cfg.getBool("render", "ssao", true);
    rset.ssaoRadius = float(std::clamp(cfg.getNumber("render", "ssaoRadius", 1.2), 0.1, 10.0));
    rset.ssaoStrength = float(std::clamp(cfg.getNumber("render", "ssaoStrength", 1.0), 0.0, 4.0));
    rset.hemisphere = float(std::clamp(cfg.getNumber("render", "hemisphereLight", 1.0), 0.0, 1.0));
    rset.specular = float(std::clamp(cfg.getNumber("render", "specular", 0.15), 0.0, 2.0));
    rset.bloom = float(std::clamp(cfg.getNumber("render", "bloom", 0.6), 0.0, 4.0));
    rset.saturation = float(std::clamp(cfg.getNumber("render", "saturation", 1.1), 0.0, 2.0));
    rset.contrast = float(std::clamp(cfg.getNumber("render", "contrast", 0.2), 0.0, 1.0));
    rset.vignette = float(std::clamp(cfg.getNumber("render", "vignette", 0.12), 0.0, 1.0));
    Renderer renderer(window, rset, lib, pool);
    renderer.pinTrain(train);

    TrafficFleet fleet(lib, train, trainFolder); // AI train types; outlives `sim` (TrainSim keeps a reference)
    Simulation sim(route, train);

    // ---------------------------------------------------------------- AI traffic
    if (cfg.getBool("simulation", "traffic", true)) {
        for (const TrafficEntry& e : loadTraffic(contentFile(opt.traffic, opt.route, findTrafficFile), route).trains)
            if (const TrainDef* def = fleet.get(e.trainFolder, renderer)) sim.addTraffic(*def, e.options); // else: logged, skipped
    }
    if (opt.startTime) route.startTime = opt.startTime; // before start(): AI trains are placed for this time
    sim.start(opt.station);
    if (const auto at = envNumber("OTS_START_TRACK")) sim.train->placeAt(*at); // debugging: jump to a track position
    // train protection: built-in Korean devices (<train>/safety.json) or the openBVE plugin
    // ([simulation] safety = auto | builtin | plugin; auto: built-in when safety.json exists)
    std::string safetyMode = cfg.get("simulation", "safety", "auto");
    std::filesystem::path safetyFile = resolvePath(trainFolder, "safety.json");
    if (opt.safety == "none") safetyMode = "plugin";
    else if (!opt.safety.empty()) safetyMode = "builtin", safetyFile = opt.safety;
    if (safetyMode == "builtin" || (safetyMode == "auto" && !safetyFile.empty())) {
        auto config = safetyFile.empty() ? Expected<safety::SafetyConfig>(safety::defaultConfig()) : safety::loadConfig(safetyFile);
        if (!config) {
            log::error("{}: {}", safetyFile.string(), config.error());
            config = safety::defaultConfig();
        }
        sim.onboard = std::make_unique<safety::Onboard>(*config);
        std::string names;
        for (safety::System s : config->systems) names += std::string(names.empty() ? "" : " + ") + std::string(safety::label(s));
        sim.pluginStatus = "built-in " + names;
        log::info("Train protection: built-in {}", names);
    } else if (cfg.getBool("simulation", "plugins", true)) {
        if (sim.startPlugin(pluginHostPath())) log::info("Plugin: {}", sim.pluginStatus);
        else log::warn("Plugin: {}", sim.pluginStatus);
    } else {
        sim.pluginStatus = "plugins disabled ([simulation] plugins = false)";
    }

    // ---------------------------------------------------------------- 3D cab (cab.json)
    std::unique_ptr<Cab> cab;
    {
        std::filesystem::path cabFile = opt.cab;
        if (cabFile.empty() && cfg.getBool("simulation", "cab3d", true)) cabFile = resolvePath(trainFolder, "cab.json");
        if (!cabFile.empty() && cabFile != "none") {
            if (auto loaded = Cab::load(cabFile, lib, sim)) {
                cab = std::move(*loaded);
                renderer.setCab(cab.get());
                log::info("Cab: {} ({}, {} elements)", cab->name(), pathString(cabFile), cab->elementCount());
            } else {
                log::error("Cab {}: {} (using the openBVE cab)", pathString(cabFile), loaded.error());
            }
        }
    }

    // ---------------------------------------------------------------- sound
    SoundManager sound;
    sound.start({.enabled = cfg.getBool("sound", "enabled", true) && (opt.maxFrames == 0 || std::getenv("OTS_SOUND")),
                 .voices = cfg.getInt("sound", "voices", 64),
                 .volume = cfg.getNumber("sound", "volume", 1.0),
                 .vvvf = !opt.vvvf.empty() ? opt.vvvf : std::filesystem::path(cfg.get("sound", "vvvf"))},
                sim);
    sim.timeFactor = opt.timeFactor > 0 ? opt.timeFactor : cfg.getNumber("game", "timeFactor", 1.0);

    CameraControl cc;
    cc.fovDeg = cfg.getNumber("display", "fov", 55.0);
    double mouseSens = cfg.getNumber("controls", "mouseSensitivity", 0.003);

    // ---------------------------------------------------------------- UI
    ui::Theme theme;
    theme.textScale = float(cfg.getNumber("display", "uiScale", 2.0));
    // TrueType UI font: the configured file first, then system faces (Latin + Korean) as fallbacks
    if (const std::string font = cfg.get("display", "font", ""); !font.empty() && !ui::fonts().addFace(font))
        log::warn("UI font not readable: {}", font);
    ui::fonts().addSystemFaces();
    if (!ui::fonts().available()) log::warn("No TrueType font found: using the built-in bitmap font (ASCII only)");
    ui::Ui gui(theme);
    HudOptions hudOpt;
    hudOpt.stats = cfg.getBool("game", "showStats", false);
    hudOpt.profiler = cfg.getBool("game", "showProfiler", true);
    hudOpt.frames = &renderer.profiler();
    hudOpt.help = cfg.getBool("game", "showHelp", true);
    ui::Window& settings = gui.addWindow("Settings");
    settings.setClosable(true);
    ui::Label* activeLabel = nullptr;
    {
        auto& tabs = settings.content().add<ui::TabbedPane>();
        // --- graphics
        auto& gfx = tabs.addTab("Graphics");
        gfx.setLayout(std::make_unique<ui::FormLayout>(12, 6));
        std::vector<std::string> aaNames;
        for (int i = 0; i < static_cast<int>(AaMode::Count); ++i) aaNames.emplace_back(aaModeName(AaMode(i)));
        gfx.add<ui::Label>("Anti-aliasing");
        auto& aaBox = gfx.add<ui::ComboBox>(aaNames);
        aaBox.setSelectedIndex(static_cast<int>(renderer.aaMode()));
        for (int i = 0; i < static_cast<int>(AaMode::Count); ++i)
            if (isDlssMode(AaMode(i)) && !renderer.dlssAvailable()) aaBox.setItemEnabled(i, false);
        aaBox.addActionListener([&](int i) {
            renderer.setAaMode(AaMode(i));
            cfg.set("render", "aa", std::string(aaModeKey(AaMode(i))));
        });
        gfx.add<ui::Label>("Frame generation");
        auto& fgBox = gfx.add<ui::ComboBox>(std::vector<std::string>{"Off", "DLSS-G 2x", "DLSS-G 3x", "DLSS-G 4x"});
        fgBox.setSelectedIndex(int(renderer.frameGen()));
        for (int i = 1; i < 4; ++i)
            if (u32(i) > renderer.frameGenMax()) fgBox.setItemEnabled(i, false);
        fgBox.addActionListener([&](int i) {
            renderer.setFrameGen(u32(i));
            cfg.set("render", "frameGeneration", std::to_string(i));
        });
        gfx.add<ui::Label>("VSync");
        auto& vs = gfx.add<ui::CheckBox>("", renderer.vsync());
        vs.addItemListener([&](bool b) {
            renderer.setVsync(b);
            cfg.set("display", "vsync", b ? "1" : "0");
        });
        gfx.add<ui::Label>("View distance");
        auto& vd = gfx.add<ui::Slider>(300.0f, 6000.0f, rset.viewDistance, 100.0f);
        vd.setValueFormatter([](f32 v) { return std::format("{:.0f} m", v); });
        vd.addChangeListener([&](f32 v) {
            renderer.setViewDistance(v);
            cfg.setNumber("render", "viewDistance", v);
        });
        gfx.add<ui::Label>("Sun shadows");
        auto& shBox = gfx.add<ui::CheckBox>("", rset.shadows);
        shBox.addItemListener([&](bool b) {
            renderer.setShadows(b);
            cfg.set("render", "shadows", b ? "1" : "0");
        });
        gfx.add<ui::Label>("Ambient occlusion");
        auto& aoBox = gfx.add<ui::CheckBox>("", rset.ssao);
        aoBox.addItemListener([&](bool b) {
            renderer.setSsao(b);
            cfg.set("render", "ssao", b ? "1" : "0");
        });
        gfx.add<ui::Label>("Bloom");
        auto& bl = gfx.add<ui::Slider>(0.0f, 2.0f, rset.bloom, 0.1f);
        bl.setValueFormatter([](f32 v) { return v <= 0.0f ? std::string("off") : std::format("{:.1f}", v); });
        bl.addChangeListener([&](f32 v) {
            renderer.setBloom(v);
            cfg.setNumber("render", "bloom", v);
        });
        gfx.add<ui::Label>("Colour grade");
        auto& grade = gfx.add<ui::CheckBox>("", rset.contrast > 0.0f || rset.saturation != 1.0f || rset.vignette > 0.0f);
        grade.addItemListener([&](bool b) {
            const RenderSettings& r = rset;
            renderer.setGrade(b ? (r.saturation != 1.0f ? r.saturation : 1.1f) : 1.0f, b ? (r.contrast > 0.0f ? r.contrast : 0.2f) : 0.0f,
                              b ? (r.vignette > 0.0f ? r.vignette : 0.12f) : 0.0f);
            cfg.set("render", "saturation", b ? "1.1" : "1");
            cfg.set("render", "contrast", b ? "0.2" : "0");
            cfg.set("render", "vignette", b ? "0.12" : "0");
        });
        gfx.add<ui::Label>("Field of view");
        auto& fov = gfx.add<ui::Slider>(30.0f, 100.0f, float(cc.fovDeg), 1.0f);
        fov.setValueFormatter([](f32 v) { return std::format("{:.0f} deg", v); });
        fov.addChangeListener([&](f32 v) {
            cc.fovDeg = v;
            cfg.setNumber("display", "fov", v);
        });
        gfx.add<ui::Label>("Active");
        activeLabel = &gfx.add<ui::Label>(renderer.activeDescription());
        gfx.add<ui::Label>("DLSS");
        gfx.add<ui::Label>(renderer.dlssAvailable() ? std::format("available, frame gen up to {}x", renderer.frameGenMax() + 1)
                                                     : std::string("not available (FSR / TAA used instead)"));
        // --- simulation
        auto& simTab = tabs.addTab("Simulation");
        simTab.setLayout(std::make_unique<ui::FormLayout>(12, 6));
        simTab.add<ui::Label>("Time factor");
        auto& tf = simTab.add<ui::Slider>(1.0f, 10.0f, float(sim.timeFactor), 1.0f);
        tf.setValueFormatter([](f32 v) { return std::format("x{:.0f}", v); });
        tf.addChangeListener([&](f32 v) { sim.timeFactor = v; });
        simTab.add<ui::Label>("Profiler");
        auto& prof = simTab.add<ui::CheckBox>("Ctrl+X", hudOpt.profiler);
        prof.addItemListener([&](bool b) {
            hudOpt.profiler = b;
            cfg.set("game", "showProfiler", b ? "1" : "0");
        });
        simTab.add<ui::Label>("Statistics");
        auto& stats = simTab.add<ui::CheckBox>("", hudOpt.stats);
        stats.addItemListener([&](bool b) {
            hudOpt.stats = b;
            cfg.set("game", "showStats", b ? "1" : "0");
        });
        simTab.add<ui::Label>("Key help");
        auto& help = simTab.add<ui::CheckBox>("", hudOpt.help);
        help.addItemListener([&](bool b) {
            hudOpt.help = b;
            cfg.set("game", "showHelp", b ? "1" : "0");
        });
        // --- controls
        auto& ctl = tabs.addTab("Controls");
        ctl.setLayout(std::make_unique<ui::FormLayout>(12, 6));
        ctl.add<ui::Label>("Key file");
        ctl.add<ui::Label>(controlsFile.empty() ? std::string("openBVE defaults") : asciiOnly(controlsFile.filename().string()));
        ctl.add<ui::Label>("Mouse");
        auto& ms = ctl.add<ui::Slider>(0.5f, 10.0f, float(mouseSens * 1000.0), 0.5f);
        ms.setValueFormatter([](f32 v) { return std::format("{:.1f}", v); });
        ms.addChangeListener([&](f32 v) {
            mouseSens = v * 0.001;
            cfg.setNumber("controls", "mouseSensitivity", mouseSens);
        });
        ctl.add<ui::Label>("");
        ctl.add<ui::Label>("Keys use the openBVE controls.cfg format");
        // --- buttons
        settings.content().add<ui::Button>("Save settings").addActionListener([&] {
            if (!opt.configFile.empty() && cfg.save(opt.configFile)) hudOpt.message = "Settings saved";
        });
        settings.content().add<ui::Button>("Resume").addActionListener([&] { settings.close(); });
        settings.content().add<ui::Button>("Quit").addActionListener([&] { window.requestClose(); });
    }
    settings.close();
    bool settingsPlaced = false;
    const bool openSettingsAtStart = std::getenv("OTS_OPEN_SETTINGS") != nullptr; // screenshot tests

    // ---------------------------------------------------------------- loading
    std::vector<ui::Vertex> hud;
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 6000 && !window.shouldClose(); ++i) {
            window.pollEvents();
            renderer.prefetch(route, sim.train->front);
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            // what is in view is enough: Ahead / Far / Idle keep streaming in the background
            if ((!renderer.streaming(JobPriority::View) && i > 3) || elapsed > 60.0) break;
            CameraState cam;
            const auto view = sim.train->driverPose();
            cam.position = view.position, cam.X = view.X, cam.Y = view.Y, cam.Z = view.Z;
            cam.trackPosition = sim.train->front;
            hud.clear();
            HudOptions lo;
            lo.visible = false;
            lo.message = std::format("Loading... {} objects, {} textures", renderer.stats().meshesPending, renderer.stats().texturesPending);
            const Extent2D e = renderer.extent();
            buildHud(hud, sim, renderer.stats(), lo, f32(e.width), f32(e.height), theme);
            renderer.render(sim, cam, hud, 1.0 / 60.0);
        }
        log::info("Initial streaming done in {:.2f} s ({} objects, {} textures left to the background)",
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), renderer.stats().meshesPending,
                  renderer.stats().texturesPending);
    }

    // ---------------------------------------------------------------- main loop
    std::array<bool, GLFW_KEY_LAST + 1> held{};
    std::vector<Command> cmds;
    auto last = std::chrono::steady_clock::now();
    double fpsAccum = 0.0;
    int fpsFrames = 0;
    double messageTimer = 0.0;
    std::string lastMessage;
    std::vector<std::vector<int>> securityHeld(GLFW_KEY_LAST + 1); // per key: plugin VirtualKeys pressed
    double cursorX = -1.0, cursorY = -1.0; // framebuffer pixels
    CameraState lastCam;                   // last frame's camera (cab mouse rays)
    bool haveCam = false;
    struct { double u = 0, v = 0; long frame = -1; } cabClick;
    if (!opt.cabClick.empty()) {
        const auto v = numberList(opt.cabClick);
        if (v.size() >= 2 && v[0] && v[1]) cabClick = {*v[0], *v[1], long(v.size() > 2 ? v[2].value_or(60.0) : 60.0)};
    }
    auto heldCommand = [&](Command c) {
        for (const KeyBinding& b : controls.bindings()) {
            if (b.command != c || b.glfwKey < 0 || !window.keyDown(b.glfwKey)) continue;
            const bool shift = window.keyDown(GLFW_KEY_LEFT_SHIFT) || window.keyDown(GLFW_KEY_RIGHT_SHIFT);
            const bool ctrl = window.keyDown(GLFW_KEY_LEFT_CONTROL) || window.keyDown(GLFW_KEY_RIGHT_CONTROL);
            const bool alt = window.keyDown(GLFW_KEY_LEFT_ALT) || window.keyDown(GLFW_KEY_RIGHT_ALT);
            if (b.modifiers == ((shift ? 1 : 0) | (ctrl ? 2 : 0) | (alt ? 4 : 0))) return true;
        }
        return false;
    };

    if (opt.camera.starts_with("exterior")) {
        cc.mode = CamMode::Exterior;
        // "exterior:<distance>,<yaw>,<pitch>" (debug / screenshots)
        if (auto c = opt.camera.find(':'); c != std::string::npos) {
            const auto v = numberList(std::string_view(opt.camera).substr(c + 1));
            for (std::size_t k = 0; k < v.size() && k < 3; ++k)
                if (v[k]) (k == 0 ? cc.extDist : k == 1 ? cc.extYaw : cc.extPitch) = *v[k];
        }
    }
    else if (opt.camera == "track") cc.mode = CamMode::Track;
    else if (opt.camera.starts_with("cab:")) { // "cab:<yaw>,<pitch>" (debug / screenshots)
        const auto v = numberList(std::string_view(opt.camera).substr(4));
        if (v.size() > 0 && v[0]) cc.yaw = *v[0];
        if (v.size() > 1 && v[1]) cc.pitch = *v[1];
    }
    for (long frame = 0; !window.shouldClose() && (opt.maxFrames <= 0 || frame < opt.maxFrames); ++frame) {
        if (opt.maxFrames > 0 && frame == opt.maxFrames - 3 && !opt.screenshot.empty()) renderer.requestScreenshot(opt.screenshot.string());
        if (opt.maxFrames > 0 && frame == opt.maxFrames - 3 && !opt.pick.empty()) {
            if (const auto v = numberList(opt.pick); v.size() == 2 && v[0] && v[1]) renderer.pick(f32(*v[0]), f32(*v[1]));
        }
        renderer.profiler().beginFrame();
        renderer.pace(); // vsync: idle here, before input, rather than in the swapchain after it
        renderer.profiler().lap(CpuStage::Pace);
        window.pollEvents();
        const auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        dt = std::clamp(dt, 0.0, 0.1);
        fpsAccum += dt;
        ++fpsFrames;
        if (fpsAccum >= 0.5) {
            hudOpt.fps = f32(fpsFrames / fpsAccum);
            fpsAccum = 0.0;
            fpsFrames = 0;
        }

        // input
        InputFrame in = window.drainInput();
        if (opt.maxFrames > 0) in = {}; // scripted runs (smoke tests) ignore the desktop's input
        const Extent2D fb = renderer.extent();
        gui.setScreenSize(f32(fb.width), f32(fb.height));
        if (!settingsPlaced) {
            settings.pack();
            settings.centre(f32(fb.width), f32(fb.height));
            settingsPlaced = true;
            if (openSettingsAtStart) {
                settings.setVisible(true);
                settings.centre(f32(fb.width), f32(fb.height));
            }
        }
        bool uiMouse = false, cabScrolled = false;
        const bool cabMouse = cab && cc.mode == CamMode::Interior && haveCam && !window.cursorCaptured();
        const CarPose& driverCarPose = sim.train->cars[std::size_t(train.spec.driverCar)];
        if (cab && cabClick.frame == frame) { // scripted click (tests): press now, release next frame
            glm::dvec3 o, d;
            cabRay(lastCam, driverCarPose, (cabClick.u + 1.0) * 0.5 * fb.width, (1.0 - cabClick.v) * 0.5 * fb.height, fb, o, d);
            const int el = cab->pick(o, d);
            const bool hit = cab->press(0, o, d);
            log::info("cab click {:.3f},{:.3f}: {} (ray {:.2f},{:.2f},{:.2f} → {:.2f},{:.2f},{:.2f})", cabClick.u, cabClick.v,
                      el >= 0 ? cab->view(std::size_t(el)).name : "-", o.x, o.y, o.z, d.x, d.y, d.z);
            if (!hit) cabClick.frame = -1;
        } else if (cab && cabClick.frame >= 0 && cabClick.frame + 1 == frame) {
            cab->release(0);
        }
        for (const InputEvent& e : in.events) {
            if (gui.anyWindowVisible() && feedUi(gui, e)) {
                uiMouse |= e.type != InputEvent::Type::Key;
                if (e.type == InputEvent::Type::Key) continue;
            }
            if (e.type == InputEvent::Type::Move || e.type == InputEvent::Type::Button) cursorX = e.x, cursorY = e.y;
            if (cabMouse && !uiMouse && !gui.wantsMouse()) {
                glm::dvec3 o, d;
                cabRay(lastCam, driverCarPose, cursorX, cursorY, fb, o, d);
                if (e.type == InputEvent::Type::Button) {
                    if (e.down) cab->press(e.code, o, d);
                    else cab->release(e.code);
                } else if (e.type == InputEvent::Type::Scroll && e.y != 0.0) {
                    cabScrolled |= cab->scroll(o, d, e.y);
                }
            } else if (cab && e.type == InputEvent::Type::Button && !e.down) {
                cab->release(e.code);
            }
            if (e.type != InputEvent::Type::Key || e.code < 0 || e.code > GLFW_KEY_LAST) continue;
            const bool wasHeld = held[std::size_t(e.code)];
            held[std::size_t(e.code)] = e.down;
            if (!e.down) { // safety-system keys report their release to the plugin
                for (int vk : securityHeld[std::size_t(e.code)]) sim.pluginKey(vk, false);
                securityHeld[std::size_t(e.code)].clear();
                continue;
            }
            if (wasHeld) continue; // edge-triggered like openBVE
            cmds.clear();
            controls.commandsFor(e.code, e.mods, cmds);
            for (Command c : cmds) {
                if (const int vk = securityKey(c); vk >= 0) {
                    sim.pluginKey(vk, true);
                    securityHeld[std::size_t(e.code)].push_back(vk);
                    continue;
                }
                switch (c) {
                case Command::CameraInterior: cc.mode = CamMode::Interior; break;
                case Command::CameraExterior: cc.mode = CamMode::Exterior; break;
                case Command::CameraTrack:
                    cc.mode = CamMode::Track;
                    cc.trackCam = -1.0;
                    break;
                case Command::CameraFlyby:
                    if (cc.mode != CamMode::Free) {
                        const auto [p, X, Y, Z] = sim.train->driverPose();
                        cc.freePos = p + Y * 3.0 - Z * 10.0;
                        cc.freeYaw = std::atan2(Z.x, Z.z);
                        cc.freePitch = -0.1;
                        cc.freeTrackGuess = sim.train->front;
                    }
                    cc.mode = CamMode::Free;
                    break;
                case Command::CameraReset:
                    cc.yaw = cc.pitch = 0.0;
                    cc.offset = glm::dvec3(0.0);
                    cc.fovDeg = cfg.getNumber("display", "fov", 55.0);
                    break;
                case Command::MiscPause: sim.paused = !sim.paused; break;
                case Command::MiscQuit: window.requestClose(); break;
                case Command::MiscFullscreen: window.setFullscreen(!window.fullscreen()); break;
                case Command::MiscFps: // profiler → profiler + statistics → off → profiler
                    if (!hudOpt.profiler) hudOpt.profiler = true, hudOpt.stats = false;
                    else if (!hudOpt.stats) hudOpt.stats = true;
                    else hudOpt.profiler = hudOpt.stats = false;
                    break;
                case Command::MiscBackface:
                    renderer.toggleBackfaceCulling();
                    hudOpt.message = renderer.backfaceCulling() ? "Backface culling on" : "Backface culling off";
                    break;
                case Command::MiscInterface: hudOpt.visible = !hudOpt.visible; break;
                case Command::MiscTimeFactor:
                    sim.timeFactor = sim.timeFactor >= 5.0 ? 1.0 : 5.0;
                    hudOpt.message = std::format("Time x{:g}", sim.timeFactor);
                    break;
                case Command::MenuActivate:
                    if (settings.visible()) {
                        settings.close();
                    } else {
                        settings.setVisible(true);
                        settings.centre(f32(fb.width), f32(fb.height));
                    }
                    break;
                case Command::HornPrimary:
                    sim.train->hornPrimary = true;
                    sim.hornBlow(1);
                    break;
                case Command::HornSecondary:
                    sim.train->hornSecondary = true;
                    sim.hornBlow(2);
                    break;
                case Command::HornMusic:
                    sim.hornBlow(3);
                    sound.hornMusic();
                    break;
                case Command::MiscMute:
                    hudOpt.message = sound.toggleMute() ? "Sound muted" : "Sound on";
                    break;
                default: sim.train->command(c); break;
                }
            }
        }
        if (!heldCommand(Command::HornPrimary)) sim.train->hornPrimary = false;
        if (!heldCommand(Command::HornSecondary)) sim.train->hornSecondary = false;
        if (cab) {
            if (cabMouse && cursorX >= 0) {
                glm::dvec3 o, d;
                cabRay(lastCam, driverCarPose, cursorX, cursorY, fb, o, d);
                cab->hover(o, d);
            } else {
                cab->hover(glm::dvec3(0.0), glm::dvec3(0.0, 0.0, 1e-9));
            }
            if (cab->capturing()) { // dragging a lever / switch: the mouse moves it, not the view
                cab->drag(in.mouseDx, in.mouseDy);
                in.mouseDx = in.mouseDy = 0.0;
            }
            if (cabScrolled) in.scroll = 0.0;
            cab->update(sim.paused ? 0.0 : dt);
            for (CabMessage& m : cab->messages) sim.messages.push_back({std::move(m.text), 0, m.seconds, sim.time + m.seconds});
            cab->messages.clear();
        }

        // camera (continuous)
        const double rot = 1.2 * dt, mov = 1.5 * dt;
        if (!uiMouse && !gui.wantsMouse()) {
            const double mdx = in.mouseDx * mouseSens, mdy = in.mouseDy * mouseSens;
            switch (cc.mode) {
            case CamMode::Interior: cc.yaw += mdx, cc.pitch -= mdy; break;
            case CamMode::Exterior: cc.extYaw += mdx, cc.extPitch += mdy; break;
            case CamMode::Free: cc.freeYaw += mdx, cc.freePitch -= mdy; break;
            default: break;
            }
            if (in.scroll != 0.0) {
                if (cc.mode == CamMode::Exterior) cc.extDist = std::clamp(cc.extDist * std::pow(0.9, in.scroll), 5.0, 2000.0);
                else if (cc.mode == CamMode::Free) cc.freeSpeed = std::clamp(cc.freeSpeed * std::pow(1.25, in.scroll), 1.0, 2000.0);
                else cc.fovDeg = std::clamp(cc.fovDeg - in.scroll * 3.0, 20.0, 110.0);
            }
        }
        double& yawRef = cc.mode == CamMode::Exterior ? cc.extYaw : (cc.mode == CamMode::Free ? cc.freeYaw : cc.yaw);
        double& pitchRef = cc.mode == CamMode::Exterior ? cc.extPitch : (cc.mode == CamMode::Free ? cc.freePitch : cc.pitch);
        if (heldCommand(Command::CameraRotateLeft)) yawRef -= rot;
        if (heldCommand(Command::CameraRotateRight)) yawRef += rot;
        if (heldCommand(Command::CameraRotateUp)) pitchRef += (cc.mode == CamMode::Exterior ? -rot : rot);
        if (heldCommand(Command::CameraRotateDown)) pitchRef -= (cc.mode == CamMode::Exterior ? -rot : rot);
        if (heldCommand(Command::CameraZoomIn)) cc.fovDeg = std::max(20.0, cc.fovDeg - 30.0 * dt);
        if (heldCommand(Command::CameraZoomOut)) cc.fovDeg = std::min(110.0, cc.fovDeg + 30.0 * dt);
        cc.pitch = std::clamp(cc.pitch, -1.4, 1.4);
        cc.freePitch = std::clamp(cc.freePitch, -1.5, 1.5);
        cc.extPitch = std::clamp(cc.extPitch, -0.2, 1.5);
        glm::dvec3 move(0.0);
        if (heldCommand(Command::CameraMoveForward)) move.z += 1;
        if (heldCommand(Command::CameraMoveBackward)) move.z -= 1;
        if (heldCommand(Command::CameraMoveLeft)) move.x -= 1;
        if (heldCommand(Command::CameraMoveRight)) move.x += 1;
        if (heldCommand(Command::CameraMoveUp)) move.y += 1;
        if (heldCommand(Command::CameraMoveDown)) move.y -= 1;

        // test autopilot: depart, then hold the speed a little under the limit
        if (opt.autopilot) {
            TrainSim& t = *sim.train;
            if (t.reverser != 1) t.command(Command::ReverserForward);
            if (const auto next = sim.nextStation) {
                const StationProgress& sp = sim.stations[*next];
                if (sp.arrived && !sp.departed) sim.time = std::max(sim.time, route.stations[*next].departure.value_or(sim.time));
            }
            // OTS_AUTOPILOT_OVER=<km/h>: deliberately run that much over the limit (safety-system tests)
            static const double over = envNumber("OTS_AUTOPILOT_OVER").value_or(-5.0);
            // the safety system knows limits we don't (ATP/TVM): when it overrides the handles,
            // drive 15 km/h under the speed it intervened at, relaxing 5 km/h every 500 m
            static double apCap = 1e9, apCapAt = 0.0;
            if (t.safetyActive && t.safetyBrake > 0 && t.safetyBrake > t.brakeNotch) {
                apCap = std::min(apCap, std::max(10.0 / 3.6, std::abs(t.speed) - 15.0 / 3.6));
                apCapAt = t.front;
            }
            if (apCap < 1e9) {
                const double relaxed = apCap + (t.front - apCapAt) / 500.0 * 5.0 / 3.6;
                if (relaxed > 330.0 / 3.6) apCap = 1e9;
                else apCap = relaxed, apCapAt = t.front;
            }
            const double target = std::min({sim.speedLimit(), 300.0 / 3.6, std::getenv("OTS_AUTOPILOT_OVER") ? 1e9 : apCap}) + over / 3.6;
            const bool fast = std::abs(t.speed) > target;
            t.emergency = false;
            t.brakeNotch = fast ? 3 : 0;
            t.powerNotch = fast ? 0 : t.maxPower();
        }
        // simulation
        renderer.profiler().lap(CpuStage::Input);
        sim.update(dt);
        renderer.profiler().lap(CpuStage::Simulation);

        // camera pose
        CameraState cam;
        cam.fovY = float(glm::radians(cc.fovDeg));
        auto [dp, X, Y, Z] = sim.train->driverPose();
        switch (cc.mode) {
        case CamMode::Interior: {
            cc.offset += move * mov;
            cc.offset = glm::clamp(cc.offset, glm::dvec3(-1.5), glm::dvec3(1.5));
            if (const auto eye = cab ? cab->eye() : std::nullopt) {
                const CarPose& c = sim.train->cars[std::size_t(train.spec.driverCar)];
                dp = c.position + X * eye->x + Y * eye->y + Z * eye->z;
            }
            cam.position = dp + X * cc.offset.x + Y * cc.offset.y + Z * cc.offset.z;
            basisFrom(lookDir(X, Y, Z, cc.yaw, cc.pitch), Y, cam);
            cam.trackPosition = sim.train->front;
            cam.interior = true;
            break;
        }
        case CamMode::Exterior: {
            const CarPose& car = sim.train->cars[std::size_t(train.spec.driverCar)];
            const glm::dvec3 target = car.position + car.Y * 2.0;
            const glm::dvec3 dir = lookDir(car.X, glm::dvec3(0, 1, 0), car.Z, cc.extYaw, -cc.extPitch);
            cam.position = target - dir * cc.extDist;
            basisFrom(dir, glm::dvec3(0, 1, 0), cam);
            cam.trackPosition = sim.train->front - 0.5 * sim.train->length();
            cam.interior = false;
            break;
        }
        case CamMode::Track: {
            const double front = sim.train->front;
            if (cc.trackCam < 0 || front > cc.trackCam + 60.0 || cc.trackCam > front + 900.0)
                cc.trackCam = std::min(front + 350.0, route.endPosition() - 2.0);
            const TrackPose tp = route.poseAt(cc.trackCam);
            cam.position = tp.position + tp.side * 5.0 + tp.up * 2.5;
            const CarPose& car = sim.train->cars.front();
            basisFrom(car.position + car.Y * 2.0 - cam.position, glm::dvec3(0, 1, 0), cam);
            cam.trackPosition = cc.trackCam;
            cam.interior = false;
            break;
        }
        case CamMode::Free: {
            const glm::dvec3 dir = lookDir(glm::dvec3(1, 0, 0), glm::dvec3(0, 1, 0), glm::dvec3(0, 0, 1), cc.freeYaw, cc.freePitch);
            basisFrom(dir, glm::dvec3(0, 1, 0), cam);
            cc.freePos += (cam.X * move.x + glm::dvec3(0, 1, 0) * move.y + cam.Z * move.z) * cc.freeSpeed * dt;
            cam.position = cc.freePos;
            cc.freeTrackGuess = nearestTrackPosition(route, cc.freePos, cc.freeTrackGuess);
            cam.trackPosition = cc.freeTrackGuess;
            cam.interior = false;
            break;
        }
        }
        lastCam = cam, haveCam = true;
        sim.cameraMode = cam.interior ? 0 : 1;
        sim.cameraPosition = cam.position;

        // HUD
        if (hudOpt.message != lastMessage) {
            lastMessage = hudOpt.message;
            messageTimer = 2.0;
        }
        if (messageTimer > 0 && (messageTimer -= dt) <= 0) hudOpt.message.clear(), lastMessage.clear();
        hudOpt.cameraName = camName(cc.mode);
        const std::string pausedMsg = sim.paused ? "PAUSED" : "";
        HudOptions h = hudOpt;
        h.audio = sound.status();
        if (h.message.empty()) h.message = pausedMsg;
        if (cab && cab->hovered() >= 0 && cabMouse) {
            h.tooltip = cab->view(std::size_t(cab->hovered())).tooltip;
            h.tooltipX = f32(cursorX), h.tooltipY = f32(cursorY);
        }
        hud.clear();
        buildHud(hud, sim, renderer.stats(), h, f32(fb.width), f32(fb.height), theme);
        if (activeLabel && activeLabel->text() != renderer.activeDescription()) activeLabel->setText(renderer.activeDescription());
        if (gui.anyWindowVisible()) {
            // Ui::build only rewrites the vertices when something changed: keep the last ones
            static std::vector<ui::Vertex> uiVertices;
            gui.build(uiVertices);
            hud.insert(hud.end(), uiVertices.begin(), uiVertices.end());
        }
        renderer.profiler().lap(CpuStage::Hud);
        if (cab) {
            for (const CabSoundRequest& r : cab->sounds) sound.playOnCar(r.file, train.spec.driverCar, r.position, r.pitch, r.volume);
            cab->sounds.clear();
        }
        sound.update(sim, dt, {cam.position, cam.X, cam.Y, cam.Z, cam.interior});
        renderer.profiler().lap(CpuStage::Sound);
        if (sound.available() && opt.maxFrames > 0 && (frame % 300) == 0)
            log::info("{} @{:.0f} m {:.0f} km/h", sound.status(), sim.train->front, sim.train->speed * 3.6);
        renderer.render(sim, cam, hud, dt);
        if (opt.maxFrames > 0 && frame % 1500 == 0) {
            const RenderStats& s = renderer.stats();
            log::info("[frame {}] km {:.2f} {:.0f} km/h | textures {} ({:.0f} MB) pending {} | meshes {} loading {} | geometry {:.1f} MB | "
                      "VRAM {:.0f} MB",
                      frame, sim.train->front / 1000.0, std::abs(sim.train->speed) * 3.6, s.textureCount, s.textureBytes / 1048576.0,
                      s.texturesPending, s.meshesResident, s.meshesPending, (s.vertexBytes + s.indexBytes) / 1048576.0, s.vramUsage / 1048576.0);
        }
    }
    if (opt.maxFrames > 0) {
        const RenderStats& s = renderer.stats();
        log::info("Smoke test: {} draws, {} instances, {} textures ({:.1f} MB BC vs {:.1f} MB RGBA8), geometry {:.1f} MB, VRAM {:.0f} MB",
                       s.drawCalls, s.instances, s.textureCount, s.textureBytes / 1048576.0, s.textureRawBytes / 1048576.0,
                       (s.vertexBytes + s.indexBytes) / 1048576.0, s.vramUsage / 1048576.0);
    }
    return 0;
}

} // namespace ots
