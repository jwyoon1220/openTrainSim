#pragma once
// Simulation state shared by the game and the renderer: clock, player train, signal sections,
// station progress, and the variables that .animated function scripts read.
#include "bve/FunctionScript.hpp"
#include "safety/Safety.hpp"
#include "sim/Plugin.hpp"
#include "sim/Traffic.hpp"
#include "sim/Train.hpp"

#include <optional>
#include <memory>
#include <string>
#include <utility>

namespace ots {

class Simulation {
public:
    Simulation(const Route& route, const TrainDef& train);
    // Puts the train at a station's stop point (first stopping station by default) and sets
    // the clock as openBVE does.
    void start(std::optional<std::size_t> station = std::nullopt);
    void update(double dt);
    // AI trains. Add before start(): start() places them (preceding trains are simulated
    // forward to where they would be at the start time).
    void addTraffic(const TrainDef& def, TrafficOptions options) { traffic.emplace_back(def, route, std::move(options)); }
    std::vector<TrafficTrain> traffic;
    // Every train on the route now: f(const TrainSim&, const std::vector<StationProgress>&, bool player).
    template <class F>
    void forEachTrain(F&& f) const {
        if (playerPresent_) f(std::as_const(*train), stations, true);
        for (const TrafficTrain& t : traffic)
            if (t.active()) f(t.train(), t.progress(), false);
    }
    // Safety-system plugin from the train's ats.cfg (managed openBVE plugin via Mono).
    // Sets pluginStatus either way.
    Expected<void> startPlugin(const std::filesystem::path& hostExe);
    // openBVE VirtualKeys (S = 0, A1 = 1 … L = 15) and horn types (1 primary, 2 secondary).
    void pluginKey(int virtualKey, bool down);
    void hornBlow(int type);
    std::unique_ptr<TrainPlugin> plugin;
    // Built-in train protection (safety.json): supervises the player train; used instead of a
    // plugin. The acknowledge button is the S key (pluginKey(0)).
    std::unique_ptr<safety::Onboard> onboard;
    std::string pluginStatus;
    std::vector<PluginMessage> messages; // plugin interface messages, newest last (HUD)

    const Route& route;
    std::unique_ptr<TrainSim> train;
    std::vector<Section> sections;            // runtime copy (aspects change)
    std::vector<StationProgress> stations;
    double time = 0.0;                        // seconds since midnight
    double timeFactor = 1.0;
    bool paused = false;
    std::optional<std::size_t> nextStation;   // next stopping (or passing) station ahead
    bool overspeed = false;
    int cameraMode = 0;                       // 0 interior, 1 exterior (for scripts)
    glm::dvec3 cameraPosition{0.0};

    [[nodiscard]] double speedLimit() const;  // route + signal limit at the front, m/s
    struct SignalAhead {
        std::size_t section;
        double distance; // m from the front of the train
        int aspect;
    };
    [[nodiscard]] std::optional<SignalAhead> signalAhead() const; // next section start
    [[nodiscard]] double distanceToStop(std::size_t station) const;
    [[nodiscard]] f32 cabBrightness() const;

private:
    void updatePlugin(double dt);
    void updateSections();
    // AI handles, then physics for every train present (the player only once it is there)
    void updateTraffic(double dt);
    void runTrafficUntil(double clock); // warm-up before the player appears
    void separateTrains();
    [[nodiscard]] bool spawnAreaClear(const TrafficTrain& t) const;
    bool playerPresent_ = true;
    std::vector<TrainSim*> order_; // separateTrains() scratch
    std::optional<int> lastRev_, lastPow_, lastBrk_; // handles last sent to the plugin
    int lastDoors_ = 0;
    std::optional<std::size_t> lastSection_; // plugin signal list sent for this section
    std::vector<std::pair<int, double>> lastSignals_;
    double prevFront_ = 0.0;
    std::optional<int> lastSafetyBrake_, lastSafetyPower_; // plugin output last logged
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
    std::optional<u32> section;  // section driving a signal
    bool trainObject = false;
    const TrainSim* train = nullptr; // the train whose objects are evaluated (null = player)
    double variable(Var v, int index) const override;

private:
    const Simulation& sim_;
};

} // namespace ots
