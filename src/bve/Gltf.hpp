#pragma once
// glTF 2.0 (.gltf / .glb) models, the format Blender and other modelling tools export.
// The scene keeps its node hierarchy so a cab script can pick parts by node name (levers,
// buttons, needles) and animate them about their own origin, as modelled.
//
// Coordinates are converted to openBVE's on load: glTF is right-handed (+Y up, +Z forward,
// +X left as seen from behind), openBVE is left-handed with +X right, so X is negated. The
// winding flips with it, which matches openBVE's clockwise front faces.
#include "bve/Object.hpp"
#include "util/Expected.hpp"

#include <span>
#include <optional>
#include <filesystem>
#include <string>
#include <vector>

namespace ots {

struct GltfNode {
    std::string name;
    std::optional<std::size_t> parent;
    std::vector<std::size_t> children;
    std::optional<std::size_t> mesh; // index into GltfScene::meshes
    glm::dmat4 world{1.0};           // node -> model, openBVE coordinates
};

struct GltfScene {
    std::vector<GltfNode> nodes;
    std::vector<StaticMesh> meshes; // in their node's local frame, openBVE coordinates
    [[nodiscard]] std::optional<std::size_t> find(std::string_view name) const;
};

Expected<GltfScene> loadGltf(const std::filesystem::path& path);

// Rigid frame of a node world matrix: rotation (scale removed) and origin.
RigidPose gltfFrame(const glm::dmat4& world);

// Bakes the meshes of the given subtrees (all roots when `roots` is empty) into one mesh,
// expressed in `frame` (model -> frame). Nodes with skip[i] set are left out with their subtree.
void gltfBake(const GltfScene& scene, std::span<const std::size_t> roots, const std::vector<bool>* skip, const glm::dmat4& frame, StaticMesh& out);

} // namespace ots
