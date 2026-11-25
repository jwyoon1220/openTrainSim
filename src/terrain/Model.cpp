#include "terrain/Model.hpp"

#include "phx/core/Log.hpp"
#include "util/Paths.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ranges>

namespace ots::terrain {
namespace {

constexpr double kStep = TrackIndex::kStep;

// Runs of `flag`, with gaps up to `gap` samples merged and runs shorter than `minLen` dropped,
// then grown outwards while `grow(i)` holds.
template <class Grow>
std::vector<SampleRange> findRuns(const std::vector<bool>& flag, std::size_t gap, std::size_t minLen, Grow grow) {
    std::vector<SampleRange> runs;
    for (std::size_t i = 0; i < flag.size();) {
        if (!flag[i]) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < flag.size() && flag[j]) ++j;
        if (!runs.empty() && i - runs.back().end <= gap) runs.back().end = j;
        else runs.push_back({i, j});
        i = j;
    }
    std::erase_if(runs, [&](const SampleRange& r) { return r.size() < minLen; });
    for (SampleRange& r : runs) {
        while (r.begin > 0 && grow(r.begin - 1)) --r.begin;
        while (r.end < flag.size() && grow(r.end)) ++r.end;
    }
    return runs;
}

// Maximal runs where `pred(i)` holds.
template <class Pred>
std::vector<SampleRange> runsWhere(std::size_t n, Pred pred) {
    std::vector<SampleRange> runs;
    for (std::size_t i = 0; i < n;) {
        if (!pred(i)) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < n && pred(j)) ++j;
        runs.push_back({i, j});
        i = j;
    }
    return runs;
}

bool isTunnelObject(const ObjectRecord& r) {
    const std::string p = lower(pathString(r.path));
    return p.find("tunnel") != std::string::npos || p.find("터널") != std::string::npos;
}

} // namespace

TerrainModel::TerrainModel(const TerrainSettings& settings, const Route& route, const ObjectLibrary& lib, TrackIndex track, GeoMapping geo, Dem dem)
    : settings_(settings), route_(route), track_(std::move(track)), geo_(std::move(geo)), dem_(std::move(dem)) {
    fitFormationWidths();
    std::vector<std::optional<double>> centreline(track_.size());
    for (std::size_t i = 0; i < track_.size(); ++i) centreline[i] = demAt(TrackIndex::plan(track_[i].position), TrackIndex::trackPos(i));
    fitHeights(centreline);
    // ground above the rail along the track (sea level at most down to 0)
    std::vector<double> cover(track_.size());
    for (std::size_t i = 0; i < track_.size(); ++i)
        cover[i] = centreline[i] ? std::max(*centreline[i], 0.0) - offset_[i] - track_[i].position.y : 0.0;
    classifyZones(lib, cover);
    if (std::getenv("OTS_TERRAIN_DEBUG")) logDebug(centreline, cover);
}

// openBVE objects (other tracks, platforms, poles, buildings) assume level ground at rail
// height: keep the formation flat out to the farthest object near the track.
void TerrainModel::fitFormationWidths() {
    const std::size_t n = track_.size();
    std::vector<float> widest(n, float(settings_.formationWidth));
    for (const RouteBlock& blk : route_.blocks)
        for (const Placement& p : std::span(route_.placements).subspan(blk.firstPlacement, blk.placementCount)) {
            if (std::abs(p.pos.y) > 15.0f) continue; // overhead / underground things
            const auto hit = track_.nearest(TrackIndex::plan(blk.origin + glm::dvec3(p.pos)), reach());
            if (!hit || std::abs(hit->lateral) > settings_.objectWidth) continue;
            widest[hit->sample] = std::max(widest[hit->sample], float(std::abs(hit->lateral) + 4.0));
        }
    // smooth along the track: running maximum over ±60 m
    constexpr std::size_t r = 6;
    halfWidth_.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto window = std::span(widest).subspan(i > r ? i - r : 0, std::min(n, i + r + 1) - (i > r ? i - r : 0));
        halfWidth_[i] = std::max(float(settings_.formationWidth), std::ranges::max(window));
    }
}

