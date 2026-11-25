#include "terrain/Terrain.hpp"

#include "phx/core/Log.hpp"
#include "terrain/Geo.hpp"
#include "terrain/Mesh.hpp"
#include "terrain/Model.hpp"
#include "terrain/Track.hpp"
#include "util/Json.hpp"
#include "util/Paths.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <format>
#include <set>

namespace ots {
namespace {

using namespace terrain;

// ------------------------------------------------------------------ settings
Expected<double> stationTrackPos(const Route& route, const Json& station) {
    if (station.isNumber()) {
        const double k = station.number();
        if (k < 0 || std::size_t(k) >= route.stations.size()) return Unexpected(std::format("no station {}", k));
        return route.stations[std::size_t(k)].trackPos;
    }
    const std::string want = lower(station.string());
    const auto it = std::ranges::find_if(route.stations, [&](const Station& s) { return lower(s.name).find(want) != std::string::npos; });
    if (it == route.stations.end()) return Unexpected(std::format("no station named '{}'", station.string()));
    return it->trackPos;
}

Expected<TerrainSettings::Anchor> parseAnchor(const Route& route, const Json& a) {
    TerrainSettings::Anchor an;
    if (a.has("track")) {
        an.track = a.num("track", 0.0);
    } else if (a["station"].isNumber() || a["station"].isString()) {
        const auto s = stationTrackPos(route, a["station"]);
        if (!s) return Unexpected(s.error());
        an.track = *s;
    } else {
        return Unexpected("needs \"track\" (m) or \"station\"");
    }
    if (!a["lat"].isNumber() || !a["lon"].isNumber()) return Unexpected("needs \"lat\" and \"lon\"");
    an.lat = a.num("lat", 0.0), an.lon = a.num("lon", 0.0);
    if (a["elevation"].isNumber()) an.elevation = a.num("elevation", 0.0);
    return an;
}

Expected<TerrainSettings::ZoneOverride> parseZone(const Json& z) {
    using ZT = TerrainSettings::ZoneType;
    static constexpr std::pair<std::string_view, ZT> kTypes[] = {{"open", ZT::Open}, {"tunnel", ZT::Tunnel}, {"routetunnel", ZT::RouteTunnel}, {"bridge", ZT::Bridge}};
    const std::string type = lower(z.str("type"));
    const auto it = std::ranges::find(kTypes, std::string_view(type), &std::pair<std::string_view, ZT>::first);
    if (it == std::end(kTypes)) return Unexpected("type must be open, tunnel, routeTunnel or bridge");
    TerrainSettings::ZoneOverride o{z.num("from", 0.0), z.num("to", 0.0), it->second};
    if (!(o.to > o.from)) return Unexpected("needs \"from\" < \"to\" (track positions, m)");
    return o;
}

// ------------------------------------------------------------------ DEM coverage
// Tiles under the corridor: across the track out to both edges, every 50 m along it.
std::vector<TileId> demTilesFor(const TrackIndex& track, const GeoMapping& geo, const TerrainSettings& s) {
    std::set<std::pair<int, int>> need; // ordered: deterministic tile order
    const double reach = s.width + s.tileSize * 1.5;
    for (std::size_t i = 0; i < track.size(); i += 5) {
        const TrackSample& sp = track[i];
        int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
        for (double across : {-reach, 0.0, reach}) {
            const TileId t = TileId::of(geo.toLatLon(TrackIndex::plan(sp.position) + sp.side * across, TrackIndex::trackPos(i)), s.zoom);
            x0 = std::min(x0, t.x), y0 = std::min(y0, t.y), x1 = std::max(x1, t.x), y1 = std::max(y1, t.y);
        }
        for (int x = x0 - 1; x <= x1 + 1; ++x)
            for (int y = y0 - 1; y <= y1 + 1; ++y) need.emplace(x, y);
    }
    std::vector<TileId> tiles;
    tiles.reserve(need.size());
    for (auto [x, y] : need) tiles.push_back({x, y});
    return tiles;
}

// ------------------------------------------------------------------ placements
// Collects new placements per route block, then splices them into route.placements.
class PlacementBuilder {
public:
    explicit PlacementBuilder(Route& route) : route_(route), extra_(route.blocks.size()) {}

    void add(ObjectId id, const glm::dvec3& origin, double trackPos) {
        const std::size_t b = std::size_t(std::clamp(route_.blockAt(trackPos), 0, int(route_.blocks.size()) - 1));
        Placement p;
        p.object = id;
        p.pos = glm::vec3(origin - route_.blocks[b].origin);
        p.trackOffset = f32(trackPos - double(b) * route_.blockLength);
        extra_[b].push_back(p);
    }

