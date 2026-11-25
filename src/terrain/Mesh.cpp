#include "terrain/Mesh.hpp"

#include "phx/core/Log.hpp"
#include "phx/core/Png.hpp"
#include "util/Paths.hpp"
#include "util/Platform.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace ots::terrain {
namespace {

// ------------------------------------------------------------------ textures
constexpr int kTexSize = 256;

// Tileable value noise in [0, 1] with `period` lattice cells across the texture.
double tileNoise(int x, int y, int period, u32 seed) {
    auto lattice = [&](int i, int j) {
        u32 h = u32((i % period + period) % period) * 374761393u + u32((j % period + period) % period) * 668265263u + seed * 2246822519u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return double((h ^ (h >> 16)) & 0xFFFF) / 65535.0;
    };
    const double cell = double(kTexSize) / period, fx = x / cell, fy = y / cell;
    const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
    auto smooth = [](double t) { return t * t * (3 - 2 * t); };
    const double ax = smooth(fx - ix), ay = smooth(fy - iy);
    return std::lerp(std::lerp(lattice(ix, iy), lattice(ix + 1, iy), ax), std::lerp(lattice(ix, iy + 1), lattice(ix + 1, iy + 1), ax), ay);
}

struct TextureRecipe {
    std::string_view name;
    std::string TerrainTextures::*out;
    glm::dvec3 base, variation;
    u32 seed;
    double grain;
};

bool writeTexture(const std::filesystem::path& file, const TextureRecipe& r) {
    std::vector<u8> rgb(kTexSize * kTexSize * 3);
    auto px = rgb.begin();
    for (int y = 0; y < kTexSize; ++y)
        for (int x = 0; x < kTexSize; ++x) {
            double n = 0.0, amplitude = 0.5;
            for (int octave = 0, period = 4; octave < 5; ++octave, period *= 2, amplitude *= 0.5) n += amplitude * tileNoise(x, y, period, r.seed + u32(octave));
            const double fine = tileNoise(x, y, 128, r.seed + 99) - 0.5;
            const glm::dvec3 c = glm::clamp(r.base + r.variation * (n - 0.5) * 2.0 + glm::dvec3(fine * r.grain), 0.0, 1.0);
            for (int k = 0; k < 3; ++k) *px++ = u8(std::lround(c[k] * 255.0));
        }
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    return phx::writePngRgb(pathString(file), kTexSize, kTexSize, rgb.data());
}

Material textured(const std::string& texture, u32 color = 0xFFFFFFFFu) {
    Material m;
    m.color = color;
    m.texture = texture;
    m.wrap = WrapMode::Repeat;
    return m;
}

// ------------------------------------------------------------------ tiles
constexpr double kUvScale = 12.0; // metres per texture repeat
glm::vec2 groundUv(double x, double z) { return {float(std::fmod(x, 4096.0) / kUvScale), float(std::fmod(z, 4096.0) / kUvScale)}; }

enum class Surface : u8 { Grass, Rock, Soil, Water };
constexpr std::size_t kSurfaces = 4;

// Height samples of a tile with a one-vertex border (for normals).
class TileGrid {
public:
    TileGrid(const TerrainModel& model, const TileSpec& t) : n_(int(std::lround(t.size / t.spacing)) + 1), stride_(n_ + 2) {
        const glm::dvec2 centre = t.corner + glm::dvec2(0.5 * t.size);
        const double halfDiagonal = (0.5 * t.size + t.spacing) * std::numbers::sqrt2;
        // every vertex's closest track sample lies within (centre distance + 2 · half diagonal)
        const auto centreHit = model.track().nearest(centre, model.reach());
        const auto near = model.track().neighbourhood(centre, (centreHit ? std::abs(centreHit->lateral) + 2.0 * t.spacing : model.reach()) +
                                                                  2.0 * halfDiagonal + TrackIndex::kStep);
        points_.resize(std::size_t(stride_) * std::size_t(stride_));
        for (int j = -1; j <= n_; ++j)
            for (int i = -1; i <= n_; ++i)
                at(i, j) = near.empty() ? model.ground(t.corner + glm::dvec2(i, j) * t.spacing)
                                        : model.ground(t.corner + glm::dvec2(i, j) * t.spacing, near);
    }
    [[nodiscard]] int size() const { return n_; }
    [[nodiscard]] const GroundPoint& operator()(int i, int j) const { return points_[index(i, j)]; }

private:
    [[nodiscard]] std::size_t index(int i, int j) const { return std::size_t(j + 1) * std::size_t(stride_) + std::size_t(i + 1); }
    GroundPoint& at(int i, int j) { return points_[index(i, j)]; }
    int n_, stride_;
    std::vector<GroundPoint> points_;
};

} // namespace

