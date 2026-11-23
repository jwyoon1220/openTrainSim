// CSV / B3D object loader (openBVE Object.CsvB3d semantics) and MeshBuilder.
#include "bve/MeshBuilder.hpp"

#include "util/Paths.hpp"
#include "util/Text.hpp"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>

namespace ots {

// ------------------------------------------------------------------ StaticMesh

void StaticMesh::computeBounds() {
    bool first = true;
    for (const SubMesh& p : parts)
        for (const MeshVertex& v : p.vertices) {
            if (first) bmin = bmax = v.pos, first = false;
            bmin = glm::min(bmin, v.pos);
            bmax = glm::max(bmax, v.pos);
        }
    if (first) bmin = bmax = glm::vec3(0.0f);
}

std::size_t StaticMesh::triangleCount() const {
    std::size_t n = 0;
    for (const SubMesh& p : parts) n += p.indices.size() / 3;
    return n;
}

void StaticMesh::transform(const glm::mat3& m, const glm::vec3& t) {
    const glm::mat3 nm = glm::inverseTranspose(m);
    const bool flip = glm::determinant(m) < 0.0f;
    for (SubMesh& p : parts) {
        for (MeshVertex& v : p.vertices) {
            v.pos = m * v.pos + t;
            const glm::vec3 n = nm * v.normal;
            const float l = glm::length(n);
            v.normal = l > 0.0f ? n / l : v.normal;
        }
        if (flip)
            for (std::size_t i = 0; i + 2 < p.indices.size(); i += 3) std::swap(p.indices[i + 1], p.indices[i + 2]);
    }
    computeBounds();
}

void StaticMesh::mirrorX() { transform(glm::mat3(glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)), glm::vec3(0.0f)); }

void StaticMesh::deformBetweenRails(double nearDistance, double farDistance) {
    std::vector<glm::vec3> all;
    for (const SubMesh& p : parts)
        for (const MeshVertex& v : p.vertices) all.push_back(v.pos);
    if (all.size() < 4) return;
    constexpr float tol = 0.05f;
    float minZ = all[0].z, maxZ = all[0].z;
    for (const glm::vec3& v : all) minZ = std::min(minZ, v.z), maxZ = std::max(maxZ, v.z);
    if (maxZ - minZ <= 2.0f * tol) return;
    int bl = -1, br = -1, tl = -1, tr = -1; // bottom = far (max z), top = near (min z)
    for (int i = 0; i < int(all.size()); ++i) {
        const glm::vec3 c = all[std::size_t(i)];
        if (c.z >= maxZ - tol) {
            if (bl < 0 || c.x < all[std::size_t(bl)].x) bl = i;
            if (br < 0 || c.x > all[std::size_t(br)].x) br = i;
        }
        if (c.z <= minZ + tol) {
            if (tl < 0 || c.x < all[std::size_t(tl)].x) tl = i;
            if (tr < 0 || c.x > all[std::size_t(tr)].x) tr = i;
        }
    }
    if (bl < 0 || br < 0 || tl < 0 || tr < 0) return;
    const double xL0 = all[std::size_t(tl)].x, xL1 = all[std::size_t(bl)].x;
    const double xR0 = all[std::size_t(tr)].x, xR1 = all[std::size_t(br)].x;
    const bool right = nearDistance < 0.0; // openBVE: secondary rail on the left → TransformRight
    const double zMin = minZ, zRange = maxZ - minZ;
    for (SubMesh& p : parts)
        for (MeshVertex& v : p.vertices) {
            double t = zRange > 0.001 ? (double(v.pos.z) - zMin) / zRange : 0.0;
            t = std::clamp(t, 0.0, 1.0);
            double faceX, originalBackX, backX;
            if (!right) {
                faceX = xL0 + t * (xL1 - xL0);
                originalBackX = xR0 + t * (xR1 - xR0);
                backX = (nearDistance - xL0) + t * ((farDistance - xL1) - (nearDistance - xL0));
            } else {
                faceX = xR0 + t * (xR1 - xR0);
                originalBackX = xL0 + t * (xL1 - xL0);
                backX = (nearDistance - xR0) + t * ((farDistance - xR1) - (nearDistance - xR0));
            }
            const double range = originalBackX - faceX;
            if (std::abs(range) > 0.001) v.pos.x = float(faceX + ((v.pos.x - faceX) / range) * (backX - faceX));
        }
    computeBounds();
}

