#pragma once
// Terrain internals: the ground model. Analyses the route once (formation width from the
// route's own objects, height fit against the DEM, tunnel / bridge zones) and then answers
// "how high is the ground here" for mesh building, from any thread.
#include "bve/ObjectLibrary.hpp"
#include "terrain/Geo.hpp"
#include "terrain/Track.hpp"

#include <span>
#include <vector>

namespace ots::terrain {

enum class Zone : u8 { Open, Tunnel, Bridge };

// Half-open range of track samples [begin, end).
struct SampleRange {
    std::size_t begin = 0, end = 0;
    [[nodiscard]] std::size_t size() const { return end - begin; }
    [[nodiscard]] double from() const { return TrackIndex::trackPos(begin); }
    [[nodiscard]] double to() const { return TrackIndex::trackPos(end); }
};

struct GroundPoint {
    double y = 0.0;
    bool modified = false; // cut / fill / ridge (soil texture)
    bool water = false;    // at or below sea level
    bool core = false;     // over a tunnel, right above the tube
    bool mouth = false;    // beside the track outside a tunnel: core + mouth = portal opening
};

class TerrainModel {
public:
    TerrainModel(const TerrainSettings& settings, const Route& route, const ObjectLibrary& lib, TrackIndex track, GeoMapping geo, Dem dem);

    [[nodiscard]] GroundPoint ground(glm::dvec2 p) const;
    [[nodiscard]] GroundPoint ground(glm::dvec2 p, const TrackIndex::Neighbourhood& near) const;

    [[nodiscard]] const TerrainSettings& settings() const { return settings_; }
    [[nodiscard]] const Route& route() const { return route_; }
    [[nodiscard]] const TrackIndex& track() const { return track_; }
    // Lining profile top (arch crown) above the rail.
    [[nodiscard]] double crown() const { return 1.5 + settings_.tunnelRadius; }
    // Tunnel zones the route does not draw itself: these get a lining and portals.
    [[nodiscard]] std::span<const SampleRange> generatedTunnels() const { return generated_; }
    [[nodiscard]] std::size_t bridgeCount() const { return bridges_; }
    [[nodiscard]] double routeTunnelLength() const { return routeTunnelLength_; }
    // How far from the track (plan) the terrain may be asked for.
    [[nodiscard]] double reach() const { return settings_.width + 2.0 * settings_.tileSize; }

private:
    [[nodiscard]] GroundPoint shape(glm::dvec2 p, const TrackHit& hit) const;
    [[nodiscard]] std::optional<double> demAt(glm::dvec2 p, double trackPos) const;
    [[nodiscard]] double offsetAt(double trackPos) const;
    void fitFormationWidths();
    void fitHeights(std::span<const std::optional<double>> centreline);
    void classifyZones(const ObjectLibrary& lib, std::span<const double> cover);
    void logDebug(std::span<const std::optional<double>> centreline, std::span<const double> cover) const;

    TerrainSettings settings_;
    const Route& route_;
    TrackIndex track_;
    GeoMapping geo_;
    Dem dem_;
    // per track sample
    std::vector<float> halfWidth_; // flat formation half width (route objects stand on it)
    std::vector<float> offset_;    // DEM altitude − route height
    std::vector<Zone> zone_;
    std::vector<SampleRange> generated_;
    std::size_t bridges_ = 0;
    double routeTunnelLength_ = 0.0;
};

} // namespace ots::terrain
