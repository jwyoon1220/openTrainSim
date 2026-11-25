#pragma once
// openBVE MeshBuilder semantics shared by the CSV/B3D and X loaders: vertices + polygon faces
// with per-face materials, transformed in place, then triangulated into StaticMesh parts.
#include "bve/Object.hpp"
#include "util/Expected.hpp"

#include <filesystem>

namespace ots {

struct BuilderVertex {
    glm::vec3 pos{0.0f};
    glm::vec3 normal{0.0f}; // zero = use the face normal
    glm::vec2 uv{0.0f};
};

struct BuilderFace {
    std::vector<u32> idx;
    std::vector<glm::vec3> normals; // optional per corner (primitives / X normals)
    u32 material = 0;
    bool face2 = false;     // double sided
    bool triangles = false; // idx is a triangle list instead of one polygon
};

struct MeshBuilder {
    std::vector<BuilderVertex> vertices;
    std::vector<BuilderFace> faces;
    std::vector<Material> materials{Material{}};

    void transform(const glm::mat3& m, const glm::vec3& t);
    void reverseFaces();
    void addCube(const glm::vec3& half);
    void addCylinder(int n, float upperRadius, float lowerRadius, float height);
    // Triangulates into `out` (appends parts) and resets the builder.
    void flush(StaticMesh& out);
};

// Merges parts with equal material/sidedness (fewer draws); splits nothing.
void mergeParts(StaticMesh& mesh);
// Rotation about an arbitrary axis (Rodrigues), as openBVE's Vector3.Rotate.
glm::mat3 axisRotation(glm::vec3 axis, float radians);

// Loaders (return false + message on failure).
Expected<StaticMesh> loadCsvB3d(const std::filesystem::path& path);
Expected<StaticMesh> loadX(const std::filesystem::path& path);

} // namespace ots
