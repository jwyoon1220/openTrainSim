#pragma once
// Built-in Korean train protection. Onboard devices (ATS, TVM430, ATP) supervise the driver
// against what the track transmits; the trackside data is derived from the simulated
// signalling (block occupancy → aspects), so every route works without authored balises.
// Route-authored zones (Route::safetyZones) say which system the track is equipped with.
#include "bve/Route.hpp"
#include "util/Expected.hpp"
#include "util/Json.hpp"

#include <array>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ots {
class TrainSim;
}

namespace ots::safety {

enum class System : u8 { Ats, Tvm430, Atp };
inline constexpr std::array<std::string_view, 3> kSystemKeys{"ats", "tvm430", "atp"};
inline constexpr std::array<std::string_view, 3> kSystemLabels{"ATS", "TVM430", "ATP"};
[[nodiscard]] std::optional<System> parseSystem(std::string_view key);
[[nodiscard]] constexpr std::string_view label(System s) { return kSystemLabels[std::size_t(s)]; }

// What a device asks of the train. Brake: notch (0 = none); emergency overrides it.
struct Command {
    int brake = 0;
    bool emergency = false;
    bool powerCut = false;
    [[nodiscard]] bool any() const { return brake > 0 || emergency || powerCut; }
    void merge(const Command& o) {
        brake = std::max(brake, o.brake);
        emergency = emergency || o.emergency;
        powerCut = powerCut || o.powerCut;
    }
};

// What the driver's display shows (HUD DMI, cab gauges).
struct Display {
    std::string code;                              // ATS code (YG …), TVM step, ATP mode
    std::optional<double> permitted;               // m/s supervised now (none = unsupervised)
    std::optional<double> target, targetDistance;  // next lower permitted speed (m/s) and where (m)
    std::optional<double> intervention;            // m/s at which the device brakes (ATP curve)
    bool warning = false;                          // alarm sounding
    bool braking = false;                          // the device is braking
    bool emergency = false;                        // … with the emergency brake
    bool ackRequired = false;                      // waiting for the driver's acknowledgement
    std::string message;                           // short Korean text, empty = none
};

// The track as the train's receivers see it: signals (section starts) and their aspects with
// the train's own occupancy ignored, line speeds, authored equipment zones.
class Trackside {
public:
    Trackside(const Route& route, std::span<const Section> sections) : route_(route), sections_(sections) {}
    [[nodiscard]] const Route& route() const { return route_; }
    [[nodiscard]] std::size_t sectionCount() const { return sections_.size(); }
    [[nodiscard]] double signalPosition(std::size_t section) const { return sections_[section].trackPos; }
    // Aspect transmitted at the signal at the start of `section`.
    [[nodiscard]] int aspect(std::size_t section) const { return sections_[section].pluginAspect; }
    // Stop: aspect 0 (the openBVE convention plugins rely on; single-aspect sections never stop).
    [[nodiscard]] bool isStop(std::size_t section) const { return !sections_[section].aspects.empty() && sections_[section].pluginAspect == 0; }
    [[nodiscard]] std::optional<std::size_t> sectionAt(double pos) const { return route_.sectionAt(pos); }
    // Signals passed going from `from` to `to` (to > from): sections [first, last).
    [[nodiscard]] std::pair<std::size_t, std::size_t> signalsBetween(double from, double to) const;
    // First signal ahead of `pos` showing stop: the end of the movement authority.
    [[nodiscard]] std::optional<std::size_t> nextStop(double pos) const;
    // Free blocks after `section` before one at stop (capped at `cap`).
    [[nodiscard]] int freeBlocksAfter(std::size_t section, int cap) const;
    // Where the block starting at `section` ends (the next signal; infinity after the last).
    [[nodiscard]] double blockEnd(std::size_t section) const {
        return section + 1 < sections_.size() ? sections_[section + 1].trackPos : std::numeric_limits<double>::infinity();
    }
    // Equipment zone at a position (none: not authored).
    [[nodiscard]] std::optional<System> zoneAt(double pos) const;
    [[nodiscard]] double lineSpeed(double pos) const { return route_.speedLimitAt(pos); }

private:
    const Route& route_;
    std::span<const Section> sections_;
};

struct Context {
    const Trackside& track;
    const TrainSim& train;
    double dt = 0.0;
    double previousFront = 0.0; // front a step ago (signals passed in between)
};

// One onboard device. Its `command` and `display` are valid after update().
class Device {
public:
    explicit Device(System s) : system(s) {}
    virtual ~Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    // Becomes the active device (start, or a transition into its zone).
    virtual void activate(const Context&) {}
    virtual void update(const Context&) = 0;
    // The driver's acknowledge button (확인, openBVE SECURITY_S).
    virtual void acknowledge(bool down) { (void)down; }

