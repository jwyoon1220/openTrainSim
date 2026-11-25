#pragma once
// Cascaded shadow maps for one directional light (the sun): stable cascade fitting.
//
// Each cascade covers a slice of the view frustum with an orientation-independent bounding
// sphere (its size never changes while the camera turns) whose centre is snapped to the shadow
// map's texel grid in absolute world space, so shadows do not shimmer when the camera moves or
// rotates. Matrices map camera-relative positions (world − camera) to Vulkan clip space:
// x, y in [-1, 1], depth in [0, 1] (0 = towards the light).
#include "phx/core/Types.hpp"

#include <glm/glm.hpp>

#include <span>

namespace phx {

struct ShadowCascadeInput {
    glm::dvec3 camera{0.0};                       // absolute world position (snapping)
    glm::vec3 forward{0, 0, 1};                   // camera axes
    f32 fovY = 1.0f, aspect = 1.0f;               // radians, width / height
    f32 nearPlane = 0.1f, distance = 300.0f;      // shadowed range
    glm::vec3 toLight{0, 1, 0};                   // unit vector towards the light
    u32 resolution = 2048;                        // texels per cascade side
    f32 splitLambda = 0.8f;                       // 0 linear … 1 logarithmic splits
    f32 casterExtent = 500.0f;                    // depth range towards the light beyond the slice
};

struct ShadowCascade {
    glm::mat4 viewProj{1.0f};     // camera-relative → clip
    glm::vec3 center{0.0f};       // camera-relative sphere centre
    f32 radius = 0.0f;
    f32 nearSplit = 0.0f, farSplit = 0.0f; // view distance range
    f32 texelWorld = 0.0f;        // world size of one shadow texel
    glm::vec3 lightX{1, 0, 0}, lightY{0, 1, 0}, lightZ{0, 0, 1}; // light space axes (Z: away from the light)
    // Could a sphere (camera-relative) cast into this cascade?
    [[nodiscard]] bool covers(const glm::vec3& c, f32 r) const;
};

void computeShadowCascades(const ShadowCascadeInput& in, std::span<ShadowCascade> out);

} // namespace phx