// ------------------------------------------------------------------ MeshBuilder

glm::mat3 axisRotation(glm::vec3 a, float t) {
    const float c = std::cos(t), s = std::sin(t), C = 1.0f - c;
    // Column-major: m[col][row]. Row-major Rodrigues matrix (openBVE Vector3.Rotate) transposed.
    glm::mat3 m;
    m[0][0] = c + C * a.x * a.x;       m[1][0] = C * a.x * a.y - s * a.z; m[2][0] = C * a.x * a.z + s * a.y;
    m[0][1] = C * a.x * a.y + s * a.z; m[1][1] = c + C * a.y * a.y;       m[2][1] = C * a.y * a.z - s * a.x;
    m[0][2] = C * a.x * a.z - s * a.y; m[1][2] = C * a.y * a.z + s * a.x; m[2][2] = c + C * a.z * a.z;
    return m;
}

void MeshBuilder::transform(const glm::mat3& m, const glm::vec3& t) {
    const glm::mat3 nm = glm::inverseTranspose(m);
    for (BuilderVertex& v : vertices) {
        v.pos = m * v.pos + t;
        if (v.normal != glm::vec3(0.0f)) {
            const glm::vec3 n = nm * v.normal;
            const float l = glm::length(n);
            if (l > 0.0f) v.normal = n / l;
        }
    }
    for (BuilderFace& f : faces)
        for (glm::vec3& n : f.normals) {
            const glm::vec3 r = nm * n;
            const float l = glm::length(r);
            if (l > 0.0f) n = r / l;
        }
    if (glm::determinant(m) < 0.0f) reverseFaces();
}

void MeshBuilder::reverseFaces() {
    for (BuilderFace& f : faces) {
        if (f.triangles) {
            for (std::size_t i = 0; i + 2 < f.idx.size(); i += 3) {
                std::swap(f.idx[i + 1], f.idx[i + 2]);
                if (f.normals.size() == f.idx.size()) std::swap(f.normals[i + 1], f.normals[i + 2]);
            }
        } else {
            std::reverse(f.idx.begin(), f.idx.end());
            std::reverse(f.normals.begin(), f.normals.end());
        }
    }
}

void MeshBuilder::addCube(const glm::vec3& h) {
    const u32 v = u32(vertices.size());
    const glm::vec3 p[8] = {{h.x, h.y, -h.z}, {h.x, -h.y, -h.z}, {-h.x, -h.y, -h.z}, {-h.x, h.y, -h.z},
                            {h.x, h.y, h.z},  {h.x, -h.y, h.z},  {-h.x, -h.y, h.z},  {-h.x, h.y, h.z}};
    for (const glm::vec3& q : p) vertices.push_back({q, glm::vec3(0.0f), glm::vec2(0.0f)});
    const u32 f[6][6] = {{0, 1, 2, 0, 2, 3}, {0, 4, 5, 0, 5, 1}, {0, 3, 7, 0, 7, 4},
                         {6, 5, 4, 6, 4, 7}, {6, 7, 3, 6, 3, 2}, {6, 2, 1, 6, 1, 5}};
    for (const auto& face : f) {
        BuilderFace bf;
        bf.triangles = true;
        bf.material = u32(materials.size() - 1);
        for (u32 k : face) bf.idx.push_back(v + k);
        faces.push_back(std::move(bf));
    }
}