    const System system;
    Command command;
    Display display;
};

// ---------------------------------------------------------------- configuration (safety.json)

struct AtsCode {
    std::string name;
    double speed = 0.0; // m/s supervised while this code is held (inf = none)
    bool stop = false;  // passing a signal transmitting it is an emergency
};

struct AtsConfig {
    std::vector<AtsCode> codes;
    std::vector<std::pair<int, std::size_t>> aspectCodes; // route aspect → code index
    std::size_t clearCode = 0;   // aspects not listed
    double ackSeconds = 3.0;     // overspeed alarm → emergency unless acknowledged within
    double tolerance = 1.0 / 3.6; // m/s over the code speed before the alarm
    int minBrake = 1;            // notch the driver must hold while still over speed
    double afterStop = 15 / 3.6; // supervised after a stop-signal emergency is reset, until the next signal
    [[nodiscard]] std::size_t codeFor(int aspect) const;
};

// TVM430 (high-speed lines): the step of the block the train is in, from the number of free
// blocks ahead: steps.back() (000) when the next block is occupied, then one step up per
// free block. The next block's step is announced (target).
struct TvmConfig {
    std::vector<double> steps;       // m/s, fastest first; the last is 000 (0)
    double warningMargin = 5 / 3.6;  // over the step: alarm
    double emergencyMargin = 15 / 3.6; // over the step: emergency brake until under the step
    double finalDecel = 0.6;         // m/s²: the 000 block's curve to its end
};

// Korean ATP (ETCS Level 1 based): movement authority (to the next signal at stop) and the
// speed profile are received at the balises of each signal; supervision with braking curves.
struct AtpConfig {
    double serviceDecel = 0.0;       // m/s², 0 = 80 % of the train's service deceleration
    double emergencyDecel = 0.0;     // m/s², 0 = 110 % of it
    double brakeDelay = 1.5;         // s until the brake is effective
    double indicationTime = 4.0;     // s of warning ahead of the service-brake curve
    double releaseSpeed = 15 / 3.6;  // allowed close to the end of authority
    double ceilingWarning = 3 / 3.6, ceilingService = 6 / 3.6, ceilingEmergency = 10 / 3.6;
    double staffResponsible = 40 / 3.6; // SR mode after a trip, until the next balise
    double lookahead = 8000.0;       // m of speed profile received
};

struct SafetyConfig {
    std::vector<System> systems;   // onboard devices
    System defaultSystem = System::Ats; // where the route authored no zone
    double transitionAckSeconds = 8.0;  // entering another system's zone: acknowledge within
    AtsConfig ats;
    TvmConfig tvm;
    AtpConfig atp;
    [[nodiscard]] bool has(System s) const { return std::ranges::find(systems, s) != systems.end(); }
};

// Defaults (Korean conventions; adjust per train in safety.json).
[[nodiscard]] SafetyConfig defaultConfig();
// <train>/safety.json; missing members keep their defaults.
[[nodiscard]] Expected<SafetyConfig> loadConfig(const std::filesystem::path& file);

// ---------------------------------------------------------------- onboard

// The train's devices: picks the active one by zone, manages transitions, and overrides the
// driver's handles with the strongest command (TrainSim::safety*).
class Onboard {
public:
    explicit Onboard(const SafetyConfig& config);
    ~Onboard();
    void update(const Trackside& track, TrainSim& train, double dt);
    void acknowledge(bool down);
    [[nodiscard]] std::optional<System> active() const;
    [[nodiscard]] const Display& display() const { return display_; }
    [[nodiscard]] const Device* device(System s) const;
    // Index of the ATS code held (AtsConfig::codes), 0 without ATS.
    [[nodiscard]] std::size_t atsCode() const;
    // Statistics (ots_check): brake interventions started, emergency ones among them.
    int interventions = 0, emergencies = 0;

private:
    Device* find(System s);
    SafetyConfig config_;
    std::vector<std::unique_ptr<Device>> devices_;
    Device* active_ = nullptr;
    std::optional<double> previousFront_;
    // transition into another system's zone, awaiting acknowledgement
    struct Transition {
        System to;
        double elapsed = 0.0;
    };
    std::optional<Transition> transition_;
    bool ackHeld_ = false;
    bool wasBraking_ = false, wasEmergency_ = false;
    Display display_;
};

} // namespace ots::safety
