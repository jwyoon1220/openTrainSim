#include "bve/Gltf.hpp"

#include "util/Paths.hpp"
#include "util/Platform.hpp"

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <array>
#include <span>
#include <format>
#include <fstream>

namespace ots {

namespace {

const glm::dmat4 kMirror = glm::scale(glm::dmat4(1.0), glm::dvec3(-1.0, 1.0, 1.0));

u32 packColor(std::span<const float> c, float alpha) {
    auto b = [](double x) { return u32(std::lround(std::clamp(x, 0.0, 1.0) * 255.0)); };
    return b(c[0]) | (b(c[1]) << 8) | (b(c[2]) << 16) | (b(alpha) << 24);
}

// Embedded images (GLB buffer views, data: URIs) are written to the user cache once so the
// texture cache can load them like any other file.
std::filesystem::path cacheDir() { return userCacheDir() / "gltf"; }

std::string imageExtension(const cgltf_image& img) {
    const std::string mime = img.mime_type ? img.mime_type : "";
    if (mime == "image/jpeg") return ".jpg";
    return ".png";
}

std::string imagePath(const std::filesystem::path& model, const cgltf_data& data, std::size_t index, const cgltf_options& opt) {
    const cgltf_image& img = data.images[index];
    if (img.uri && std::string_view(img.uri).substr(0, 5) != "data:") {
        std::string uri = img.uri;
        cgltf_decode_uri(uri.data()); // in place, may shorten
        if (const auto end = uri.find('\0'); end != std::string::npos) uri.resize(end);
        const std::filesystem::path p = resolvePath(model.parent_path(), uri);
        return p.empty() ? std::string() : p.string();
    }
    const void* bytes = nullptr;
    std::size_t size = 0;
    void* decoded = nullptr;
    if (img.buffer_view && img.buffer_view->buffer->data) {
        bytes = static_cast<const u8*>(img.buffer_view->buffer->data) + img.buffer_view->offset;
        size = img.buffer_view->size;
    } else if (img.uri) {
        const std::string_view uri(img.uri); // data:<mime>;base64,<payload>
        const std::size_t comma = uri.find(',');
        if (comma == std::string_view::npos || uri.substr(0, comma).find(";base64") == std::string_view::npos) return {};
        const std::string_view payload = uri.substr(comma + 1);
        size = payload.size() / 4 * 3 - (payload.ends_with('=') ? 1 : 0) - (payload.ends_with("==") ? 1 : 0);
        if (cgltf_load_buffer_base64(&opt, size, payload.data(), &decoded) != cgltf_result_success) return {};
        bytes = decoded;
    }
    if (!bytes) return {};
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(model, ec).time_since_epoch().count();
    const std::filesystem::path dir = cacheDir();
    const std::filesystem::path out =
        dir / std::format("{:016x}_{}{}", std::hash<std::string>{}(std::format("{}|{}", model.string(), stamp)), index, imageExtension(img));
    if (!std::filesystem::exists(out, ec)) {
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path tmp = out.string() + ".tmp";
        std::ofstream f(tmp, std::ios::binary);
        f.write(static_cast<const char*>(bytes), std::streamsize(size));
        f.close();
        std::filesystem::rename(tmp, out, ec);
    }
    if (decoded) std::free(decoded);
    return out.string();
}

struct SurfaceMaterial {
    Material material;
    bool doubleSided = false;
};

SurfaceMaterial materialOf(const std::filesystem::path& model, const cgltf_data& data, const cgltf_material* m, const cgltf_options& opt,
                           std::vector<std::string>& images) {
    if (!m) return {};
    Material out;
    std::array<float, 4> base{1, 1, 1, 1};
    const cgltf_texture* tex = nullptr;
    if (m->has_pbr_metallic_roughness) {
        std::copy_n(m->pbr_metallic_roughness.base_color_factor, 4, base.begin());
        tex = m->pbr_metallic_roughness.base_color_texture.texture;
    } else if (m->has_pbr_specular_glossiness) {
        std::copy_n(m->pbr_specular_glossiness.diffuse_factor, 4, base.begin());
        tex = m->pbr_specular_glossiness.diffuse_texture.texture;
    }
    out.color = packColor(base, m->alpha_mode == cgltf_alpha_mode_opaque ? 1.0f : base[3]);
    std::array<float, 3> e{m->emissive_factor[0], m->emissive_factor[1], m->emissive_factor[2]};
    if (m->has_emissive_strength)
        for (float& x : e) x *= m->emissive_strength.emissive_strength;
    if (m->unlit) std::copy_n(base.begin(), 3, e.begin()); // KHR_materials_unlit: shows its colour in the dark
    out.emissive = packColor(e, 0.0f) & 0xFFFFFFu;
    if (tex && tex->image) {
        const std::size_t i = std::size_t(tex->image - data.images);
        if (images[i].empty()) images[i] = imagePath(model, data, i, opt);
        if (images[i].empty()) images[i] = "-";
        if (images[i] != "-") out.texture = images[i];
        if (tex->sampler && (tex->sampler->wrap_s == 33071 || tex->sampler->wrap_t == 33071)) out.wrap = WrapMode::Clamp;
        else out.wrap = WrapMode::Repeat;
    }
    return {out, bool(m->double_sided)};
}

Expected<SubMesh> readPrimitive(const cgltf_primitive& p) {
    SubMesh sm;
    const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr;
    for (cgltf_size i = 0; i < p.attributes_count; ++i) {
        const cgltf_attribute& a = p.attributes[i];
        if (a.type == cgltf_attribute_type_position) pos = a.data;
        else if (a.type == cgltf_attribute_type_normal) nrm = a.data;
        else if (a.type == cgltf_attribute_type_texcoord && a.index == 0) uv = a.data;
    }
    if (!pos) return sm; // nothing to draw
    if (p.has_draco_mesh_compression) return Unexpected("Draco-compressed meshes are not supported");
    sm.vertices.resize(pos->count);
    for (cgltf_size i = 0; i < pos->count; ++i) {
        MeshVertex& v = sm.vertices[i];
        std::array<float, 4> f{};
        cgltf_accessor_read_float(pos, i, f.data(), 3);
        v.pos = {-f[0], f[1], f[2]};
        if (nrm && cgltf_accessor_read_float(nrm, i, f.data(), 3)) v.normal = {-f[0], f[1], f[2]};
        else v.normal = glm::vec3(0.0f);
        if (uv && cgltf_accessor_read_float(uv, i, f.data(), 2)) v.uv = {f[0], f[1]};
        else v.uv = glm::vec2(0.0f);
    }
    std::vector<u32> idx;
    const cgltf_size n = p.indices ? p.indices->count : pos->count;
    idx.resize(n);
    for (cgltf_size i = 0; i < n; ++i) idx[i] = p.indices ? u32(cgltf_accessor_read_index(p.indices, i)) : u32(i);
    switch (p.type) {
    case cgltf_primitive_type_triangles: sm.indices = std::move(idx); break;
    case cgltf_primitive_type_triangle_strip:
        for (std::size_t i = 2; i < idx.size(); ++i) {
            if (i % 2) sm.indices.insert(sm.indices.end(), {idx[i - 1], idx[i - 2], idx[i]});
            else sm.indices.insert(sm.indices.end(), {idx[i - 2], idx[i - 1], idx[i]});
        }
        break;
    case cgltf_primitive_type_triangle_fan:
        for (std::size_t i = 2; i < idx.size(); ++i) sm.indices.insert(sm.indices.end(), {idx[0], idx[i - 1], idx[i]});
        break;
    default: sm.vertices.clear(); return sm; // points / lines: not drawn
    }
    if (std::ranges::any_of(sm.indices, [&](u32 i) { return i >= sm.vertices.size(); })) return Unexpected("index out of range");
    if (!nrm) { // smooth normals from the faces
        for (std::size_t t = 0; t + 2 < sm.indices.size(); t += 3) {
            MeshVertex &a = sm.vertices[sm.indices[t]], &b = sm.vertices[sm.indices[t + 1]], &c = sm.vertices[sm.indices[t + 2]];
            const glm::vec3 fn = glm::cross(c.pos - a.pos, b.pos - a.pos); // clockwise front faces
            a.normal += fn, b.normal += fn, c.normal += fn;
        }
        for (MeshVertex& v : sm.vertices) {
            const float l = glm::length(v.normal);
            v.normal = l > 1e-12f ? v.normal / l : glm::vec3(0, 1, 0);
        }
    }
    return sm;
}

} // namespace

std::optional<std::size_t> GltfScene::find(std::string_view name) const {
    const auto it = std::ranges::find(nodes, name, &GltfNode::name);
    return it != nodes.end() ? std::optional(std::size_t(it - nodes.begin())) : std::nullopt;
}

Expected<GltfScene> loadGltf(const std::filesystem::path& path) {
    cgltf_options opt{};
    cgltf_data* raw = nullptr;
    const std::string file = path.string();
    if (cgltf_parse_file(&opt, file.c_str(), &raw) != cgltf_result_success) return Unexpected("not a glTF 2.0 file");
    const std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(raw, &cgltf_free);
    if (cgltf_load_buffers(&opt, data.get(), file.c_str()) != cgltf_result_success) return Unexpected("cannot read the glTF buffers");
    GltfScene out;

    std::vector<std::string> images(data->images_count);
    // meshes are per node (a glTF mesh can be shared by several nodes; each gets its copy)
    out.nodes.assign(data->nodes_count, {});
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const cgltf_node& n = data->nodes[i];
        GltfNode& g = out.nodes[i];
        g.name = n.name ? n.name : (n.mesh && n.mesh->name ? n.mesh->name : std::format("node{}", i));
        if (n.parent) g.parent = std::size_t(n.parent - data->nodes);
        for (cgltf_size c = 0; c < n.children_count; ++c) g.children.push_back(std::size_t(n.children[c] - data->nodes));
        std::array<float, 16> m;
        cgltf_node_transform_world(&n, m.data());
        g.world = kMirror * glm::dmat4(glm::make_mat4(m.data())) * kMirror;
        if (!n.mesh) continue;
        StaticMesh mesh;
        for (cgltf_size p = 0; p < n.mesh->primitives_count; ++p) {
            const cgltf_primitive& prim = n.mesh->primitives[p];
            auto primitive = readPrimitive(prim);
            if (!primitive) return Unexpected(std::format("{}: {}", g.name, primitive.error()));
            SubMesh& sm = *primitive;
            if (sm.indices.empty()) continue;
            auto [material, doubleSided] = materialOf(path, *data, prim.material, opt, images);
            sm.material = std::move(material);
            sm.doubleSided = doubleSided;
            mesh.parts.push_back(std::move(sm));
        }
        if (mesh.parts.empty()) continue;
        g.mesh = out.meshes.size();
        out.meshes.push_back(std::move(mesh));
    }
    if (out.meshes.empty()) return Unexpected("the model has no triangles");
    return out;
}