void MeshBuilder::addCylinder(int n, float upperRadius, float lowerRadius, float height) {
    const bool upperCap = upperRadius > 0.0f, lowerCap = lowerRadius > 0.0f;
    upperRadius = std::abs(upperRadius), lowerRadius = std::abs(lowerRadius);
    const float ns = height >= 0.0f ? 1.0f : -1.0f;
    const float d = 2.0f * float(M_PI) / float(n);
    const float g = 0.5f * height;
    const float a = height != 0.0f ? std::atan((lowerRadius - upperRadius) / height) : 0.0f;
    const u32 v = u32(vertices.size());
    std::vector<glm::vec3> normals(std::size_t(2 * n));
    float t = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float dx = std::cos(t), dz = std::sin(t);
        vertices.push_back({{dx * upperRadius, g, dz * upperRadius}, glm::vec3(0.0f), glm::vec2(0.0f)});
        vertices.push_back({{dx * lowerRadius, -g, dz * lowerRadius}, glm::vec3(0.0f), glm::vec2(0.0f)});
        glm::vec3 normal(dx * ns, 0.0f, dz * ns);
        const glm::vec3 s = glm::cross(normal, glm::vec3(0, 1, 0));
        if (glm::length(s) > 0.0f) normal = axisRotation(glm::normalize(s), a) * normal;
        normals[std::size_t(2 * i)] = normals[std::size_t(2 * i + 1)] = normal;
        t += d;
    }
    const u32 m = u32(materials.size() - 1);
    for (int i = 0; i < n; ++i) {
        const u32 i0 = u32((2 * i + 2) % (2 * n)), i1 = u32((2 * i + 3) % (2 * n)), i2 = u32(2 * i + 1), i3 = u32(2 * i);
        BuilderFace f;
        f.material = m;
        f.idx = {v + i0, v + i1, v + i2, v + i3};
        f.normals = {normals[i0], normals[i1], normals[i2], normals[i3]};
        faces.push_back(std::move(f));
    }
    for (int cap = 0; cap < 2; ++cap) {
        if ((cap == 0 && !lowerCap) || (cap == 1 && !upperCap)) continue;
        BuilderFace f;
        f.material = m;
        for (int j = 0; j < n; ++j) f.idx.push_back(cap == 0 ? v + u32(2 * j + 1) : v + u32(2 * (n - j - 1)));
        faces.push_back(std::move(f));
    }
}

namespace {

glm::vec3 polygonNormal(const std::vector<BuilderVertex>& vs, const u32* idx, std::size_t n) {
    // Newell's method; same orientation as cross(v1 - v0, v2 - v0) for convex polygons.
    glm::vec3 nrm(0.0f);
    for (std::size_t i = 0; i < n; ++i) {
        const glm::vec3& a = vs[idx[i]].pos;
        const glm::vec3& b = vs[idx[(i + 1) % n]].pos;
        nrm.x += (a.y - b.y) * (a.z + b.z);
        nrm.y += (a.z - b.z) * (a.x + b.x);
        nrm.z += (a.x - b.x) * (a.y + b.y);
    }
    const float l = glm::length(nrm);
    return l > 1e-12f ? nrm / l : glm::vec3(0.0f, 1.0f, 0.0f);
}

struct VertexKey {
    u32 index;
    u32 nx, ny, nz;
    bool operator==(const VertexKey&) const = default;
};
struct VertexKeyHash {
    std::size_t operator()(const VertexKey& k) const {
        return std::size_t(k.index) * 73856093u ^ std::size_t(k.nx) * 19349663u ^ std::size_t(k.ny) * 83492791u ^ k.nz;
    }
};
u32 bits(float f) {
    u32 u;
    std::memcpy(&u, &f, 4);
    return u;
}

} // namespace