TerrainTextures TerrainTextures::ensure() {
    static const std::array<TextureRecipe, 4> recipes{{
        {"grass", &TerrainTextures::grass, {0.32, 0.40, 0.20}, {0.07, 0.08, 0.05}, 11, 0.06},
        {"rock", &TerrainTextures::rock, {0.46, 0.44, 0.41}, {0.10, 0.10, 0.09}, 23, 0.10},
        {"soil", &TerrainTextures::soil, {0.48, 0.41, 0.31}, {0.08, 0.07, 0.06}, 37, 0.08},
        {"concrete", &TerrainTextures::concrete, {0.58, 0.57, 0.55}, {0.05, 0.05, 0.05}, 51, 0.05},
    }};
    const std::filesystem::path dir = userCacheDir() / "terrain" / "v1";
    TerrainTextures t;
    for (const TextureRecipe& r : recipes) {
        const std::filesystem::path file = dir / (std::string(r.name) + ".png");
        if (!fileExists(file) && !writeTexture(file, r)) phx::log::warn("Terrain: cannot write {}", pathString(file));
        t.*r.out = pathString(file);
    }
    return t;
}

StaticMesh buildTile(const TerrainModel& model, const TerrainTextures& tex, const TileSpec& t) {
    const TileGrid grid(model, t);
    const int n = grid.size();
    std::array<SubMesh, kSurfaces> parts;
    parts[std::size_t(Surface::Grass)].material = textured(tex.grass);
    parts[std::size_t(Surface::Rock)].material = textured(tex.rock);
    parts[std::size_t(Surface::Soil)].material = textured(tex.soil);
    parts[std::size_t(Surface::Water)].material.color = 0xFF6E5A3Cu; // RGBA (r low byte): muted blue-grey

    auto worldXZ = [&](int i, int j) { return t.corner + glm::dvec2(i, j) * t.spacing; };
    auto position = [&](int i, int j, double dy = 0.0) {
        const glm::dvec2 w = worldXZ(i, j);
        return glm::vec3(glm::dvec3(w.x, grid(i, j).y + dy, w.y) - t.origin);
    };
    // vertices are shared per surface: index of (i, j) in each part, created on first use
    std::vector<std::array<u32, kSurfaces>> vertexOf(std::size_t(n) * std::size_t(n), {~0u, ~0u, ~0u, ~0u});
    auto vertex = [&](Surface s, int i, int j) {
        u32& v = vertexOf[std::size_t(j) * std::size_t(n) + std::size_t(i)][std::size_t(s)];
        if (v != ~0u) return v;
        const glm::dvec3 normal = glm::normalize(glm::dvec3(-(grid(i + 1, j).y - grid(i - 1, j).y) / (2 * t.spacing), 1.0,
                                                            -(grid(i, j + 1).y - grid(i, j - 1).y) / (2 * t.spacing)));
        SubMesh& part = parts[std::size_t(s)];
        v = u32(part.vertices.size());
        const glm::dvec2 w = worldXZ(i, j);
        part.vertices.push_back({position(i, j), glm::vec3(normal), groundUv(w.x, w.y)});
        return v;
    };

    for (int j = 0; j + 1 < n; ++j)
        for (int i = 0; i + 1 < n; ++i) {
            const std::array<glm::ivec2, 4> quad{{{i, j}, {i + 1, j}, {i, j + 1}, {i + 1, j + 1}}};
            // two triangles, clockwise seen from above (openBVE front faces): a c b, b c d
            for (const auto& tri : {std::array{0, 2, 1}, std::array{1, 2, 3}}) {
                bool core = false, mouth = false, water = true, modified = false;
                std::array<glm::dvec3, 3> p;
                for (int k = 0; k < 3; ++k) {
                    const glm::ivec2 q = quad[std::size_t(tri[std::size_t(k)])];
                    const GroundPoint& g = grid(q.x, q.y);
                    core |= g.core, mouth |= g.mouth, water &= g.water, modified |= g.modified;
                    p[std::size_t(k)] = {q.x * t.spacing, g.y, q.y * t.spacing};
                }
                if (core && mouth) continue; // tunnel mouth: the portal fills it
                const bool steep = std::abs(glm::normalize(glm::cross(p[2] - p[0], p[1] - p[0])).y) < 0.72;
                const Surface s = water ? Surface::Water : modified ? Surface::Soil : steep ? Surface::Rock : Surface::Grass;
                for (int k : tri) {
                    const glm::ivec2 q = quad[std::size_t(k)];
                    parts[std::size_t(s)].indices.push_back(vertex(s, q.x, q.y));
                }
            }
        }

    // skirts hide cracks against neighbours with another spacing
    const double drop = 2.0 * t.spacing + 4.0;
    auto skirt = [&](glm::ivec2 a, glm::ivec2 b) {
        if (grid(a.x, a.y).core || grid(b.x, b.y).core) return; // never hang into a tunnel
        SubMesh& sm = parts[std::size_t(Surface::Grass)];
        const u32 base = u32(sm.vertices.size());
        for (int k = 0; k < 4; ++k) {
            const glm::ivec2 q = k & 1 ? b : a;
            const glm::dvec2 w = worldXZ(q.x, q.y);
            sm.vertices.push_back({position(q.x, q.y, k >= 2 ? -drop : 0.0), glm::vec3(0, 1, 0), groundUv(w.x, w.y)});
        }
        for (u32 x : {0u, 1u, 2u, 1u, 3u, 2u, 0u, 2u, 1u, 1u, 2u, 3u}) sm.indices.push_back(base + x); // both sides
    };
    for (int k = 0; k + 1 < n; ++k) {
        skirt({k, 0}, {k + 1, 0});
        skirt({k, n - 1}, {k + 1, n - 1});
        skirt({0, k}, {0, k + 1});
        skirt({n - 1, k}, {n - 1, k + 1});
    }
    StaticMesh mesh;
    for (SubMesh& p : parts)
        if (!p.indices.empty()) mesh.parts.push_back(std::move(p));
    return mesh;
}