RigidPose gltfFrame(const glm::dmat4& w) {
    // Gram-Schmidt: drop scale (and shear) so the element rotates rigidly about the origin
    glm::dvec3 x = glm::dvec3(w[0]), y = glm::dvec3(w[1]), z;
    x = glm::length(x) > 1e-12 ? glm::normalize(x) : glm::dvec3(1, 0, 0);
    y -= x * glm::dot(x, y);
    y = glm::length(y) > 1e-12 ? glm::normalize(y) : glm::dvec3(0, 1, 0);
    z = glm::cross(x, y); // a mirrored node's reflection stays in its baked mesh
    return {glm::dmat3(x, y, z), glm::dvec3(w[3])};
}

void gltfBake(const GltfScene& scene, std::span<const std::size_t> roots, const std::vector<bool>* skip, const glm::dmat4& frame, StaticMesh& out) {
    std::vector<std::size_t> stack(roots.begin(), roots.end());
    if (stack.empty())
        for (std::size_t i = 0; i < scene.nodes.size(); ++i)
            if (!scene.nodes[i].parent) stack.push_back(i);
    while (!stack.empty()) {
        const std::size_t n = stack.back();
        stack.pop_back();
        if (skip && (*skip)[n]) continue;
        const GltfNode& g = scene.nodes[n];
        stack.insert(stack.end(), g.children.begin(), g.children.end());
        if (!g.mesh) continue;
        const glm::dmat4 t = frame * g.world;
        const glm::dmat3 nm = glm::transpose(glm::inverse(glm::dmat3(t)));
        const bool flip = glm::determinant(glm::dmat3(t)) < 0.0;
        for (const SubMesh& src : scene.meshes[*g.mesh].parts) {
            auto it = std::find_if(out.parts.begin(), out.parts.end(),
                                   [&](const SubMesh& s) { return s.material == src.material && s.doubleSided == src.doubleSided; });
            if (it == out.parts.end()) {
                out.parts.push_back({src.material, src.doubleSided, {}, {}});
                it = out.parts.end() - 1;
            }
            const u32 base = u32(it->vertices.size());
            for (const MeshVertex& v : src.vertices) {
                MeshVertex o = v;
                o.pos = glm::vec3(t * glm::dvec4(v.pos, 1.0));
                const glm::dvec3 nn = nm * glm::dvec3(v.normal);
                o.normal = glm::length(nn) > 1e-12 ? glm::vec3(glm::normalize(nn)) : v.normal;
                it->vertices.push_back(o);
            }
            for (std::size_t i = 0; i + 2 < src.indices.size(); i += 3) {
                const u32 a = base + src.indices[i], b = base + src.indices[i + 1], c = base + src.indices[i + 2];
                it->indices.insert(it->indices.end(), flip ? std::initializer_list<u32>{a, c, b} : std::initializer_list<u32>{a, b, c});
            }
        }
    }
}

} // namespace ots