void MeshBuilder::flush(StaticMesh& out) {
    if (faces.empty()) {
        *this = MeshBuilder{};
        return;
    }
    struct PartBuild {
        SubMesh mesh;
        std::unordered_map<VertexKey, u32, VertexKeyHash> remap;
    };
    std::map<std::pair<u32, bool>, PartBuild> parts;
    auto corner = [&](PartBuild& pb, u32 vi, const glm::vec3& n) -> u32 {
        const VertexKey key{vi, bits(n.x), bits(n.y), bits(n.z)};
        auto it = pb.remap.find(key);
        if (it != pb.remap.end()) return it->second;
        const u32 idx = u32(pb.mesh.vertices.size());
        pb.mesh.vertices.push_back({vertices[vi].pos, n, vertices[vi].uv});
        pb.remap.emplace(key, idx);
        return idx;
    };
    for (const BuilderFace& f : faces) {
        if (f.idx.size() < 3) continue;
        bool valid = true;
        for (u32 i : f.idx) valid &= i < vertices.size();
        if (!valid) continue;
        const u32 mat = std::min<u32>(f.material, u32(materials.size() - 1));
        PartBuild& pb = parts[{mat, f.face2}];
        auto cornerNormal = [&](std::size_t k, const glm::vec3& faceN) {
            if (k < f.normals.size() && f.normals[k] != glm::vec3(0.0f)) return f.normals[k];
            const glm::vec3& vn = vertices[f.idx[k]].normal;
            return vn != glm::vec3(0.0f) ? vn : faceN;
        };
        if (f.triangles) {
            for (std::size_t t = 0; t + 2 < f.idx.size(); t += 3) {
                const glm::vec3 fn = polygonNormal(vertices, &f.idx[t], 3);
                for (std::size_t k = 0; k < 3; ++k) pb.mesh.indices.push_back(corner(pb, f.idx[t + k], cornerNormal(t + k, fn)));
            }
        } else {
            const glm::vec3 fn = polygonNormal(vertices, f.idx.data(), f.idx.size());
            const u32 c0 = corner(pb, f.idx[0], cornerNormal(0, fn));
            u32 prev = corner(pb, f.idx[1], cornerNormal(1, fn));
            for (std::size_t k = 2; k < f.idx.size(); ++k) {
                const u32 cur = corner(pb, f.idx[k], cornerNormal(k, fn));
                pb.mesh.indices.insert(pb.mesh.indices.end(), {c0, prev, cur});
                prev = cur;
            }
        }
    }
    for (auto& [key, pb] : parts) {
        if (pb.mesh.indices.empty()) continue;
        pb.mesh.material = materials[key.first];
        pb.mesh.doubleSided = key.second;
        if (pb.mesh.material.wrap == WrapMode::Auto) {
            bool inside = true;
            for (const MeshVertex& v : pb.mesh.vertices)
                inside &= v.uv.x >= -0.01f && v.uv.x <= 1.01f && v.uv.y >= -0.01f && v.uv.y <= 1.01f;
            pb.mesh.material.wrap = inside ? WrapMode::Clamp : WrapMode::Repeat;
        }
        if (pb.mesh.material.texture.empty()) pb.mesh.material.wrap = WrapMode::Repeat; // irrelevant: canonicalise
        out.parts.push_back(std::move(pb.mesh));
    }
    *this = MeshBuilder{};
}

void mergeParts(StaticMesh& mesh) {
    std::vector<SubMesh> merged;
    for (SubMesh& p : mesh.parts) {
        auto it = std::find_if(merged.begin(), merged.end(),
                               [&](const SubMesh& m) { return m.doubleSided == p.doubleSided && m.material == p.material; });
        if (it == merged.end()) {
            merged.push_back(std::move(p));
            continue;
        }
        if (it->vertices.size() + p.vertices.size() > 65535) { // keep 16-bit indices possible
            merged.push_back(std::move(p));
            continue;
        }
        const u32 base = u32(it->vertices.size());
        it->vertices.insert(it->vertices.end(), p.vertices.begin(), p.vertices.end());
        for (u32 i : p.indices) it->indices.push_back(base + i);
    }
    mesh.parts = std::move(merged);
    mesh.computeBounds();
}

// ------------------------------------------------------------------ CSV / B3D

namespace {

double arg(const std::vector<std::string_view>& a, std::size_t i, double def = 0.0) {
    double v;
    if (i < a.size() && !trim(a[i]).empty() && parseDoubleVb6(a[i], v)) return v;
    return def;
}

u32 colorArgs(const std::vector<std::string_view>& a, bool alpha) {
    auto c = [&](std::size_t i, double def) { return u32(std::clamp(int(std::lround(arg(a, i, def))), 0, 255)); };
    return c(0, 255) | (c(1, 255) << 8) | (c(2, 255) << 16) | ((alpha ? c(3, 255) : 255u) << 24);
}

} // namespace

