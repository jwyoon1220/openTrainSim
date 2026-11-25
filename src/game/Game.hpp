#pragma once
// Application: loads route + train, runs the simulation and the render loop, maps input
// (openBVE controls) to train / camera commands, hosts the HUD and the settings window.
#include "bve/Controls.hpp"
#include "util/Config.hpp"

#include <filesystem>
#include <optional>

namespace ots {

struct LaunchOptions {
    std::filesystem::path route;
    std::filesystem::path train;     // empty = the route's Train.Folder
    std::filesystem::path configFile;
    std::optional<std::size_t> station; // start station (default: the first stop)
    long maxFrames = 0;              // > 0: quit after N frames (smoke test)
    std::filesystem::path screenshot; // with maxFrames: save the last frame as PNG
    std::string camera;               // initial camera: cab / exterior / track / free
    std::string pick;                 // "u,v": log objects under that screen point at the end
    bool fullscreen = false;          // start in borderless fullscreen ([display] fullscreen)
    bool maximized = false;           // start with a maximized window ([display] maximized)
    std::filesystem::path cab;        // 3D cab definition (default <train>/cab.json), "none" = openBVE cab
    std::filesystem::path vvvf;       // VVVF sound preset (default <train>/vvvf.json), "none" = off
    std::string cabClick;             // "u,v[,frame]": scripted click on a cab element (tests)
    std::filesystem::path traffic;    // AI traffic (default <route>.traffic.json), "none" = no AI trains
    std::filesystem::path terrain;    // terrain definition (default <route>.terrain.json), "none" = off
    bool autopilot = false;           // test drive: release brakes, full power, obey limits loosely
    double timeFactor = 0.0;          // > 0: override the simulation speed
    std::optional<double> startTime;  // clock at the start (seconds since midnight; default: the route's)
    std::filesystem::path safety;     // built-in train protection config (default <train>/safety.json), "none" = plugin
};

int runGame(const LaunchOptions& options, Config& config);

} // namespace ots
