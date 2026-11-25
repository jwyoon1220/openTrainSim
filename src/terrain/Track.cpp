#include "terrain/Track.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ots::terrain {

TrackIndex::TrackIndex(const Route& route) {
    const double end = route.endPosition();
    samples_.reserve(std::size_t(end / kStep) + 1);
    for (double s = 0.0; s <= end; s += kStep) {
        const TrackPose p = route.poseAt(s);
        const glm::dvec2 side = plan(p.side);
        samples_.push_back({p.position, glm::length(side) > 1e-9 ? glm::normalize(side) : glm::dvec2(1.0, 0.0)});
    }
    for (std::uint32_t i = 0; i < samples_.size(); ++i) grid_[cellOf(plan(samples_[i].position))].push_back(i);
}

std::size_t TrackIndex::sampleAt(double s) const {
    return std::min(samples_.size() - 1, std::size_t(std::max(0.0, std::round(s / kStep))));
}

std::optional<TrackHit> TrackIndex::nearest(glm::dvec2 p, double maxDistance) const {
    const Cell c = cellOf(p);
    const int rings = int(std::ceil(maxDistance / kCell)) + 2;
    double best = std::numeric_limits<double>::infinity();
    std::size_t bestId = 0;
    for (int r = 0; r <= rings; ++r) {
        for (std::int64_t x = c.x - r; x <= c.x + r; ++x) {
            const bool edgeColumn = x == c.x - r || x == c.x + r;
            for (std::int64_t z = c.z - r; z <= c.z + r; z += edgeColumn ? 1 : 2 * std::max(r, 1)) { // ring cells only
                const auto it = grid_.find({x, z});
                if (it == grid_.end()) continue;
                for (std::uint32_t i : it->second) {
                    const glm::dvec2 d = plan(samples_[i].position) - p;
                    if (const double q = glm::dot(d, d); q < best) best = q, bestId = i;
                }
            }
        }
        if (best < std::numeric_limits<double>::infinity() && std::sqrt(best) < double(r) * kCell) break;
    }
    if (best == std::numeric_limits<double>::infinity()) return std::nullopt;
    return refine(p, bestId, best);
}

TrackIndex::Neighbourhood TrackIndex::neighbourhood(glm::dvec2 centre, double radius) const {
    Neighbourhood n;
    const Cell lo = cellOf(centre - glm::dvec2(radius)), hi = cellOf(centre + glm::dvec2(radius));
    for (std::int64_t x = lo.x; x <= hi.x; ++x)
        for (std::int64_t z = lo.z; z <= hi.z; ++z) {
            const auto it = grid_.find({x, z});
            if (it == grid_.end()) continue;
            for (std::uint32_t i : it->second) {
                const glm::dvec2 q = plan(samples_[i].position);
                if (glm::distance(q, centre) <= radius) n.ids_.push_back(i), n.points_.push_back(q);
            }
        }
    return n;
}

std::optional<TrackHit> TrackIndex::nearest(glm::dvec2 p, const Neighbourhood& n) const {
    double best = std::numeric_limits<double>::infinity();
    std::size_t bestAt = 0;
    for (std::size_t k = 0; k < n.points_.size(); ++k) {
        const glm::dvec2 d = n.points_[k] - p;
        // ties go to the lower sample index, like the ring search's ordered buckets
        if (const double q = glm::dot(d, d); q < best || (q == best && n.ids_[k] < n.ids_[bestAt])) best = q, bestAt = k;
    }
    if (n.points_.empty()) return std::nullopt;
    return refine(p, n.ids_[bestAt], best);
}

TrackHit TrackIndex::refine(glm::dvec2 p, std::size_t best, double bestDistSq) const {
    TrackHit hit;
    hit.trackPos = trackPos(best);
    hit.railY = samples_[best].position.y;
    for (std::size_t a = best > 0 ? best - 1 : 0; a <= best && a + 1 < samples_.size(); ++a) {
        const glm::dvec3 &A = samples_[a].position, &B = samples_[a + 1].position;
        const glm::dvec2 e = plan(B) - plan(A);
        const double len2 = glm::dot(e, e);
        if (len2 < 1e-9) continue;
        const double t = std::clamp(glm::dot(p - plan(A), e) / len2, 0.0, 1.0);
        const glm::dvec2 d = plan(A) + e * t - p;
        if (const double q = glm::dot(d, d); q <= bestDistSq) {
            bestDistSq = q;
            hit.trackPos = (double(a) + t) * kStep;
            hit.railY = A.y + (B.y - A.y) * t;
        }
    }
    hit.sample = sampleAt(hit.trackPos);
    // signed lateral distance to the interpolated track point, against the local side vector
    const std::size_t a = std::min(samples_.size() - 1, std::size_t(std::floor(hit.trackPos / kStep)));
    const std::size_t b = std::min(samples_.size() - 1, a + 1);
    const double t = std::clamp(hit.trackPos / kStep - double(a), 0.0, 1.0);
    const glm::dvec2 onTrack = glm::mix(plan(samples_[a].position), plan(samples_[b].position), t);
    hit.lateral = glm::dot(p - onTrack, samples_[hit.sample].side);
    return hit;
}

} // namespace ots::terrain
