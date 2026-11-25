#pragma once
// In-memory openBVE objects. Coordinates follow openBVE: left-handed, X right, Y up, Z forward;
// front faces are wound clockwise as seen by the viewer.
#include "bve/FunctionScript.hpp"
#include "phx/core/Types.hpp"

#include <glm/glm.hpp>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace ots {
using namespace phx; // u8 / u32 / f32 ...

enum class MaterialBlend : u8 { Normal, Additive };
enum class WrapMode : u8 { Auto, Repeat, Clamp };

struct Material {
    u32 color = 0xFFFFFFFFu;    // RGBA8 (r in the low byte)
    u32 emissive = 0;           // RGB8, 0 = none
    std::string texture;        // resolved absolute path, empty = untextured
    bool colorKey = false;      // decal transparent colour
    u8 keyR = 0, keyG = 0, keyB = 0;
    MaterialBlend blend = MaterialBlend::Normal;
    WrapMode wrap = WrapMode::Auto;
    u16 glowHalfDistance = 0;   // additive glow fade (openBVE SetBlendMode)
    bool operator==(const Material&) const = default;
};

struct MeshVertex {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec2 uv;
};

// One draw: triangles sharing a material and a sidedness.
struct SubMesh {
    Material material;
    bool doubleSided = false;
    std::vector<MeshVertex> vertices;
    std::vector<u32> indices;
};

struct StaticMesh {
    std::vector<SubMesh> parts;
    std::string warning; // a non-fatal load problem (e.g. a missing texture), empty if none
    glm::vec3 bmin{0.0f}, bmax{0.0f};
    void computeBounds();
    [[nodiscard]] bool empty() const { return parts.empty(); }
    [[nodiscard]] std::size_t triangleCount() const;
    void transform(const glm::mat3& m, const glm::vec3& t); // rotation/scale + translation
    void mirrorX();
    // openBVE StaticObject.Transform: platform/crack deformation between two rails.
    void deformBetweenRails(double nearDistance, double farDistance);
};

// A rigid placement: rotation (columns = local axes) and origin.
struct RigidPose {
    glm::dmat3 rotation{1.0};
    glm::dvec3 position{0.0};
};

// Damped follower used by Rotate*Damping (natural frequency, damping ratio).
struct DampingSpec {
    f32 frequency = 0.0f, ratio = 0.0f;
    [[nodiscard]] bool enabled() const { return frequency > 0.0f; }
};

using ObjectId = u32;
inline constexpr ObjectId kNoObject = ~0u;

// [Object] section of an .animated file.
struct AnimatedPart {
    std::vector<ObjectId> states;   // kNoObject = empty state
    glm::vec3 position{0.0f};
    std::shared_ptr<Function> stateFn;
    std::array<glm::vec3, 3> translateDir{glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)};
    std::array<std::shared_ptr<Function>, 3> translateFn;
    std::array<glm::vec3, 3> rotateDir{glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)};
    std::array<std::shared_ptr<Function>, 3> rotateFn;
    std::array<DampingSpec, 3> rotateDamping;
    std::array<glm::vec2, 2> textureShiftDir{glm::vec2(1, 0), glm::vec2(0, 1)};
    std::array<std::shared_ptr<Function>, 2> textureShiftFn;
    f32 refreshRate = 0.0f;
    [[nodiscard]] bool isStatic() const {
        return !stateFn && !translateFn[0] && !translateFn[1] && !translateFn[2] && !rotateFn[0] && !rotateFn[1] && !rotateFn[2] &&
               !textureShiftFn[0] && !textureShiftFn[1];
    }
};

struct AnimatedDef {
    std::vector<AnimatedPart> parts;
};

} // namespace ots
