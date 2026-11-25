#include "route/NativeRoute.hpp"

#include "bve/FunctionScript.hpp"
#include "sim/Traffic.hpp"
#include "util/Json.hpp"
#include "util/Text.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>

namespace ots {
namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

glm::dvec2 rotate2(glm::dvec2 v, double c, double s) { return {v.x * c - v.y * s, v.x * s + v.y * c}; }

u32 rgb(u8 r, u8 g, u8 b, u8 a = 255) { return u32(r) | (u32(g) << 8) | (u32(b) << 16) | (u32(a) << 24); }

// ---------------------------------------------------------------- mesh helpers

struct MeshMaker {
    StaticMesh mesh;
    SubMesh& part(u32 color, u32 emissive = 0) {
        for (SubMesh& p : mesh.parts)
            if (p.material.color == color && p.material.emissive == emissive) return p;
        SubMesh& p = mesh.parts.emplace_back();
        p.material.color = color;
        p.material.emissive = emissive;
        return p;
    }
    void quad(SubMesh& p, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d) { // counter-clockwise seen from outside
        const glm::vec3 n = glm::normalize(glm::cross(b - a, d - a));
        const u32 base = u32(p.vertices.size());
        for (const glm::vec3& v : {a, b, c, d}) p.vertices.push_back({v, n, glm::vec2(0.0f)});
        p.indices.insert(p.indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
    }
    // Axis-aligned box (x right, y up, z forward).
    void box(glm::vec3 lo, glm::vec3 hi, u32 color, u32 emissive = 0) {
        SubMesh& p = part(color, emissive);
        const glm::vec3 c[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
                                {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
        quad(p, c[0], c[3], c[2], c[1]); // back (-z)
        quad(p, c[4], c[5], c[6], c[7]); // front (+z)
        quad(p, c[0], c[4], c[7], c[3]); // left
        quad(p, c[1], c[2], c[6], c[5]); // right
        quad(p, c[3], c[7], c[6], c[2]); // top
        quad(p, c[0], c[1], c[5], c[4]); // bottom
    }
    StaticMesh take() {
        mesh.computeBounds();
        return std::move(mesh);
    }
};

// One block of track: ballast, concrete sleepers every 0.6 m, two rails, a strip of ground.
StaticMesh trackMesh(double length, double gauge) {
    MeshMaker m;
    const f32 L = f32(length), half = f32(gauge) * 0.5f;
    SubMesh& ground = m.part(rgb(96, 120, 70));
    // ground to the horizon (no terrain yet); slightly overlapping blocks hide seams on curves
    m.quad(ground, {-3000.0f, -0.76f, -1.0f}, {-2.6f, -0.75f, -1.0f}, {-2.6f, -0.75f, L + 1.0f}, {-3000.0f, -0.76f, L + 1.0f});
    m.quad(ground, {2.6f, -0.75f, -1.0f}, {3000.0f, -0.76f, -1.0f}, {3000.0f, -0.76f, L + 1.0f}, {2.6f, -0.75f, L + 1.0f});
    SubMesh& ballast = m.part(rgb(128, 122, 112));
    m.quad(ballast, {-1.6f, -0.32f, 0.0f}, {1.6f, -0.32f, 0.0f}, {1.6f, -0.32f, L}, {-1.6f, -0.32f, L});     // top
    m.quad(ballast, {-2.6f, -0.75f, 0.0f}, {-1.6f, -0.32f, 0.0f}, {-1.6f, -0.32f, L}, {-2.6f, -0.75f, L});   // left shoulder
    m.quad(ballast, {1.6f, -0.32f, 0.0f}, {2.6f, -0.75f, 0.0f}, {2.6f, -0.75f, L}, {1.6f, -0.32f, L});       // right shoulder
    for (f32 z = 0.3f; z < L; z += 0.6f) m.box({-1.25f, -0.33f, z - 0.12f}, {1.25f, -0.17f, z + 0.12f}, rgb(170, 168, 160));
    for (const f32 x : {-half - 0.036f, half - 0.036f}) {
        m.box({x, -0.17f, 0.0f}, {x + 0.072f, -0.03f, L}, rgb(92, 70, 56)); // web + foot (rusty)
        m.box({x + 0.004f, -0.03f, 0.0f}, {x + 0.068f, 0.0f, L}, rgb(190, 192, 196)); // polished head
    }
    return m.take();
}

// Korean colour-light signal, 4 lamps top→bottom Y, G, R, Y: one mesh per aspect
// (0 R, 1 YY, 2 Y, 3 YG, 4 G). The mast stands at the origin, lamps face −z (the train).
StaticMesh signalMesh(int aspect) {
    MeshMaker m;
    m.box({-0.08f, 0.0f, -0.08f}, {0.08f, 5.0f, 0.08f}, rgb(150, 150, 150)); // mast
    m.box({-0.24f, 3.4f, -0.30f}, {0.24f, 5.2f, -0.08f}, rgb(20, 20, 22)); // head
    const std::array<u32, 4> on{rgb(255, 200, 50), rgb(70, 255, 150), rgb(255, 50, 60), rgb(255, 200, 50)};
    const std::array<u32, 4> off{rgb(60, 50, 25), rgb(20, 55, 35), rgb(60, 20, 20), rgb(60, 50, 25)};
    constexpr std::array<std::array<bool, 4>, 5> lamps{{{false, false, true, false},   // R
                                                         {true, false, false, true},    // YY
                                                         {false, false, false, true},   // Y
                                                         {true, true, false, false},    // YG
                                                         {false, true, false, false}}}; // G
    const auto& state = lamps[std::size_t(std::clamp(aspect, 0, 4))];
    for (std::size_t i = 0; i < 4; ++i) {
        const f32 y = 4.95f - f32(i) * 0.42f;
        const u32 c = state[i] ? on[i] : off[i];
        m.box({-0.15f, y - 0.15f, -0.34f}, {0.15f, y + 0.15f, -0.30f}, c, state[i] ? (c & 0xFFFFFFu) : 0u);
        m.box({-0.2f, y + 0.16f, -0.46f}, {0.2f, y + 0.19f, -0.30f}, rgb(20, 20, 22)); // hood
    }
    return m.take();
}

// Platform: `length` m long behind the origin (z from −length to 0), at `side` (−1 left, 1 right).
StaticMesh platformMesh(double length, int side) {
    MeshMaker m;
    const f32 L = f32(length), s = f32(side);
    const f32 x0 = s > 0 ? 1.65f : -6.0f, x1 = s > 0 ? 6.0f : -1.65f;
    m.box({x0, -0.75f, -L}, {x1, 0.4f, 0.0f}, rgb(175, 172, 165)); // 1.15 m above rail (high platform)
    const f32 edge = s > 0 ? x0 : x1 - 0.5f;
    m.box({edge, 0.4f, -L}, {edge + 0.5f, 0.41f, 0.0f}, rgb(230, 200, 40)); // tactile edge
    // canopy over the middle third
    const f32 cx0 = s > 0 ? 3.0f : -5.0f, cx1 = s > 0 ? 5.0f : -3.0f;
    for (f32 z = -L * 0.66f; z <= -L * 0.33f; z += 10.0f) m.box({cx0 + 0.8f, 0.4f, z - 0.1f}, {cx0 + 1.0f, 3.6f, z + 0.1f}, rgb(120, 120, 125));
    m.box({cx0 - 0.5f, 3.6f, -L * 0.7f}, {cx1 + 0.3f, 3.75f, -L * 0.3f}, rgb(200, 205, 210));
    return m.take();
}

// Kilometre post board on the left.
StaticMesh kmPostMesh() {
    MeshMaker m;
    m.box({-0.04f, -0.5f, -0.04f}, {0.04f, 1.6f, 0.04f}, rgb(230, 230, 230));
    m.box({-0.25f, 1.3f, -0.05f}, {0.25f, 1.7f, -0.04f}, rgb(250, 250, 250));
    return m.take();
}

// ---------------------------------------------------------------- reading

struct Range {
    double at = 0.0;
    double value = 0.0, extra = 0.0;
};

double valueAt(const std::vector<Range>& r, double pos) {
    double v = 0.0;
    for (const Range& x : r) {
        if (x.at > pos) break;
        v = x.value;
    }
    return v;
}
const Range* rangeAt(const std::vector<Range>& r, double pos) {
    const Range* out = nullptr;
    for (const Range& x : r) {
        if (x.at > pos) break;
        out = &x;
    }
    return out;
}

std::vector<Range> ranges(const Json& list, std::string_view key, std::string_view extraKey = {}) {
    std::vector<Range> out;
    for (const Json& e : list.items()) out.push_back({e.num("at", 0.0), e.num(key, 0.0), extraKey.empty() ? 0.0 : e.num(extraKey, 0.0)});
    std::ranges::stable_sort(out, {}, &Range::at);
    return out;
}

std::optional<double> clockOf(const Json& v) { return v.isString() ? parseClock(v.string()) : std::nullopt; }

} // namespace

bool isNativeRoute(const std::filesystem::path& file) {
    const std::string name = file.filename().string();
    return name.size() > 11 && iequals(std::string_view(name).substr(name.size() - 11), ".route.json");
}

Expected<LoadedRoute> loadAnyRoute(const std::filesystem::path& file, ObjectLibrary& lib) {
    return isNativeRoute(file) ? loadNativeRoute(file, lib) : loadRoute(file, lib);
}

Expected<LoadedRoute> loadNativeRoute(const std::filesystem::path& file, ObjectLibrary& lib) {
    const auto t0 = std::chrono::steady_clock::now();
    auto parsed = Json::parseFile(file);
    if (!parsed) return Unexpected(std::format("{}: {}", file.string(), parsed.error()));
    const Json& j = *parsed;
    LoadedRoute out;
    Route& route = out.route;
    route.file = file;
    route.comment = j.str("name");
    route.gauge = j.num("gauge", 1.435);
    route.blockLength = 25.0;
    route.trainFolder = j.str("train");
    route.trainFolderHint = file.parent_path();
    route.startTime = clockOf(j["startTime"]);
    const double length = j.num("length", 0.0);
    if (length <= route.blockLength) return Unexpected("route length missing or too short");

    // environment: sun (elevation / azimuth, degrees), sky colour as fog
    {
        const Json& env = j["environment"];
        const double el = glm::radians(env.num("sunElevation", 40.0)), az = glm::radians(env.num("sunAzimuth", 150.0));
        route.lightDirection = glm::vec3(glm::normalize(glm::dvec3(std::sin(az) * std::cos(el), std::sin(el), std::cos(az) * std::cos(el))));
        route.ambient = glm::vec3(f32(env.num("ambient", 170.0) / 255.0));
        route.diffuse = glm::vec3(f32(env.num("diffuse", 200.0) / 255.0));
        route.initialFog = {f32(env.num("fogStart", 2000.0)), f32(env.num("fogEnd", 6000.0)), 0xD8C4A8u}; // 0xBBGGRR (red in the low byte): pale sky blue
    }

    // ---- geometry: curves (radius, cant) and grades (permille) per 25 m block
    const std::vector<Range> curves = ranges(j["curves"], "radius", "cant");
    const std::vector<Range> grades = ranges(j["grades"], "permille");
    const int nBlocks = int(std::ceil(length / route.blockLength)) + 1;
    route.blocks.resize(std::size_t(nBlocks));
    glm::dvec3 position(0.0);
    glm::dvec2 dir(0.0, 1.0);
    std::vector<std::vector<Placement>> placed(static_cast<std::size_t>(nBlocks));
    const ObjectId track = lib.addProcedural("native-track-25", [g = route.gauge, L = route.blockLength] { return trackMesh(L, g); });
    const ObjectId kmPost = lib.addProcedural("native-km-post", [] { return kmPostMesh(); });
    std::vector<glm::dmat3> blockRot(static_cast<std::size_t>(nBlocks));
    for (int i = 0; i < nBlocks; ++i) {
        RouteBlock& b = route.blocks[std::size_t(i)];
        const double start = i * route.blockLength;
        b.origin = position;
        b.startDir = dir;
        b.pitch = valueAt(grades, start) / 1000.0;
        if (const Range* c = rangeAt(curves, start)) b.curveRadius = c->value, b.cant = c->extra / 1000.0; // cant in mm
        // advance (openBVE block geometry: a true arc, the chord at half the turn)
        const double p = b.pitch, s = route.blockLength / std::sqrt(1.0 + p * p), h = s * p;
        glm::dvec2 chord = dir;
        if (b.curveRadius != 0.0) {
            const double r = b.curveRadius, bb = s / std::abs(r);
            const double c = std::sqrt(std::max(2.0 * r * r * (1.0 - std::cos(bb)), 0.0));
            const double a = 0.5 * (r > 0 ? 1.0 : -1.0) * bb;
            chord = rotate2(dir, std::cos(-a), std::sin(-a));
            position += glm::dvec3(chord.x * c, h, chord.y * c);
            dir = rotate2(dir, std::cos(-2.0 * a), std::sin(-2.0 * a));
        } else {
            position += glm::dvec3(dir.x * s, h, dir.y * s);
        }
        // the block's track object follows the chord (and the grade)
        const glm::dvec3 Z = glm::normalize(glm::dvec3(chord.x, p, chord.y));
        const glm::dvec3 X = glm::normalize(glm::dvec3(chord.y, 0.0, -chord.x));
        blockRot[std::size_t(i)] = glm::dmat3(X, glm::cross(Z, X), Z);
        placed[std::size_t(i)].push_back({track, glm::vec3(0.0f), glm::quat_cast(glm::mat3(blockRot[std::size_t(i)])), 0.0f, std::nullopt});
    }
    auto place = [&](ObjectId obj, double trackPos, glm::dvec3 offset, std::optional<u32> section = std::nullopt, bool turn = false) {
        const int bi = std::clamp(route.blockAt(trackPos), 0, nBlocks - 1);
        const TrackPose pose = route.poseAt(trackPos);
        glm::dmat3 rot(pose.side, pose.up, pose.direction);
        if (turn) rot = glm::dmat3(-pose.side, pose.up, -pose.direction);
        Placement pl;
        pl.object = obj;
        pl.pos = glm::vec3(pose.position + rot * offset - route.blocks[std::size_t(bi)].origin);
        pl.rot = glm::quat_cast(glm::mat3(rot));
        pl.trackOffset = f32(trackPos - bi * route.blockLength);
        pl.section = section;
        placed[std::size_t(bi)].push_back(pl);
    };
    for (double km = 1000.0; km < length; km += 1000.0) place(kmPost, km, {-3.2, 0.0, 0.0});

    // ---- speed limits
    for (const Json& l : j["speedLimits"].items()) route.limits.push_back({l.num("at", 0.0), l.has("kmh") ? l.num("kmh", 0.0) / 3.6 : kInf});
    std::ranges::stable_sort(route.limits, {}, &SpeedLimit::trackPos);

    // ---- stations
    for (const Json& s : j["stations"].items()) {
        Station st;
        st.name = s.str("name");
        st.trackPos = s.num("at", 0.0);
        st.arrival = clockOf(s["arrival"]);
        st.departure = clockOf(s["departure"]);
        st.stopDuration = s.num("stopSeconds", 30.0);
        st.mode = s.flag("pass", false) ? StopMode::AllPass : StopMode::AllStop;
        st.terminal = s.flag("terminal", false);
        const std::string side = s.str("platform", "right");
        st.doors = side == "left" ? -1 : side == "both" ? 0 : 1;
        st.stops.push_back({st.trackPos, s.num("tolerance", 5.0), s.num("tolerance", 5.0), 0});
        const double platform = s.num("platformLength", 200.0);
        const int sideSign = st.doors < 0 ? -1 : 1;
        const ObjectId plat =
            lib.addProcedural(std::format("native-platform-{:.0f}-{}", platform, sideSign), [platform, sideSign] { return platformMesh(platform, sideSign); });
        place(plat, st.trackPos + 10.0, {0.0, 0.0, 0.0});
        if (st.doors == 0) {
            const ObjectId other = lib.addProcedural(std::format("native-platform-{:.0f}--1", platform), [platform] { return platformMesh(platform, -1); });
            place(other, st.trackPos + 10.0, {0.0, 0.0, 0.0});
        }
        route.stations.push_back(std::move(st));
    }
    std::ranges::stable_sort(route.stations, {}, &Station::trackPos);

    // ---- block signals: automatic spacing + explicit positions + station departure signals
    {
        const Json& sig = j["signals"];
        std::vector<int> aspects;
        for (const Json& a : sig["aspects"].items()) aspects.push_back(int(a.number()));
        if (aspects.empty()) aspects = {0, 1, 2, 3, 4};
        std::vector<std::pair<double, std::optional<std::size_t>>> at; // position, departure station
        at.emplace_back(0.0, std::nullopt);
        if (const double spacing = sig.num("spacing", 0.0); spacing > 0)
            for (double x = spacing; x < length - 50.0; x += spacing) at.emplace_back(x, std::nullopt);
        for (const Json& x : sig["at"].items()) at.emplace_back(x.number(), std::nullopt);
        const double depOffset = sig.num("departureOffset", 30.0);
        for (std::size_t i = 0; i < route.stations.size(); ++i)
            if (route.stations[i].stopsHere() && !route.stations[i].terminal) at.emplace_back(route.stations[i].trackPos + depOffset, i);
        std::ranges::sort(at, {}, &std::pair<double, std::optional<std::size_t>>::first);
        // merge signals closer than 100 m (a departure signal wins)
        std::vector<std::pair<double, std::optional<std::size_t>>> merged;
        for (const auto& s : at) {
            if (!merged.empty() && s.first - merged.back().first < 100.0) {
                if (s.second) merged.back() = s;
                continue;
            }
            merged.push_back(s);
        }
        // signal head object: states per aspect, driven by the section's aspect
        auto def = std::make_shared<AnimatedDef>();
        AnimatedPart part;
        for (int a = 0; a <= 4; ++a) part.states.push_back(lib.addMesh(std::format("native-signal-{}", a), std::make_shared<StaticMesh>(signalMesh(a))));
        part.stateFn = Function::compile("section");
        def->parts.push_back(std::move(part));
        const ObjectId signal = lib.addAnimated("native-signal", std::move(def), 6.0f);
        const double sideX = sig.str("side", "left") == "right" ? 2.8 : -2.8;
        for (const auto& [pos, station] : merged) {
            Section s;
            s.trackPos = pos;
            s.aspects = aspects;
            s.departureStation = station;
            s.currentAspect = s.pluginAspect = aspects.back();
            route.sections.push_back(std::move(s));
            if (pos > 0.0) place(signal, pos, {sideX, 0.0, 0.0}, u32(route.sections.size() - 1));
        }
        // aspect speeds (Route.Signal): R none (stop handled separately), YY 25, Y 45, YG 65, G none
        route.signalSpeeds = {kInf, 25 / 3.6, 45 / 3.6, 65 / 3.6, kInf};
        if (const Json& sp = sig["speeds"]; sp.isArray()) {
            route.signalSpeeds.clear();
            for (const Json& v : sp.items()) route.signalSpeeds.push_back(v.isNumber() && v.number() > 0 ? v.number() / 3.6 : kInf);
        }
    }

    // ---- train protection zones
    for (const Json& z : j["safety"].items()) route.safetyZones.push_back({z.num("at", 0.0), z.str("system")});
    std::ranges::stable_sort(route.safetyZones, {}, &SafetyZone::trackPos);

    // flatten placements per block (as the openBVE parser does)
    for (int i = 0; i < nBlocks; ++i) {
        RouteBlock& rb = route.blocks[std::size_t(i)];
        rb.firstPlacement = u32(route.placements.size());
        rb.placementCount = u32(placed[std::size_t(i)].size());
        glm::vec3 mn(0.0f), mx(0.0f);
        for (const Placement& p : placed[std::size_t(i)]) mn = glm::min(mn, p.pos), mx = glm::max(mx, p.pos);
        rb.bmin = mn, rb.bmax = mx;
        rb.fogStart = route.initialFog.start, rb.fogEnd = route.initialFog.end, rb.fogColor = route.initialFog.color;
        route.placements.insert(route.placements.end(), placed[std::size_t(i)].begin(), placed[std::size_t(i)].end());
    }
    out.stats.placements = route.placements.size();
    out.stats.parseSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

} // namespace ots
