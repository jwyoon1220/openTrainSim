#pragma once
// A parsed openBVE route, ready for simulation and streaming: per-block track geometry plus a
// compact list of object placements (44 bytes each; matrices are rebuilt only for what is
// visible). World coordinates are double precision; placements are relative to their block.
#include "bve/ObjectLibrary.hpp"
#include "util/Expected.hpp"

#include <glm/gtc/quaternion.hpp>

#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ots {

struct Placement {
    ObjectId object = kNoObject;
    glm::vec3 pos{0.0f};        // relative to the block origin
    glm::quat rot{1, 0, 0, 0};  // openBVE Transformation (columns X, Y, Z)
    f32 trackOffset = 0.0f;     // track position − block start
    std::optional<u32> section; // signal: the section whose aspect drives it
};
static_assert(sizeof(Placement) <= 48);

struct TrackPose {
    glm::dvec3 position{0.0};
    glm::dvec3 direction{0, 0, 1}, up{0, 1, 0}, side{1, 0, 0};
    double curveRadius = 0.0, pitch = 0.0, cant = 0.0;
};

struct RouteBlock {
    glm::dvec3 origin{0.0};       // rail 0 at the block start
    glm::dvec2 startDir{0.0, 1.0}; // horizontal direction at the start (x, z)
    double pitch = 0.0;           // rise per metre
    double curveRadius = 0.0;     // signed, 0 = straight
    double cant = 0.0;            // m
    u32 firstPlacement = 0, placementCount = 0;
    glm::vec3 bmin{0.0f}, bmax{0.0f}; // placement bounds (origin-relative, incl. object radius)
    f32 fogStart = 0, fogEnd = 0;
    u32 fogColor = 0;
    std::optional<u32> background;    // Background(n) in effect, none if never set
    i32 runSound = 0, flangeSound = 0; // train [Run] / [Flange] index for rail 0 (Train.Run / Train.Flange)
    bool pointSound = false;           // a rail crosses rail 0 here: switch sound at the block start
};

struct Fog {
    f32 start = 0.0f, end = 0.0f; // end == 0 → no fog
    u32 color = 0x808080;
};

struct StopPoint {
    double trackPos = 0.0;
    double backward = 5.0, forward = 5.0;
    int cars = 0;
};

enum class StopMode : u8 { AllStop, AllPass, PlayerStop, PlayerPass };

struct Station {
    std::string name;
    std::optional<double> arrival, departure; // seconds since midnight
    StopMode mode = StopMode::AllStop;
    bool terminal = false;
    int doors = 0;           // -1 left, 1 right, 0 both / none
    bool forcedRedSignal = false;
    double stopDuration = 15.0;
    std::vector<StopPoint> stops;
    double trackPos = 0.0;   // position of the Sta command
    std::filesystem::path arrivalSound, departureSound; // resolved, empty = none
    [[nodiscard]] bool stopsHere() const { return mode == StopMode::AllStop || mode == StopMode::PlayerStop; }
    [[nodiscard]] const StopPoint* stopFor(int cars) const;
    // Where the front of a train of `cars` cars stops (the Sta position without stop points).
    [[nodiscard]] double stopPosition(int cars) const {
        const StopPoint* sp = stopFor(cars);
        return sp ? sp->trackPos : trackPos;
    }
};

enum class SectionType : u8 { IndexBased, ValueBased };

struct Section {
    double trackPos = 0.0;
    std::vector<int> aspects;  // aspect numbers (index-based: in order of free sections ahead)
    SectionType type = SectionType::IndexBased;
    std::optional<std::size_t> departureStation; // held at the first aspect until a train may leave it
    int currentAspect = 0;     // aspect number currently shown
    int pluginAspect = 0;      // aspect reported to the train plugin (own occupancy ignored)
};

struct SpeedLimit {
    double trackPos = 0.0;
    double speed = std::numeric_limits<double>::infinity(); // m/s
};

// Track.Beacon / Track.Transponder: passed to the train plugin when the front passes it.
struct Beacon {
    double trackPos = 0.0;
    int type = 0, optional = 0;
    std::optional<std::size_t> section; // none: the next red section ahead
};

// Train-protection equipment (native routes): `system` ("ats", "tvm430", "atp") from trackPos on.
struct SafetyZone {
    double trackPos = 0.0;
    std::string system;
};

// Track.Announce: a sound played in the player train when its front passes.
struct RouteSound {
    double trackPos = 0.0;
    std::filesystem::path file;
    double speed = 0.0; // m/s; > 0 → pitch follows the train speed (pitch = v / speed)
};

struct BrightnessPoint {
    double trackPos = 0.0;
    f32 value = 1.0f;
};

struct Background {
    std::filesystem::path texture;
    int repetitions = 6;
    bool keepAspect = false;
};

struct Route {
    std::filesystem::path file, objectFolder, soundFolder, trainFolderHint;
    std::string comment, timetable, trainFolder;
    double blockLength = 25.0;
    double gauge = 1.435;
    std::vector<RouteBlock> blocks;
    std::vector<Placement> placements;
    std::vector<Station> stations;
    std::vector<Section> sections;
    std::vector<SpeedLimit> limits;
    std::vector<BrightnessPoint> brightness;
    std::vector<Beacon> beacons;
    std::vector<SafetyZone> safetyZones;  // sorted by trackPos
    std::vector<RouteSound> announcements;
    std::vector<Background> backgrounds;  // index = Background(n), empty texture = undefined
    std::vector<int> runSounds, flangeSounds; // Train.Run(railtype) / Train.Flange(railtype)
    std::vector<double> signalSpeeds;     // Route.Signal(aspect) km/h → m/s (inf = none)
    Fog initialFog;
    glm::vec3 ambient{160 / 255.0f}, diffuse{160 / 255.0f};
    glm::vec3 lightDirection{0.0f};       // towards the light
    std::optional<double> startTime;      // Route.StartTime (clock, s)
    // Route.RunInterval / Train.Interval (s): > 0 a preceding train that left that much earlier,
    // < 0 a following train (extension; openBVE only uses positive values).
    std::vector<double> runIntervals;
    // Route.Change: how the safety system starts (plugin Initialize mode, openBVE values).
    enum class SafetyStart : i8 { ServiceBrake = -1, Emergency = 0, Off = 1 } safetyStart = SafetyStart::ServiceBrake;
    double firstUsedPosition = 0.0;

    // Geometry along rail 0 (openBVE TrackFollower: true arcs inside a block).
    [[nodiscard]] TrackPose poseAt(double trackPos) const;
    [[nodiscard]] int blockAt(double trackPos) const;
    [[nodiscard]] double endPosition() const { return double(blocks.size()) * blockLength; }
    [[nodiscard]] double speedLimitAt(double trackPos) const;
    [[nodiscard]] f32 brightnessAt(double trackPos) const;
    [[nodiscard]] Fog fogAt(double trackPos) const;
    [[nodiscard]] std::optional<std::size_t> backgroundAt(double trackPos) const;
    // Section containing a track position (none before the first section).
    [[nodiscard]] std::optional<std::size_t> sectionAt(double trackPos) const;
};

struct RouteLoadStats {
    std::size_t expressions = 0, files = 0, placements = 0, unknownCommands = 0, missingObjects = 0;
    double parseSeconds = 0.0;
    std::map<std::string, int> unknown; // command → count
};

struct LoadedRoute {
    Route route;
    RouteLoadStats stats;
};

// Parses a CSV route (openBVE CsvRwRouteParser semantics). Objects are only interned.
Expected<LoadedRoute> loadRoute(const std::filesystem::path& file, ObjectLibrary& lib);

} // namespace ots
