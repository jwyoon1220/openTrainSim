#pragma once
// Application: loads route + train, runs the simulation and the render loop, maps input
// (openBVE controls) to train / camera commands, hosts the HUD and the settings window.
#include "bve/Controls.hpp"
#include "util/Config.hpp"

#include <filesystem>

namespace ots {

struct LaunchOptions {
    std::filesystem::path route;
    std::filesystem::path train;     // empty = the route's Train.Folder
    std::filesystem::path configFile;
    int station = -1;                // start station (-1 = first stop)
    long maxFrames = 0;              // > 0: quit after N frames (smoke test)
    std::filesystem::path screenshot; // with maxFrames: save the last frame as PNG
    std::string camera;               // initial camera: cab / exterior / track / free
    std::string pick;                 // "u,v": log objects under that screen point at the end
    bool autopilot = false;           // test drive: release brakes, full power, obey limits loosely
    double timeFactor = 0.0;          // > 0: override the simulation speed
};

int runGame(const LaunchOptions& options, Config& config);

} // namespace ots
