#include "game/Game.hpp"

#include "audio/Audio.hpp"
#include "audio/TrainSounds.hpp"
#include "bve/Route.hpp"
#include "bve/Train.hpp"
#include "phx/core/Log.hpp"
#include "phx/platform/UiInput.hpp"
#include "phx/platform/Window.hpp"
#include "phx/ui/Ui.hpp"
#include "render/Hud.hpp"
#include "render/Renderer.hpp"
#include "sim/World.hpp"
#include "util/Text.hpp"
#include "util/ThreadPool.hpp"

#include <GLFW/glfw3.h>

#include <array>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <format>
#include <thread>

namespace ots {
namespace {

enum class CamMode { Interior, Exterior, Track, Free };
const char* camName(CamMode m) {
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

} // namespace

int runGame(const LaunchOptions& opt, Config& cfg) {
    setDefaultCodepage(cfg.get("content", "codepage", "CP949"));
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    const int workersCfg = cfg.getInt("render", "workers", 0);
    ThreadPool pool(workersCfg > 0 ? static_cast<unsigned>(workersCfg) : std::max(1u, hw - 2));
    ObjectLibrary lib;

    // ---------------------------------------------------------------- route + train
    Route route;
    RouteLoadStats rs;
    std::string err;
    log::info("Loading route {}", opt.route.string());
    if (!loadRoute(opt.route, lib, route, rs, err)) {
        log::error("{}", err);
        return 1;
    }
    log::info("Route: {:.1f} km, {} placements, {} objects, {} stations ({:.2f} s)", route.endPosition() / 1000.0, rs.placements, lib.size(),
                   route.stations.size(), rs.parseSeconds);
    std::filesystem::path trainFolder = opt.train;
    if (trainFolder.empty()) trainFolder = findTrainFolder(route.file, route.trainFolderHint, route.trainFolder);
    if (trainFolder.empty()) {
        log::error("Train folder '{}' not found (use --train <folder>)", route.trainFolder);
        return 1;
    }
    TrainDef train;
    if (!loadTrain(trainFolder, lib, train, err)) {
        log::error("{}", err);
        return 1;
    }
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
    RenderSettings rset;
    {   // anti-aliasing / upscaling (older configs: [display] msaa = N)
        const int msaa = cfg.getInt("display", "msaa", 4);
        rset.aa = msaa >= 8 ? AaMode::Msaa8 : msaa >= 4 ? AaMode::Msaa4 : msaa >= 2 ? AaMode::Msaa2 : AaMode::Off;
        AaMode m;
        if (parseAaMode(cfg.get("render", "aa"), m)) rset.aa = m;
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
    Renderer renderer(window, rset, lib, pool);
    renderer.pinTrain(train);

    Simulation sim(route, train);
    sim.start(opt.station);
    if (cfg.getBool("simulation", "plugins", true)) {
        if (sim.startPlugin(pluginHostPath(), sim.pluginStatus)) log::info("Plugin: {}", sim.pluginStatus);
        else log::warn("Plugin: {}", sim.pluginStatus);
    } else {
        sim.pluginStatus = "plugins disabled ([simulation] plugins = false)";
    }

    // ---------------------------------------------------------------- sound
    AudioSystem audio;
    std::unique_ptr<TrainSounds> trainSounds;
    if (cfg.getBool("sound", "enabled", true) && (opt.maxFrames == 0 || std::getenv("OTS_SOUND"))) {
        if (audio.init(std::clamp(cfg.getInt("sound", "voices", 64), 8, 256))) {
            audio.masterVolume = float(std::clamp(cfg.getNumber("sound", "volume", 1.0), 0.0, 4.0));
            trainSounds = std::make_unique<TrainSounds>(audio, sim);
        }
    }
    sim.timeFactor = opt.timeFactor > 0 ? opt.timeFactor : cfg.getNumber("game", "timeFactor", 1.0);

    CameraControl cc;
    cc.fovDeg = cfg.getNumber("display", "fov", 55.0);
    double mouseSens = cfg.getNumber("controls", "mouseSensitivity", 0.003);

    // ---------------------------------------------------------------- UI
    ui::Theme theme;
    theme.textScale = float(cfg.getNumber("display", "uiScale", 2.0));
    ui::Ui gui(theme);
    HudOptions hudOpt;
    hudOpt.stats = cfg.getBool("game", "showStats", false);
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
        for (int i = 0; i < int(AaMode::Count); ++i) aaNames.push_back(aaModeName(AaMode(i)));
        gfx.add<ui::Label>("Anti-aliasing");
        auto& aaBox = gfx.add<ui::ComboBox>(aaNames);
        aaBox.setSelectedIndex(int(renderer.aaMode()));
        for (int i = 0; i < int(AaMode::Count); ++i)
            if (isDlssMode(AaMode(i)) && !renderer.dlssAvailable()) aaBox.setItemEnabled(i, false);
        aaBox.addActionListener([&](int i) {
            renderer.setAaMode(AaMode(i));
            cfg.set("render", "aa", aaModeKey(AaMode(i)));
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
        simTab.add<ui::Label>("Statistics");
        auto& stats = simTab.add<ui::CheckBox>("Ctrl+X", hudOpt.stats);
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
            if ((!renderer.streaming() && i > 3) || elapsed > 60.0) break;
            CameraState cam;
            glm::dvec3 X, Y, Z;
            sim.train->driverPose(cam.position, X, Y, Z);
            cam.X = X, cam.Y = Y, cam.Z = Z;
            cam.trackPosition = sim.train->front;
            hud.clear();
            HudOptions lo;
            lo.visible = false;
            lo.message = std::format("Loading... {} objects, {} textures", renderer.stats().meshesPending, renderer.stats().texturesPending);
            const Extent2D e = renderer.extent();
            buildHud(hud, sim, renderer.stats(), lo, f32(e.width), f32(e.height), theme);
            renderer.render(sim, cam, hud, 1.0 / 60.0);
        }
        log::info("Initial streaming done in {:.2f} s",
                       std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
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
            auto v = split(opt.camera.substr(c + 1), ',');
            double x;
            if (v.size() > 0 && parseDouble(v[0], x)) cc.extDist = x;
            if (v.size() > 1 && parseDouble(v[1], x)) cc.extYaw = x;
            if (v.size() > 2 && parseDouble(v[2], x)) cc.extPitch = x;
        }
    }
    else if (opt.camera == "track") cc.mode = CamMode::Track;
    else if (opt.camera.starts_with("cab:")) { // "cab:<yaw>,<pitch>" (debug / screenshots)
        auto v = split(opt.camera.substr(4), ',');
        double x;
        if (v.size() > 0 && parseDouble(v[0], x)) cc.yaw = x;
        if (v.size() > 1 && parseDouble(v[1], x)) cc.pitch = x;
    }
    for (long frame = 0; !window.shouldClose() && (opt.maxFrames <= 0 || frame < opt.maxFrames); ++frame) {
        if (opt.maxFrames > 0 && frame == opt.maxFrames - 3 && !opt.screenshot.empty()) renderer.requestScreenshot(opt.screenshot.string());
        if (opt.maxFrames > 0 && frame == opt.maxFrames - 3 && !opt.pick.empty()) {
            auto v = split(opt.pick, ',');
            double u = 0, w = 0;
            if (v.size() == 2 && parseDouble(v[0], u) && parseDouble(v[1], w)) renderer.pick(f32(u), f32(w));
        }
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
        bool uiMouse = false;
        for (const InputEvent& e : in.events) {
            if (gui.anyWindowVisible() && feedUi(gui, e)) {
                uiMouse |= e.type != InputEvent::Type::Key;
                if (e.type == InputEvent::Type::Key) continue;
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
                        glm::dvec3 p, X, Y, Z;
                        sim.train->driverPose(p, X, Y, Z);
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
                case Command::MiscFps: hudOpt.stats = !hudOpt.stats; break;
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
                    if (trainSounds) trainSounds->hornMusic();
                    break;
                case Command::MiscMute:
                    audio.muted = !audio.muted;
                    hudOpt.message = audio.muted ? "Sound muted" : "Sound on";
                    break;
                default: sim.train->command(c); break;
                }
            }
        }
        if (!heldCommand(Command::HornPrimary)) sim.train->hornPrimary = false;
        if (!heldCommand(Command::HornSecondary)) sim.train->hornSecondary = false;

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
            const StationProgress* sp = sim.nextStation >= 0 ? &sim.stations[std::size_t(sim.nextStation)] : nullptr;
            if (sp && sp->arrived && !sp->departed) sim.time = std::max(sim.time, route.stations[std::size_t(sim.nextStation)].departure);
            // OTS_AUTOPILOT_OVER=<km/h>: deliberately run that much over the limit (safety-system tests)
            static const double over = std::getenv("OTS_AUTOPILOT_OVER") ? std::atof(std::getenv("OTS_AUTOPILOT_OVER")) : -5.0;
            const double target = std::min(sim.speedLimit(), 300.0 / 3.6) + over / 3.6;
            const bool fast = std::abs(t.speed) > target;
            t.emergency = false;
            t.brakeNotch = fast ? 3 : 0;
            t.powerNotch = fast ? 0 : t.maxPower();
        }
        // simulation
        sim.update(dt);

        // camera pose
        CameraState cam;
        cam.fovY = float(glm::radians(cc.fovDeg));
        glm::dvec3 dp, X, Y, Z;
        sim.train->driverPose(dp, X, Y, Z);
        switch (cc.mode) {
        case CamMode::Interior: {
            cc.offset += move * mov;
            cc.offset = glm::clamp(cc.offset, glm::dvec3(-1.5), glm::dvec3(1.5));
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
        if (audio.available()) h.audio = std::format("sound {} playing, {} audible{}", audio.activeSounds(), audio.audibleVoices(), audio.muted ? " (muted)" : "");
        if (h.message.empty()) h.message = pausedMsg;
        hud.clear();
        buildHud(hud, sim, renderer.stats(), h, f32(fb.width), f32(fb.height), theme);
        if (activeLabel && activeLabel->text() != renderer.activeDescription()) activeLabel->setText(renderer.activeDescription());
        if (gui.anyWindowVisible()) {
            // Ui::build only rewrites the vertices when something changed: keep the last ones
            static std::vector<ui::Vertex> uiVertices;
            gui.build(uiVertices);
            hud.insert(hud.end(), uiVertices.begin(), uiVertices.end());
        }
        if (trainSounds) {
            trainSounds->update(sim, sim.paused ? 0.0 : dt * sim.timeFactor);
            if (opt.maxFrames > 0 && (frame % 300) == 0)
                log::info("sound @{:.0f} m {:.0f} km/h: {} playing, {} audible", sim.train->front, sim.train->speed * 3.6, audio.activeSounds(),
                          audio.audibleVoices());
            audio.update(dt, cam.position, cam.X, cam.Y, cam.Z, sim.train->cars, train.spec.driverCar, cam.interior);
        }
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