// ------------------------------------------------------------------ tunnels
namespace {

// Lining cross-section (track-local x right, y up from the rail): left foot, over the arch,
// right foot.
std::vector<glm::dvec2> liningProfile(const TerrainModel& model) {
    const double r = model.settings().tunnelRadius, foot = -model.settings().formationDepth - 0.2;
    constexpr int kArch = 16;
    std::vector<glm::dvec2> p{{-r, foot}};
    for (int k = 0; k <= kArch; ++k) {
        const double a = std::numbers::pi * (1.0 - double(k) / kArch);
        p.push_back({r * std::cos(a), 1.5 + r * std::sin(a)});
    }
    p.push_back({r, foot});
    return p;
}

glm::dvec3 trackPoint(const TrackPose& tp, glm::dvec2 q) { return tp.position + tp.side * q.x + tp.up * q.y; }

// Point at fraction f of a polyline's length.
glm::dvec2 alongPath(std::span<const glm::dvec2> path, double f) {
    double total = 0.0;
    for (std::size_t k = 1; k < path.size(); ++k) total += glm::distance(path[k], path[k - 1]);
    double want = f * total;
    for (std::size_t k = 1; k < path.size(); ++k) {
        const double l = glm::distance(path[k], path[k - 1]);
        if (want <= l || k + 1 == path.size()) return glm::mix(path[k - 1], path[k], l > 0 ? std::min(1.0, want / l) : 0.0);
        want -= l;
    }
    return path.back();
}

} // namespace

