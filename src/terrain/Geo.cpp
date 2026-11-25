#include "terrain/Geo.hpp"

#include "phx/core/Log.hpp"
#include "util/Image.hpp"
#include "util/Paths.hpp"
#include "util/Platform.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <numbers>
#include <thread>

namespace ots::terrain {
namespace {

constexpr double kEarthRadius = 6378137.0;
constexpr double kDeg = std::numbers::pi / 180.0;
constexpr int kTilePixels = 256;

double mercatorY(double latDeg) { // 0 at the top of the world, 1 at the bottom
    const double r = latDeg * kDeg;
    return (1.0 - std::log(std::tan(r) + 1.0 / std::cos(r)) / std::numbers::pi) * 0.5;
}

glm::dvec2 horizontal(const glm::dvec3& v) { return {v.x, v.z}; }

} // namespace

// ------------------------------------------------------------------ GeoMapping

glm::dvec2 GeoMapping::Span::apply(glm::dvec2 p) const {
    const glm::dvec2 d = p - world0;
    return map0 + glm::dvec2(c * d.x - s * d.y, s * d.x + c * d.y);
}

glm::dvec2 GeoMapping::metricOf(LatLon p) const {
    return {(p.lon - origin_.lon) * kDeg * kEarthRadius * std::cos(p.lat * kDeg), (p.lat - origin_.lat) * kDeg * kEarthRadius};
}

Expected<GeoMapping> GeoMapping::fit(const Route& route, const TerrainSettings& settings) {
    const auto& anchors = settings.anchors;
    if (anchors.empty()) return Unexpected("needs at least one anchor");
    GeoMapping m;
    m.origin_ = {anchors.front().lat, anchors.front().lon};
    auto worldAt = [&](double s) { return horizontal(route.poseAt(s).position); };

    if (anchors.size() == 1) {
        // world (x, z) and (east, north) as complex numbers: rotate the track direction onto
        // the heading (compass degrees, clockwise from north)
        const glm::dvec3 dir = route.poseAt(anchors.front().track).direction;
        const double phi = (90.0 - settings.heading) * kDeg - std::atan2(dir.z, dir.x);
        m.spans_.push_back({-std::numeric_limits<double>::infinity(), worldAt(anchors.front().track), glm::dvec2(0.0), std::cos(phi), std::sin(phi)});
        return m;
    }
    double shortest = std::numeric_limits<double>::infinity();
    for (std::size_t k = 0; k + 1 < anchors.size(); ++k) {
        const auto &a = anchors[k], &b = anchors[k + 1];
        const glm::dvec2 w0 = worldAt(a.track), w = worldAt(b.track) - w0;
        const glm::dvec2 g0 = m.metricOf({a.lat, a.lon}), g = m.metricOf({b.lat, b.lon}) - g0;
        if (glm::length(w) < 1.0) return Unexpected(std::format("anchors {} and {} are at the same place", k, k + 1));
        const double sigma = glm::length(g) / glm::length(w);
        const double phi = std::atan2(g.y, g.x) - std::atan2(w.y, w.x);
        m.spans_.push_back({k == 0 ? -std::numeric_limits<double>::infinity() : a.track, w0, g0, sigma * std::cos(phi), sigma * std::sin(phi)});
        shortest = std::min(shortest, b.track - a.track);
        if (std::abs(sigma - 1.0) > 0.1)
            phx::log::warn("Terrain: anchors {}-{} scale {:.2f} (the route is {:.0f}% {} than the map there; check the anchors)", k, k + 1, sigma,
                           std::abs(1.0 / sigma - 1.0) * 100.0, sigma < 1.0 ? "longer" : "shorter");
        else
            phx::log::info("Terrain: anchors {}-{} scale {:.3f}, rotation {:.1f}°", k, k + 1, sigma, phi / kDeg);
    }
    m.blend_ = std::clamp(0.25 * shortest, 50.0, 3000.0);
    return m;
}

glm::dvec2 GeoMapping::toMetric(glm::dvec2 world, double s) const {
    // span containing s (spans_[0] starts at -inf)
    const auto it = std::upper_bound(spans_.begin() + 1, spans_.end(), s, [](double v, const Span& sp) { return v < sp.start; });
    const std::size_t k = std::size_t(it - spans_.begin()) - 1;
    const glm::dvec2 g = spans_[k].apply(world);
    // halfway blend into the neighbouring span around interior anchors
    if (k + 1 < spans_.size() && spans_[k + 1].start - s < blend_)
        return glm::mix(g, spans_[k + 1].apply(world), 0.5 * (1.0 - (spans_[k + 1].start - s) / blend_));
    if (k > 0 && s - spans_[k].start < blend_) return glm::mix(g, spans_[k - 1].apply(world), 0.5 * (1.0 - (s - spans_[k].start) / blend_));
    return g;
}

LatLon GeoMapping::toLatLon(glm::dvec2 world, double trackPos) const {
    const glm::dvec2 m = toMetric(world, trackPos);
    const double lat = origin_.lat + m.y / kEarthRadius / kDeg;
    return {lat, origin_.lon + m.x / (kEarthRadius * std::cos(lat * kDeg)) / kDeg};
}

// ------------------------------------------------------------------ tiles

TileId TileId::of(LatLon p, int zoom) {
    const double n = std::ldexp(1.0, zoom);
    return {int(std::floor((p.lon + 180.0) / 360.0 * n)), int(std::floor(mercatorY(p.lat) * n))};
}

DemSource::DemSource(std::string pattern, int zoom, const std::filesystem::path& settingsDir)
    : pattern_(std::move(pattern)), zoom_(zoom), remote_(pattern_.starts_with("http://") || pattern_.starts_with("https://")), baseDir_(settingsDir) {
    if (remote_) cacheDir_ = userCacheDir() / "dem" / std::format("{:016x}", std::hash<std::string>{}(pattern_));
}

std::string DemSource::expand(TileId t) const {
    std::string s = pattern_;
    for (auto [key, value] : {std::pair{std::string_view("{z}"), zoom_}, {"{x}", t.x}, {"{y}", t.y}})
        for (std::size_t p; (p = s.find(key)) != std::string::npos;) s.replace(p, key.size(), std::to_string(value));
    return s;
}

std::filesystem::path DemSource::file(TileId t) const {
    if (remote_) return cacheDir_ / std::to_string(zoom_) / std::to_string(t.x) / (std::to_string(t.y) + ".png");
    return combinePath(baseDir_, expand(t));
}

std::string DemSource::url(TileId t) const { return remote_ ? expand(t) : std::string(); }

DemSource::FetchResult DemSource::fetchMissing(std::span<const TileId> tiles) const {
    FetchResult result;
    if (!remote_) return result;
    std::vector<TileId> missing;
    std::ranges::copy_if(tiles, std::back_inserter(missing), [&](TileId t) { return !fileExists(file(t)); });
    if (missing.empty()) return result;
    phx::log::info("Terrain: downloading {} DEM tiles (zoom {}) to {}", missing.size(), zoom_, pathString(cacheDir_));
    std::atomic<std::size_t> next{0}, done{0}, failed{0};
    auto worker = [&] {
        for (std::size_t i; (i = next++) < missing.size();) {
            const std::filesystem::path target = file(missing[i]), part = target.string() + ".part";
            std::error_code ec;
            std::filesystem::create_directories(target.parent_path(), ec);
            const int rc = runProcess({"curl", "-sfL", "--retry", "2", "--connect-timeout", "15", "-o", pathString(part), url(missing[i])});
            if (rc == 0) std::filesystem::rename(part, target, ec);
            if (rc != 0 || ec) {
                std::filesystem::remove(part, ec);
                if (++failed == 1)
                    phx::log::warn("Terrain: download failed ({}): {}", rc < 0 ? std::string("curl not found") : std::format("curl exit {}", rc), url(missing[i]));
            }
            if (const std::size_t d = ++done; d % 50 == 0) phx::log::info("Terrain: {}/{} tiles", d, missing.size());
        }
    };
    {
        std::vector<std::jthread> threads;
        for (int k = 0; k < 8; ++k) threads.emplace_back(worker);
    } // joined
    result.failed = failed;
    result.fetched = missing.size() - result.failed;
    return result;
}

// ------------------------------------------------------------------ Dem

Dem::Dem(int zoom, std::span<const TileId> tiles) : scale_(std::ldexp(double(kTilePixels), zoom)) {
    if (tiles.empty()) return;
    min_ = max_ = tiles.front();
    for (TileId t : tiles) {
        min_ = {std::min(min_.x, t.x), std::min(min_.y, t.y)};
        max_ = {std::max(max_.x, t.x), std::max(max_.y, t.y)};
    }
    grid_.resize(std::size_t(max_.x - min_.x + 1) * std::size_t(max_.y - min_.y + 1));
}

bool Dem::load(TileId t, const std::filesystem::path& file) {
    if (t.x < min_.x || t.x > max_.x || t.y < min_.y || t.y > max_.y) return false;
    const auto img = loadRgbImage(file);
    if (!img || img->width != kTilePixels || img->height != kTilePixels) return false;
    std::vector<std::int16_t> heights(kTilePixels * kTilePixels);
    const u8* px = img->pixels.data();
    for (std::size_t i = 0; i < heights.size(); ++i, px += 3) {
        const double metres = px[0] * 256.0 + px[1] + px[2] / 256.0 - 32768.0; // Terrarium encoding
        heights[i] = std::int16_t(std::clamp(std::lround(metres * 4.0), -32000L, 32000L));
    }
    grid_[std::size_t(t.y - min_.y) * std::size_t(max_.x - min_.x + 1) + std::size_t(t.x - min_.x)] = std::move(heights);
    return true;
}

const std::vector<std::int16_t>* Dem::tile(std::int64_t tx, std::int64_t ty) const {
    if (tx < min_.x || tx > max_.x || ty < min_.y || ty > max_.y) return nullptr;
    const auto& t = grid_[std::size_t(ty - min_.y) * std::size_t(max_.x - min_.x + 1) + std::size_t(tx - min_.x)];
    return t.empty() ? nullptr : &t;
}

std::optional<double> Dem::pixel(std::int64_t px, std::int64_t py) const {
    const auto* t = tile(px >> 8, py >> 8);
    if (!t) return std::nullopt;
    return (*t)[std::size_t((py & 255) * kTilePixels + (px & 255))] * 0.25;
}

std::optional<double> Dem::height(LatLon p) const {
    const double fx = (p.lon + 180.0) / 360.0 * scale_ - 0.5, fy = mercatorY(p.lat) * scale_ - 0.5;
    const double x0 = std::floor(fx), y0 = std::floor(fy), ax = fx - x0, ay = fy - y0;
    std::array<std::optional<double>, 4> v;
    std::optional<double> any;
    for (std::size_t k = 0; k < v.size(); ++k)
        if ((v[k] = pixel(std::int64_t(x0) + std::int64_t(k & 1), std::int64_t(y0) + std::int64_t(k >> 1)))) any = v[k];
    if (!any) return std::nullopt;
    auto at = [&](std::size_t k) { return v[k].value_or(*any); }; // edge of the loaded area: repeat
    return (at(0) * (1 - ax) + at(1) * ax) * (1 - ay) + (at(2) * (1 - ax) + at(3) * ax) * ay;
}

} // namespace ots::terrain