    void commit() && {
        std::size_t added = 0;
        for (const auto& e : extra_) added += e.size();
        std::vector<Placement> merged;
        merged.reserve(route_.placements.size() + added);
        for (std::size_t b = 0; b < route_.blocks.size(); ++b) {
            RouteBlock& blk = route_.blocks[b];
            const u32 first = u32(merged.size());
            const auto own = std::span(route_.placements).subspan(blk.firstPlacement, blk.placementCount);
            merged.insert(merged.end(), own.begin(), own.end());
            merged.insert(merged.end(), extra_[b].begin(), extra_[b].end());
            blk.firstPlacement = first;
            blk.placementCount = u32(merged.size()) - first;
        }
        route_.placements = std::move(merged);
    }

private:
    Route& route_;
    std::vector<std::vector<Placement>> extra_;
};

using ModelPtr = std::shared_ptr<const TerrainModel>;
using TexturesPtr = std::shared_ptr<const TerrainTextures>;

// Registers a procedural object whose mesh `make()` builds on a loader thread.
template <class Make>
ObjectId procedural(ObjectLibrary& lib, const std::string& key, Make make) {
    return lib.addProcedural(key, std::move(make));
}

std::size_t addTiles(ObjectLibrary& lib, PlacementBuilder& out, const ModelPtr& model, const TexturesPtr& tex, const std::string& tag) {
    const TerrainSettings& s = model->settings();
    const TrackIndex& track = model->track();
    const double ts = s.tileSize;
    // tiles within `width` of the track (sampled every 50 m), in a stable order
    std::set<std::pair<std::int64_t, std::int64_t>> tiles;
    const int r = int(std::ceil(s.width / ts)) + 1;
    for (std::size_t i = 0; i < track.size(); i += 5) {
        const glm::dvec2 p = TrackIndex::plan(track[i].position);
        const std::int64_t cx = std::int64_t(std::floor(p.x / ts)), cz = std::int64_t(std::floor(p.y / ts));
        for (std::int64_t x = cx - r; x <= cx + r; ++x)
            for (std::int64_t z = cz - r; z <= cz + r; ++z) {
                const glm::dvec2 lo(double(x) * ts, double(z) * ts);
                const glm::dvec2 gap = glm::max(glm::max(lo - p, p - (lo + ts)), glm::dvec2(0.0)); // point → square
                if (glm::dot(gap, gap) < s.width * s.width) tiles.emplace(x, z);
            }
    }
    std::size_t count = 0;
    for (auto [x, z] : tiles) {
        TileSpec spec;
        spec.corner = {double(x) * ts, double(z) * ts};
        spec.size = ts;
        const glm::dvec2 centre = spec.corner + glm::dvec2(0.5 * ts);
        const auto hit = track.nearest(centre, model->reach());
        if (!hit) continue;
        spec.spacing = std::abs(hit->lateral) - 0.71 * ts < s.nearDistance ? s.nearSpacing : s.farSpacing;
        spec.origin = {centre.x, hit->railY, centre.y};
        const ObjectId id = procedural(lib, std::format("terrain/{}/{}_{}", tag, x, z), [model, tex, spec] { return buildTile(*model, *tex, spec); });
        out.add(id, spec.origin, hit->trackPos);
        ++count;
    }
    return count;
}

void addTunnels(ObjectLibrary& lib, PlacementBuilder& out, const ModelPtr& model, const TexturesPtr& tex, const std::string& tag) {
    constexpr double kLiningChunk = 200.0; // m per object (streaming / culling granularity)
    const Route& route = model->route();
    for (const SampleRange& r : model->generatedTunnels()) {
        const double from = r.from(), to = std::min(route.endPosition() - 1.0, r.to());
        for (double c = from; c < to; c += kLiningChunk) {
            const double c1 = std::min(to, c + kLiningChunk);
            const glm::dvec3 origin = route.poseAt(c).position;
            out.add(procedural(lib, std::format("tunnel/{}/{:.0f}", tag, c), [=] { return buildLining(*model, *tex, c, c1, origin); }), origin, c);
        }
        for (const auto& [s, entrance] : {std::pair{from, true}, std::pair{to, false}}) {
            const glm::dvec3 origin = route.poseAt(s).position;
            out.add(procedural(lib, std::format("portal/{}/{:.0f}", tag, s), [=, s = s, entrance = entrance] { return buildPortal(*model, *tex, s, entrance, origin); }),
                    origin, s);
        }
    }
}

} // namespace

std::filesystem::path findTerrainFile(const std::filesystem::path& routeFile) {
    std::string base = routeFile.stem().string();
    if (base.ends_with(".route")) base.resize(base.size() - 6); // native "<name>.route.json" → <name>.terrain.json
    return resolvePath(routeFile.parent_path(), base + ".terrain.json");
}

Expected<TerrainSettings> loadTerrainSettings(const std::filesystem::path& file, const Route& route) {
    const auto parsed = Json::parseFile(file);
    if (!parsed) return Unexpected(parsed.error());
    const Json& j = *parsed;
    TerrainSettings out;
    for (std::size_t i = 0; i < j["anchors"].size(); ++i) {
        const auto a = parseAnchor(route, j["anchors"][i]);
        if (!a) return Unexpected(std::format("anchors[{}]: {}", i, a.error()));
        out.anchors.push_back(*a);
    }
    if (out.anchors.empty()) return Unexpected("needs at least one anchor");
    std::ranges::sort(out.anchors, {}, &TerrainSettings::Anchor::track);
    if (j["heading"].isNumber()) out.heading = j.num("heading", 0.0);
    if (out.anchors.size() == 1 && std::isnan(out.heading)) return Unexpected("one anchor needs \"heading\" (degrees, north = 0, east = 90)");
    out.dem = j.str("dem", out.dem);
    out.zoom = std::clamp(int(j.num("zoom", out.zoom)), 8, 15);
    out.width = std::clamp(j.num("width", out.width), 100.0, 10000.0);
    out.verticalWindow = std::max(500.0, j.num("verticalWindow", out.verticalWindow));
    const Json& f = j["formation"];
    out.formationWidth = f.num("width", out.formationWidth);
    out.formationDepth = f.num("depth", out.formationDepth);
    out.objectWidth = std::max(0.0, f.num("objectWidth", out.objectWidth));
    out.cutSlope = std::max(0.1, f.num("cutSlope", out.cutSlope));
    out.fillSlope = std::max(0.1, f.num("fillSlope", out.fillSlope));
    out.bridgeHeight = j.num("bridgeHeight", out.bridgeHeight);
    const Json& t = j["tunnels"]; // an object, or just true / false
    out.tunnels = t.isBool() ? t.boolean() : t.flag("enabled", true);
    out.tunnelCover = t.num("cover", out.tunnelCover);
    out.tunnelMinLength = t.num("minLength", out.tunnelMinLength);
    out.tunnelRadius = t.num("radius", out.tunnelRadius);
    for (std::size_t i = 0; i < j["zones"].size(); ++i) {
        const auto z = parseZone(j["zones"][i]);
        if (!z) return Unexpected(std::format("zones[{}]: {}", i, z.error()));
        out.zones.push_back(*z);
    }
    const Json& m = j["mesh"];
    out.nearSpacing = std::clamp(m.num("nearSpacing", out.nearSpacing), 1.0, 64.0);
    out.farSpacing = std::clamp(m.num("farSpacing", out.farSpacing), out.nearSpacing, 128.0);
    out.nearDistance = m.num("nearDistance", out.nearDistance);
    return out;
}

Expected<TerrainStats> buildTerrain(Route& route, ObjectLibrary& lib, const TerrainSettings& settings, const std::filesystem::path& settingsDir) {
    if (route.blocks.empty()) return Unexpected("empty route");
    TrackIndex track(route);
    auto geo = GeoMapping::fit(route, settings);
    if (!geo) return Unexpected(geo.error());
    TerrainStats stats;

    // elevation data
    const std::vector<TileId> tiles = demTilesFor(track, *geo, settings);
    const DemSource source(settings.dem, settings.zoom, settingsDir);
    stats.demTiles = tiles.size();
    stats.demDownloaded = source.fetchMissing(tiles).fetched;
    Dem dem(settings.zoom, tiles);
    stats.demMissing = std::size_t(std::ranges::count_if(tiles, [&](TileId t) { return !dem.load(t, source.file(t)); }));
    if (stats.demMissing == tiles.size()) return Unexpected(std::format("no DEM tiles could be read ({})", settings.dem));
    if (stats.demMissing) phx::log::warn("Terrain: {} of {} DEM tiles missing (flat there)", stats.demMissing, tiles.size());

    const auto model = std::make_shared<const TerrainModel>(settings, route, lib, std::move(track), std::move(*geo), std::move(dem));
    const auto textures = std::make_shared<const TerrainTextures>(TerrainTextures::ensure());
    stats.tunnels = model->generatedTunnels().size();
    for (const SampleRange& r : model->generatedTunnels()) stats.tunnelLength += r.to() - r.from();
    stats.routeTunnelLength = model->routeTunnelLength();
    stats.bridges = model->bridgeCount();

    PlacementBuilder placements(route);
    const std::string tag = std::format("{:x}", std::hash<std::string>{}(pathString(route.file)));
    stats.tiles = addTiles(lib, placements, model, textures, tag);
    addTunnels(lib, placements, model, textures, tag);
    std::move(placements).commit();
    return stats;
}

} // namespace ots