void TerrainModel::fitHeights(std::span<const std::optional<double>> centreline) {
    const std::size_t n = track_.size();
    offset_.assign(n, 0.0f);
    // anchors with a rail altitude: interpolate between them
    std::vector<std::pair<double, double>> known; // (track position, offset)
    for (const auto& a : settings_.anchors)
        if (!std::isnan(a.elevation)) known.emplace_back(a.track, a.elevation - route_.poseAt(a.track).position.y);
    if (!known.empty()) {
        for (std::size_t i = 0; i < n; ++i) {
            const double s = TrackIndex::trackPos(i);
            const auto hi = std::ranges::upper_bound(known, s, {}, &std::pair<double, double>::first);
            double o = hi == known.begin() ? known.front().second : hi == known.end() ? known.back().second : 0.0;
            if (hi != known.begin() && hi != known.end()) {
                const auto lo = std::prev(hi);
                o = lo->second + (hi->second - lo->second) * (s - lo->first) / (hi->first - lo->first);
            }
            offset_[i] = float(o);
        }
        return;
    }
    // Running median of (DEM − rail) along the track: follows the large-scale profile while
    // hills (tunnels, cuttings) and valleys (bridges, embankments) stay.
    const std::size_t half = std::size_t(settings_.verticalWindow / kStep / 2), stride = 25;
    std::vector<double> marks, window;
    for (std::size_t c = 0; c < n; c += stride) {
        window.clear();
        for (std::size_t i = c > half ? c - half : 0; i < std::min(n, c + half); i += 2)
            if (centreline[i] && *centreline[i] > 0.0) window.push_back(*centreline[i] - track_[i].position.y);
        if (window.empty()) {
            marks.push_back(marks.empty() ? 0.0 : marks.back());
            continue;
        }
        const auto mid = window.begin() + std::ptrdiff_t(window.size() / 2);
        std::ranges::nth_element(window, mid);
        marks.push_back(*mid);
    }
    for (std::size_t i = 0; i < n; ++i) {
        const double f = double(i) / stride;
        const std::size_t a = std::min(marks.size() - 1, std::size_t(f)), b = std::min(marks.size() - 1, a + 1);
        offset_[i] = float(marks[a] + (marks[b] - marks[a]) * (f - double(a)));
    }
    phx::log::info("Terrain: rail altitude at the start ≈ {:.0f} m", track_[0].position.y + offset_[0]);
}

void TerrainModel::classifyZones(const ObjectLibrary& lib, std::span<const double> cover) {
    const std::size_t n = track_.size();
    auto samplesOf = [&](double from, double to) { // samples from the one at or before `from` up to `to`
        const std::size_t first = std::size_t(std::max(0.0, std::floor(from / kStep)));
        return std::views::iota(std::min(first, n), std::min(n, std::size_t(std::max(0.0, std::floor(to / kStep))) + 1));
    };
    // blocks where the route draws its own tunnel (Wall / Ground / FreeObj named "tunnel")
    std::vector<bool> routeTunnel(n, false), station(n, false);
    for (std::size_t b = 0; b < route_.blocks.size(); ++b) {
        const RouteBlock& blk = route_.blocks[b];
        const auto placements = std::span(route_.placements).subspan(blk.firstPlacement, blk.placementCount);
        if (std::ranges::any_of(placements, [&](const Placement& p) { return isTunnelObject(lib[p.object]); }))
            for (std::size_t i : samplesOf(double(b) * route_.blockLength, double(b + 1) * route_.blockLength)) routeTunnel[i] = true;
    }
    // stations stay in the open (platforms): no generated tunnel over them
    for (const Station& st : route_.stations) {
        double lo = st.trackPos - 50.0, hi = st.trackPos + 50.0;
        for (const StopPoint& sp : st.stops) lo = std::min(lo, sp.trackPos - 450.0), hi = std::max(hi, sp.trackPos + 50.0);
        for (std::size_t i : samplesOf(lo, hi)) station[i] = true;
    }

    zone_.assign(n, Zone::Open);
    auto mark = [&](const SampleRange& r, Zone z) { std::fill(zone_.begin() + std::ptrdiff_t(r.begin), zone_.begin() + std::ptrdiff_t(r.end), z); };
    if (settings_.tunnels) {
        const auto free = [&](std::size_t i) { return !station[i] && !routeTunnel[i]; };
        std::vector<bool> deep(n);
        for (std::size_t i = 0; i < n; ++i) deep[i] = cover[i] > settings_.tunnelCover && free(i);
        const double portalCover = crown() + 1.5; // portals where the hill covers the arch
        for (const SampleRange& r : findRuns(deep, 8, std::size_t(settings_.tunnelMinLength / kStep), [&](std::size_t i) { return cover[i] > portalCover && free(i); }))
            mark(r, Zone::Tunnel);
    }
    for (std::size_t i = 0; i < n; ++i)
        if (routeTunnel[i]) zone_[i] = Zone::Tunnel;
    {
        std::vector<bool> high(n);
        for (std::size_t i = 0; i < n; ++i) high[i] = zone_[i] == Zone::Open && -cover[i] > settings_.bridgeHeight;
        for (const SampleRange& r : findRuns(high, 4, 2, [&](std::size_t i) { return zone_[i] == Zone::Open && -cover[i] > 0.6 * settings_.bridgeHeight; }))
            mark(r, Zone::Bridge);
    }
    // hand-made corrections from the JSON, last one wins
    using ZT = TerrainSettings::ZoneType;
    for (const TerrainSettings::ZoneOverride& o : settings_.zones)
        for (std::size_t i = std::size_t(std::max(0.0, std::floor(o.from / kStep))); i < n && TrackIndex::trackPos(i) < o.to; ++i) {
            routeTunnel[i] = o.type == ZT::RouteTunnel;
            zone_[i] = o.type == ZT::Tunnel || o.type == ZT::RouteTunnel ? Zone::Tunnel : o.type == ZT::Bridge ? Zone::Bridge : Zone::Open;
        }

    generated_ = runsWhere(n, [&](std::size_t i) { return zone_[i] == Zone::Tunnel && !routeTunnel[i]; });
    bridges_ = runsWhere(n, [&](std::size_t i) { return zone_[i] == Zone::Bridge; }).size();
    routeTunnelLength_ = double(std::ranges::count(routeTunnel, true)) * kStep;
}

