#pragma once
// Terrain internals: route ↔ map alignment and the elevation model (Terrarium DEM tiles).
#include "bve/Route.hpp"
#include "terrain/Terrain.hpp"
#include "util/Expected.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ots::terrain {

struct LatLon {
    double lat = 0.0, lon = 0.0; // degrees
};

// Route world (x, z) → latitude / longitude. One similarity transform (rotation + uniform
// scale) per span between consecutive anchors, blended around interior anchors so the map is
// continuous; every anchor maps exactly.
class GeoMapping {
public:
    // Fails if the anchors cannot be fitted. Logs the scale of every span.
    static Expected<GeoMapping> fit(const Route& route, const TerrainSettings& settings);
    [[nodiscard]] LatLon toLatLon(glm::dvec2 world, double trackPos) const;

private:
    struct Span {
        double start = 0.0;          // track position where the span begins
        glm::dvec2 world0{0.0}, map0{0.0};
        double c = 1.0, s = 0.0;     // σ·cos φ, σ·sin φ
        [[nodiscard]] glm::dvec2 apply(glm::dvec2 p) const;
    };
    [[nodiscard]] glm::dvec2 toMetric(glm::dvec2 world, double trackPos) const; // east, north (m) from the origin
    [[nodiscard]] glm::dvec2 metricOf(LatLon p) const;
    std::vector<Span> spans_; // sorted by start
    LatLon origin_;
    double blend_ = 1000.0;
};

// Web-Mercator tile of a zoom level.
struct TileId {
    int x = 0, y = 0;
    bool operator==(const TileId&) const = default;
    static TileId of(LatLon p, int zoom);
};

// Where tiles come from: a URL template (downloaded once into the cache) or a local path
// template, both with {z} {x} {y}.
class DemSource {
public:
    DemSource(std::string pattern, int zoom, const std::filesystem::path& settingsDir);
    [[nodiscard]] bool remote() const { return remote_; }
    [[nodiscard]] std::filesystem::path file(TileId t) const;
    [[nodiscard]] std::string url(TileId t) const;
    [[nodiscard]] const std::filesystem::path& cacheDir() const { return cacheDir_; }
    [[nodiscard]] int zoom() const { return zoom_; }
    // Downloads the tiles not in the cache yet (8 parallel curl processes); returns how many
    // were fetched and how many failed.
    struct FetchResult {
        std::size_t fetched = 0, failed = 0;
    };
    FetchResult fetchMissing(std::span<const TileId> tiles) const;

private:
    [[nodiscard]] std::string expand(TileId t) const;
    std::string pattern_;
    int zoom_;
    bool remote_;
    std::filesystem::path cacheDir_, baseDir_;
};

// Heights from Terrarium tiles held in a dense grid over the tiles' bounding box (array
// lookups, no hashing on the per-vertex path).
class Dem {
public:
    Dem(int zoom, std::span<const TileId> tiles);
    // Decodes a tile; false if the file is missing or not a 256×256 Terrarium PNG.
    bool load(TileId t, const std::filesystem::path& file);
    // Metres above sea level, bilinear; nullopt where no tile is loaded.
    [[nodiscard]] std::optional<double> height(LatLon p) const;

private:
    [[nodiscard]] std::optional<double> pixel(std::int64_t px, std::int64_t py) const;
    [[nodiscard]] const std::vector<std::int16_t>* tile(std::int64_t tx, std::int64_t ty) const;
    double scale_; // pixels around the world
    TileId min_{}, max_{};
    std::vector<std::vector<std::int16_t>> grid_; // row-major over [min_, max_], empty = no tile; 0.25 m units
};

} // namespace ots::terrain
