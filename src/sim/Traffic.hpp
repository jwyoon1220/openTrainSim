#pragma once
// AI trains (docs/traffic.md): other trains on rail 0, same direction, driven by a simple
// driver that obeys signals, speed limits and its (shifted) timetable, stops at stations and
// leaves the route at its terminal. Defined by the route's Route.RunInterval and by
// <route>.traffic.json.
#include "sim/Train.hpp"
#include "util/Expected.hpp"

#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ots {

class Simulation;

struct StationProgress {
    bool arrived = false, doorsOpened = false, departed = false, passed = false;
    double arrivalTime = -1.0;
    double stopError = 0.0; // metres from the stop point when stopped
};

struct TrafficOptions {
    std::string name;
    // Timetable shift in seconds: > 0 runs that much earlier (preceding train), < 0 later
    // (following). Route.RunInterval semantics.
    double interval = 0.0;
    std::optional<double> departure;          // instead of an interval: clock time (s) it leaves `station`
    std::optional<std::size_t> station;       // start station (default: the player's)
    double maxSpeed = std::numeric_limits<double>::infinity(); // m/s
    bool stops = true;                        // false: runs through every station
    std::optional<double> dwell;              // minimum stop (s), default: the station's
};

class TrafficTrain {
public:
    enum class State : u8 { Waiting, Running, Done };

    TrafficTrain(const TrainDef& def, const Route& route, TrafficOptions options);

    [[nodiscard]] const std::string& name() const { return options_.name; }
    [[nodiscard]] State state() const { return state_; }
    [[nodiscard]] bool active() const { return state_ == State::Running; }
    [[nodiscard]] const TrainSim& train() const { return *train_; }
    [[nodiscard]] TrainSim& train() { return *train_; }
    [[nodiscard]] const std::vector<StationProgress>& progress() const { return progress_; }
    [[nodiscard]] std::optional<std::size_t> stoppedAt() const { return stop_ ? std::optional(stop_->station) : std::nullopt; }

    // Timetable and start station, given the player's start (clock, station). Returns when
    // the train appears.
    double schedule(double clock, std::optional<std::size_t> playerStation);
    [[nodiscard]] double spawnTime() const { return spawnTime_; }
    [[nodiscard]] bool due(double clock) const { return state_ == State::Waiting && clock >= spawnTime_; }
    [[nodiscard]] double spawnFront() const; // where its front stands when it appears
    // Appears at its start station, doors open, waiting for its departure time.
    void spawn(double clock);
    // Not yet running after all (it would stand inside the player): enter later.
    void postpone(double clock);
    // One driver decision (handles, doors); physics runs separately.
    void drive(const Simulation& sim);

private:
    struct Stop {
        std::size_t station;
        double departAt; // clock time the doors close and it may leave
    };
    void trackSignalSpeed(const Simulation& sim, std::size_t section);
    void serveStop(const Simulation& sim);
    [[nodiscard]] std::optional<std::size_t> nextStop(const Simulation& sim);
    [[nodiscard]] bool tryArrive(const Simulation& sim, std::size_t station);
    [[nodiscard]] double departureAt(const Station& s, double arrival) const;
    [[nodiscard]] double stopPosition(std::size_t station) const;
    void setHandles(double target, double neededDeceleration, double brakeRate);
    void hold();

    TrafficOptions options_;
    const Route& route_;
    std::unique_ptr<TrainSim> train_;
    std::vector<StationProgress> progress_;
    State state_ = State::Waiting;
    double timeOffset_ = 0.0; // its station times = timetable − timeOffset
    double spawnTime_ = 0.0;
    std::optional<std::size_t> startStation_;
    std::optional<Stop> stop_;
    // Route.Signal speed: the aspect seen when passing a signal applies until the next one.
    std::optional<std::size_t> section_;
    double signalSpeed_ = std::numeric_limits<double>::infinity(), aheadSignalSpeed_ = std::numeric_limits<double>::infinity();
};

// ------------------------------------------------------------------ traffic.json
struct TrafficEntry {
    std::filesystem::path trainFolder; // empty = the player's train
    TrafficOptions options;
};

struct TrafficConfig {
    bool routeIntervals = true; // also use Route.RunInterval
    std::vector<TrafficEntry> trains;
};

// "<route stem>.traffic.json" beside the route, or empty.
std::filesystem::path findTrafficFile(const std::filesystem::path& routeFile);
Expected<TrafficConfig> loadTrafficConfig(const std::filesystem::path& file);

// "HH:MM:SS", "HH:MM" or openBVE "HH.MMSS" → seconds since midnight.
std::optional<double> parseClock(std::string_view s);

} // namespace ots
