#pragma once
// Terrain internals: rail 0 sampled every kStep metres with a uniform-grid spatial index for
// "closest point of the track" queries.
#include "bve/Route.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace ots::terrain {

struct TrackSample {
    glm::dvec3 position{0.0}; // rail 0
    glm::dvec2 side{1.0, 0.0}; // horizontal unit vector to the right of the track
};

// Closest point of rail 0 (in plan) to a query point.
struct TrackHit {
    double trackPos = 0.0;  // m
    double lateral = 0.0;   // signed distance, + to the right
    double railY = 0.0;     // rail height there
    std::size_t sample = 0; // nearest sample index
};

class TrackIndex {
public:
    static constexpr double kStep = 10.0;

    explicit TrackIndex(const Route& route);

    [[nodiscard]] std::size_t size() const { return samples_.size(); }
    [[nodiscard]] const TrackSample& operator[](std::size_t i) const { return samples_[i]; }
    [[nodiscard]] static double trackPos(std::size_t i) { return double(i) * kStep; }
    [[nodiscard]] std::size_t sampleAt(double trackPos) const; // rounded, clamped
    [[nodiscard]] static glm::dvec2 plan(const glm::dvec3& p) { return {p.x, p.z}; }

    // Searches outwards ring by ring up to `maxDistance`.
    [[nodiscard]] std::optional<TrackHit> nearest(glm::dvec2 p, double maxDistance) const;

    // The samples that can be nearest to any point within `radius` of `centre`: one grid query
    // per terrain tile, then a linear scan per vertex over a short, cache-friendly list.
    class Neighbourhood {
    public:
        [[nodiscard]] bool empty() const { return ids_.empty(); }

    private:
        friend class TrackIndex;
        std::vector<std::uint32_t> ids_;
        std::vector<glm::dvec2> points_;
    };
    [[nodiscard]] Neighbourhood neighbourhood(glm::dvec2 centre, double radius) const;
    [[nodiscard]] std::optional<TrackHit> nearest(glm::dvec2 p, const Neighbourhood& n) const;

private:
    struct Cell {
        std::int64_t x, z;
        bool operator==(const Cell&) const = default;
    };
    struct CellHash {
        std::size_t operator()(const Cell& c) const noexcept { return std::size_t(c.x * 73856093) ^ std::size_t(c.z * 19349663); }
    };
    static constexpr double kCell = 128.0;
    [[nodiscard]] static Cell cellOf(glm::dvec2 p) { return {std::int64_t(std::floor(p.x / kCell)), std::int64_t(std::floor(p.y / kCell))}; }
    // Exact closest point on the two segments around the nearest sample.
    [[nodiscard]] TrackHit refine(glm::dvec2 p, std::size_t best, double bestDistSq) const;

    std::vector<TrackSample> samples_;
    std::unordered_map<Cell, std::vector<std::uint32_t>, CellHash> grid_;
};

} // namespace ots::terrain
