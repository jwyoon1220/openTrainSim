#pragma once
// Terrain from a digital elevation model (DEM) around an openBVE route (docs/terrain.md).
// The route is pinned to the map by one or more anchors (track position ↔ latitude /
// longitude); real heights are sampled along the mapped corridor and shaped to the track:
// a flat formation under it, cut and fill slopes beside it, tunnels (with portals and a
// lining) where the ground is far above it and untouched valleys under high bridges.
// Terrain tiles and tunnel pieces become procedural objects placed in the route's blocks,
// so they stream, cull and cast shadows like every other route object.
#include "bve/ObjectLibrary.hpp"
#include "bve/Route.hpp"
#include "util/Expected.hpp"

#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace ots {

struct TerrainSettings {
    struct Anchor {
        double track = std::numeric_limits<double>::quiet_NaN(); // track position (m)
        double lat = 0.0, lon = 0.0;                              // degrees (WGS84)
        double elevation = std::numeric_limits<double>::quiet_NaN(); // rail altitude (m), optional
    };
    std::vector<Anchor> anchors;
    double heading = std::numeric_limits<double>::quiet_NaN(); // one anchor: track direction (deg, north = 0, east = 90)
    // DEM: Terrarium PNG tiles (URL or local path template with {z} {x} {y}).
    std::string dem = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png";
    int zoom = 13;
    double width = 1500.0;          // terrain half width around the track (m)
    double verticalWindow = 8000.0; // height fit: running median window (m)
    // shaping
    double formationWidth = 8.0;    // flat half width at the track (m)
    double formationDepth = 0.5;    // below the rail (m); openBVE ground sits 0.3 m below
    double objectWidth = 60.0;      // route objects up to this far out widen the formation (m)
    double cutSlope = 1.0;          // rise per metre of cut faces
    double fillSlope = 0.67;        // drop per metre of embankments
    double bridgeHeight = 12.0;     // deeper valleys are left alone (bridge / viaduct)
    bool tunnels = true;
    double tunnelCover = 15.0;      // ground this far above the rail → tunnel (m)
    double tunnelMinLength = 60.0;
    double tunnelRadius = 5.5;      // lining half width / arch radius (m)
    // manual corrections along the track (applied last)
    enum class ZoneType : unsigned char { Open, Tunnel, RouteTunnel, Bridge };
    struct ZoneOverride {
        double from = 0.0, to = 0.0; // track positions (m)
        ZoneType type = ZoneType::Open;
    };
    std::vector<ZoneOverride> zones;
    // mesh
    double tileSize = 256.0;
    double nearSpacing = 4.0, farSpacing = 16.0;
    double nearDistance = 300.0;    // tiles closer than this use nearSpacing
};

// "<route stem>.terrain.json" beside the route, or empty.
std::filesystem::path findTerrainFile(const std::filesystem::path& routeFile);
Expected<TerrainSettings> loadTerrainSettings(const std::filesystem::path& file, const Route& route);

struct TerrainStats {
    std::size_t demTiles = 0, demDownloaded = 0, demMissing = 0;
    std::size_t tiles = 0, tunnels = 0, bridges = 0;
    double tunnelLength = 0.0;      // generated tunnels
    double routeTunnelLength = 0.0; // tunnels the route draws itself (objects named "tunnel")
};

// Downloads / reads the DEM, analyses the track and appends terrain placements to the route
// (route.placements is rebuilt: call before anything indexes placements).
Expected<TerrainStats> buildTerrain(Route& route, ObjectLibrary& lib, const TerrainSettings& settings, const std::filesystem::path& settingsDir);

} // namespace ots