bool loadCsvB3d(const std::filesystem::path& path, StaticMesh& out, std::string& error) {
    std::string text;
    if (!readTextFile(path, text)) {
        error = "cannot read";
        return false;
    }
    const bool b3d = iendsWith(path.filename().string(), ".b3d");
    const std::filesystem::path dir = path.parent_path();
    MeshBuilder b;
    StaticMesh mesh;
    // Faces finalised by earlier builders are already in `mesh`; *All transforms reach them too.
    auto transformAll = [&](const glm::mat3& m, const glm::vec3& t) { mesh.transform(m, t); };

    for (const std::string& rawLine : splitLines(text)) {
        std::string_view line = rawLine;
        if (auto sc = line.find(';'); sc != std::string_view::npos) line = line.substr(0, sc);
        line = trim(line);
        if (line.empty()) continue;
        std::string cmd;
        std::vector<std::string_view> a;
        if (b3d) {
            if (line.front() == '[') {
                cmd = lower(trim(line.substr(1, line.find(']') == std::string_view::npos ? line.size() - 1 : line.find(']') - 1)));
            } else {
                std::size_t sp = 0;
                while (sp < line.size() && line[sp] != ' ' && line[sp] != '\t') ++sp;
                cmd = lower(line.substr(0, sp));
                if (sp < line.size()) a = split(trim(line.substr(sp)), ',');
            }
        } else {
            auto fields = split(line, ',');
            cmd = lower(trim(fields[0]));
            a.assign(fields.begin() + 1, fields.end());
        }
        for (auto& s : a) s = trim(s);

        if (cmd == "createmeshbuilder" || cmd == "meshbuilder") {
            b.flush(mesh);
        } else if (cmd == "addvertex" || cmd == "vertex") {
            b.vertices.push_back({{float(arg(a, 0)), float(arg(a, 1)), float(arg(a, 2))},
                                  {float(arg(a, 3)), float(arg(a, 4)), float(arg(a, 5))},
                                  glm::vec2(0.0f)});
            if (b.vertices.back().normal != glm::vec3(0.0f)) b.vertices.back().normal = glm::normalize(b.vertices.back().normal);
        } else if (cmd == "addface" || cmd == "face" || cmd == "addface2" || cmd == "face2") {
            BuilderFace f;
            f.face2 = cmd.back() == '2';
            f.material = u32(b.materials.size() - 1);
            bool ok = true;
            for (std::string_view s : a) {
                if (s.empty()) continue;
                int i;
                if (!parseIntVb6(s, i) || i < 0 || i >= int(b.vertices.size())) {
                    ok = false;
                    break;
                }
                f.idx.push_back(u32(i));
            }
            if (ok && f.idx.size() >= 3) b.faces.push_back(std::move(f));
        } else if (cmd == "cube") {
            b.addCube({float(arg(a, 0, 1)), float(arg(a, 1, 1)), float(arg(a, 2, 1))});
        } else if (cmd == "cylinder") {
            int n = int(arg(a, 0, 8));
            if (n < 2) n = 8;
            b.addCylinder(n, float(arg(a, 1, 1)), float(arg(a, 2, 1)), float(arg(a, 3, 1)));
        } else if (cmd == "translate" || cmd == "translateall") {
            const glm::vec3 t(float(arg(a, 0)), float(arg(a, 1)), float(arg(a, 2)));
            b.transform(glm::mat3(1.0f), t);
            if (cmd == "translateall") transformAll(glm::mat3(1.0f), t);
        } else if (cmd == "scale" || cmd == "scaleall") {
            glm::vec3 s(float(arg(a, 0, 1)), float(arg(a, 1, 1)), float(arg(a, 2, 1)));
            for (int k = 0; k < 3; ++k)
                if (s[k] == 0.0f) s[k] = 1.0f;
            const glm::mat3 m(glm::vec3(s.x, 0, 0), glm::vec3(0, s.y, 0), glm::vec3(0, 0, s.z));
            b.transform(m, glm::vec3(0.0f));
            if (cmd == "scaleall") transformAll(m, glm::vec3(0.0f));
        } else if (cmd == "rotate" || cmd == "rotateall") {
            glm::vec3 axis(float(arg(a, 0)), float(arg(a, 1)), float(arg(a, 2)));
            const double angle = arg(a, 3);
            if (glm::dot(axis, axis) == 0.0f) axis = glm::vec3(1, 0, 0);
            if (angle != 0.0) {
                const glm::mat3 m = axisRotation(glm::normalize(axis), float(angle * M_PI / 180.0));
                b.transform(m, glm::vec3(0.0f));
                if (cmd == "rotateall") transformAll(m, glm::vec3(0.0f));
            }
        } else if (cmd == "shear" || cmd == "shearall") {
            glm::vec3 d(float(arg(a, 0)), float(arg(a, 1)), float(arg(a, 2)));
            glm::vec3 s(float(arg(a, 3)), float(arg(a, 4)), float(arg(a, 5)));
            const float r = float(arg(a, 6));
            if (glm::dot(d, d) > 0) d = glm::normalize(d);
            if (glm::dot(s, s) > 0) s = glm::normalize(s);
            // v' = v + s · r(d·v)
            glm::mat3 m(1.0f);
            for (int c = 0; c < 3; ++c)
                for (int rr = 0; rr < 3; ++rr) m[c][rr] += s[rr] * r * d[c];
            b.transform(m, glm::vec3(0.0f));
            if (cmd == "shearall") transformAll(m, glm::vec3(0.0f));
        } else if (cmd == "mirror" || cmd == "mirrorall") {
            const glm::vec3 s(arg(a, 0) != 0 ? -1.0f : 1.0f, arg(a, 1) != 0 ? -1.0f : 1.0f, arg(a, 2) != 0 ? -1.0f : 1.0f);
            const glm::mat3 m(glm::vec3(s.x, 0, 0), glm::vec3(0, s.y, 0), glm::vec3(0, 0, s.z));
            b.transform(m, glm::vec3(0.0f));
            if (cmd == "mirrorall") transformAll(m, glm::vec3(0.0f));
        } else if (cmd == "setcolor" || cmd == "color" || cmd == "setcolorall" || cmd == "colorall") {
            const u32 c = colorArgs(a, true);
            for (Material& m : b.materials) m.color = c;
            // openBVE (issue #1397): the colour applies to existing faces only; later faces
            // get a fresh default material.
            b.materials.push_back(Material{});
            if (cmd.ends_with("all"))
                for (SubMesh& p : mesh.parts) p.material.color = c;
        } else if (cmd == "setemissivecolor" || cmd == "emissivecolor" || cmd == "setemissivecolorall" || cmd == "emissivecolorall") {
            const u32 c = colorArgs(a, false) & 0x00FFFFFFu;
            for (Material& m : b.materials) m.emissive = c;
            if (cmd.ends_with("all"))
                for (SubMesh& p : mesh.parts) p.material.emissive = c;
        } else if (cmd == "setdecaltransparentcolor" || cmd == "transparent") {
            const u32 c = colorArgs(a, false);
            for (Material& m : b.materials) {
                m.colorKey = true;
                m.keyR = u8(c), m.keyG = u8(c >> 8), m.keyB = u8(c >> 16);
            }
        } else if (cmd == "setblendmode" || cmd == "blendmode") {
            const bool additive = !a.empty() && iequals(a[0], "additive");
            const double glow = arg(a, 1, 0.0);
            for (Material& m : b.materials) {
                m.blend = additive ? MaterialBlend::Additive : MaterialBlend::Normal;
                m.glowHalfDistance = u16(std::clamp(glow, 0.0, 65535.0));
            }
        } else if (cmd == "setwrapmode" || cmd == "wrapmode") {
            if (!a.empty()) {
                const std::string w = lower(a[0]);
                b.materials.back().wrap = w.find("clamp") != std::string::npos && w.find("repeat") == std::string::npos
                                              ? WrapMode::Clamp
                                              : WrapMode::Repeat;
            }
        } else if (cmd == "loadtexture" || cmd == "load") {
            if (!a.empty() && !a[0].empty()) {
                const std::filesystem::path tex = resolvePath(dir, a[0]);
                if (tex.empty()) error = "texture not found: " + std::string(a[0]);
                for (Material& m : b.materials) m.texture = tex.string();
            }
        } else if (cmd == "settexturecoordinates" || cmd == "coordinates") {
            int i;
            if (!a.empty() && parseIntVb6(a[0], i) && i >= 0 && i < int(b.vertices.size()))
                b.vertices[std::size_t(i)].uv = {float(arg(a, 1)), float(arg(a, 2))};
        }
        // GenerateNormals, SetText, crossfading etc.: no-ops here.
    }
    b.flush(mesh);
    mergeParts(mesh);
    out = std::move(mesh);
    return true;
}

} // namespace ots