void TerrainModel::logDebug(std::span<const std::optional<double>> centreline, std::span<const double> cover) const {
    for (const SampleRange& r : generated_)
        phx::log::info("Terrain: tunnel {:.0f}–{:.0f} m ({:.0f} m, max cover {:.0f} m)", r.from(), r.to(), r.to() - r.from(),
                       std::ranges::max(cover.subspan(r.begin, r.size())));
    for (double s : {525.0, 1000.0, 5000.0, 20000.0}) {
        const std::size_t i = std::size_t(s / kStep);
        if (i >= track_.size()) continue;
        const TrackSample& sp = track_[i];
        for (double d : {0.0, 20.0, 100.0}) {
            const glm::dvec2 p = TrackIndex::plan(sp.position) + sp.side * d;
            const auto hit = track_.nearest(p, reach());
            phx::log::info("terrain dbg s={} d={}: rail {:.1f}, terrain {:.1f} (nearest s {:.1f} d {:.1f}), dem {:.1f}, off {:.1f}, zone {}", s, d,
                           sp.position.y, ground(p).y, hit ? hit->trackPos : 0.0, hit ? hit->lateral : 0.0, centreline[i].value_or(NAN), offset_[i],
                           int(zone_[i]));
        }
    }
}

std::optional<double> TerrainModel::demAt(glm::dvec2 p, double trackPos) const { return dem_.height(geo_.toLatLon(p, trackPos)); }

double TerrainModel::offsetAt(double s) const {
    const double f = std::clamp(s / kStep, 0.0, double(offset_.size() - 1));
    const std::size_t a = std::size_t(f), b = std::min(offset_.size() - 1, a + 1);
    return offset_[a] + (offset_[b] - offset_[a]) * (f - double(a));
}

GroundPoint TerrainModel::ground(glm::dvec2 p) const {
    const auto hit = track_.nearest(p, reach());
    return hit ? shape(p, *hit) : GroundPoint{};
}

GroundPoint TerrainModel::ground(glm::dvec2 p, const TrackIndex::Neighbourhood& near) const {
    const auto hit = track_.nearest(p, near);
    return hit ? shape(p, *hit) : GroundPoint{};
}

GroundPoint TerrainModel::shape(glm::dvec2 p, const TrackHit& hit) const {
    GroundPoint g;
    const double off = offsetAt(hit.trackPos);
    const double formation = hit.railY - settings_.formationDepth;
    double altitude = demAt(p, hit.trackPos).value_or(formation + off);
    if (altitude <= 0.0) g.water = true, altitude = 0.0; // sea: flat at sea level
    double y = altitude - off;
    const double ad = std::abs(hit.lateral);
    const double halfWidth = halfWidth_[hit.sample];
    const double tube = settings_.tunnelRadius + 4.0; // portal opening half width
    switch (zone_[track_.sampleAt(hit.trackPos)]) {
    case Zone::Tunnel: {
        // at least a ridge over the tube (route tunnels where the DEM is low, thin cover)
        const double ridge = hit.railY + crown() + 3.0 - std::max(0.0, ad - tube) * settings_.fillSlope;
        if (ridge > y) y = ridge, g.modified = true;
        g.core = ad < tube;
        break;
    }
    case Zone::Bridge:
        if (ad < halfWidth) y = std::min(y, formation - 1.0);
        g.mouth = ad < tube;
        break;
    case Zone::Open: {
        // flat formation, then cut / fill slopes out to the natural ground
        const double out = ad - halfWidth;
        const double shaped = out <= 0.0 ? formation : std::clamp(y, formation - out * settings_.fillSlope, formation + out * settings_.cutSlope);
        g.modified = std::abs(shaped - y) > 0.05;
        y = shaped;
        g.mouth = ad < tube;
        break;
    }
    }
    g.y = y;
    return g;
}

} // namespace ots::terrain