StaticMesh buildLining(const TerrainModel& model, const TerrainTextures& tex, double from, double to, const glm::dvec3& origin) {
    const auto profile = liningProfile(model);
    std::vector<double> arc(profile.size(), 0.0);
    for (std::size_t k = 1; k < profile.size(); ++k) arc[k] = arc[k - 1] + glm::distance(profile[k], profile[k - 1]);
    SubMesh sm;
    sm.material = textured(tex.concrete);
    sm.doubleSided = true;
    const int steps = std::max(1, int(std::ceil((to - from) / 5.0)));
    for (int i = 0; i <= steps; ++i) {
        const double s = std::lerp(from, to, double(i) / steps);
        const TrackPose tp = model.route().poseAt(s);
        for (std::size_t k = 0; k < profile.size(); ++k) {
            const glm::dvec2 inward = glm::normalize(glm::dvec2(0.0, 1.5) - profile[k]);
            sm.vertices.push_back({glm::vec3(trackPoint(tp, profile[k]) - origin), glm::vec3(glm::normalize(tp.side * inward.x + tp.up * inward.y)),
                                   glm::vec2(float(arc[k] / 3.0), float((s - from) / 3.0))});
        }
    }
    const u32 ring = u32(profile.size());
    for (u32 i = 0; i < u32(steps); ++i)
        for (u32 k = 0; k + 1 < ring; ++k) {
            const u32 a = i * ring + k, b = a + 1, c = a + ring, d = c + 1;
            sm.indices.insert(sm.indices.end(), {a, c, b, b, c, d});
        }
    StaticMesh mesh;
    mesh.parts.push_back(std::move(sm));
    return mesh;
}

StaticMesh buildPortal(const TerrainModel& model, const TerrainTextures& tex, double trackPos, bool entrance, const glm::dvec3& origin) {
    const TrackPose tp = model.route().poseAt(trackPos);
    const auto inner = liningProfile(model);
    const double halfWidth = model.settings().tunnelRadius + 7.0, top = model.crown() + 3.0, foot = -model.settings().formationDepth - 1.5;
    const std::array<glm::dvec2, 4> outer{{{-halfWidth, foot}, {-halfWidth, top}, {halfWidth, top}, {halfWidth, foot}}};
    SubMesh sm;
    sm.material = textured(tex.concrete, 0xFFD8D8D8u);
    sm.doubleSided = true;
    // a strip between the arch and the outer rectangle, matched by arc length
    constexpr u32 kSteps = 48;
    const glm::vec3 normal(entrance ? -tp.direction : tp.direction);
    for (u32 k = 0; k <= kSteps; ++k)
        for (const glm::dvec2 q : {alongPath(inner, double(k) / kSteps), alongPath(outer, double(k) / kSteps)})
            sm.vertices.push_back({glm::vec3(trackPoint(tp, q) - origin), normal, glm::vec2(float(q.x / 3.0), float(q.y / 3.0))});
    for (u32 k = 0; k < kSteps; ++k) {
        const u32 a = k * 2, b = a + 1, c = a + 2, d = a + 3;
        sm.indices.insert(sm.indices.end(), {a, b, c, c, b, d});
    }
    StaticMesh mesh;
    mesh.parts.push_back(std::move(sm));
    return mesh;
}

} // namespace ots::terrain
