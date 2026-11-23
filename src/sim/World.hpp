#pragma once
// Simulation state shared by the game and the renderer: clock, player train, signal sections,
// station progress, and the variables that .animated function scripts read.
#include "bve/FunctionScript.hpp"
#include "sim/Plugin.hpp"
#include "sim/Train.hpp"

#include <memory>

namespace ots {

struct StationProgress {
    bool arrived = false, doorsOpened = false, departed = false, passed = false;
    double arrivalTime = -1.0;
    double stopError = 0.0; // metres from the stop point when stopped
};

class Simulation {
public:
    Simulation(const Route& route, const TrainDef& train);
    // Puts the train at a station's stop point (first stopping station by default) and sets
    // the clock as openBVE does.
    void start(int station = -1);
    void update(double dt);
    // Safety-system plugin from the train's ats.cfg (managed openBVE plugin via Mono).
    bool startPlugin(const std::filesystem::path& hostExe, std::string& status);
    // openBVE VirtualKeys (S = 0, A1 = 1 … L = 15) and horn types (1 primary, 2 secondary).
    void pluginKey(int virtualKey, bool down);
    void hornBlow(int type);
    std::unique_ptr<TrainPlugin> plugin;
    std::string pluginStatus;
    std::vector<PluginMessage> messages; // plugin interface messages, newest last (HUD)

    const Route& route;
    std::unique_ptr<TrainSim> train;
    std::vector<Section> sections;            // runtime copy (aspects change)
    std::vector<StationProgress> stations;
    double time = 0.0;                        // seconds since midnight
    double timeFactor = 1.0;
    bool paused = false;
    int nextStation = -1;                     // next stopping (or passing) station ahead
    bool overspeed = false;
    int cameraMode = 0;                       // 0 interior, 1 exterior (for scripts)
    glm::dvec3 cameraPosition{0.0};

    [[nodiscard]] double speedLimit() const;  // route + signal limit at the front, m/s
    [[nodiscard]] int signalAhead(double& distance, int& aspect) const; // next section start
    [[nodiscard]] double distanceToStop(int station) const;
    [[nodiscard]] f32 cabBrightness() const;

private:
    void updatePlugin(double dt);
    void updateSections();
    int lastRev_ = -9, lastPow_ = -1, lastBrk_ = -1, lastDoors_ = 0, lastSection_ = -2;
    std::vector<std::pair<int, double>> lastSignals_;
    double prevFront_ = 0.0;
    int lastSafetyBrake_ = -1, lastSafetyPower_ = -1;
    std::vector<int> tracePanel_;
    void updateStations(double dt);
};

// Variables for one evaluation of an animated object's functions.
class ScriptContext final : public FunctionContext {
public:
    explicit ScriptContext(const Simulation& sim) : sim_(sim) {}
    int car = 0;                 // car index (train objects), 0 otherwise
    double trackPosition = 0.0;  // object's track position (route objects)
    glm::dvec3 position{0.0};    // object's world position
    int section = -1;            // section driving a signal
    bool trainObject = false;
    double variable(Var v, int index) const override;

private:
    const Simulation& sim_;
};

} // namespace ots
