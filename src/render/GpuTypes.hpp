#pragma once
// Structures shared with the shaders (shaders/common.glsl), scalar layout.
#include "phx/core/Types.hpp"

#include <glm/glm.hpp>

#include <array>

namespace ots {
using namespace phx;

// 24 bytes: position, octahedral normal (2×snorm16), uv.
struct GpuVertex {
    f32 px, py, pz;
    u32 normal;
    f32 u, v;
};
static_assert(sizeof(GpuVertex) == 24);

// Camera-relative 3×4 transform (rows) + per-instance parameters.
struct GpuInstance {
    glm::vec4 row0, row1, row2;
    glm::vec4 params; // xy: texture shift (in [0, 1)), z: brightness, w: 1 = sample repeating (shifted)
    glm::vec4 prev;   // xyz: previous camera-relative position − current (motion vectors)
};
static_assert(sizeof(GpuInstance) == 80);

// GpuMaterial::flags bits (shaders/common.glsl reads the same values)
inline constexpr u32 kMatTextured = 1u << 0;
inline constexpr u32 kMatAlphaTest = 1u << 1;
inline constexpr u32 kMatBlend = 1u << 2;
inline constexpr u32 kMatAdditive = 1u << 3;
inline constexpr u32 kMatGlowShift = 16; // bits 16..31: glow half distance (m)

struct GpuMaterial {
    u32 texture; // bindless slot (0 = white fallback)
    u32 color;   // RGBA8
    u32 emissive;
    u32 flags;
};
static_assert(sizeof(GpuMaterial) == 16);

struct GpuFrame {
    glm::mat4 viewProj;     // camera at the origin (instances are camera-relative), jittered
    glm::mat4 viewProjNoJitter;
    glm::mat4 prevViewProj; // previous frame, unjittered, relative to the previous camera
    glm::vec4 lightDir;     // xyz: towards the light
    glm::vec4 ambient;      // rgb
    glm::vec4 diffuse;      // rgb
    glm::vec4 fog;          // start, end, enabled, alpha-to-coverage (1) / discard (0)
    glm::vec4 fogColor;
    glm::vec4 background;   // radius, y0, y1, repetitions
    glm::vec4 misc;         // x: background texture slot, y: background alpha, z: time, w: mip bias
    std::array<glm::mat4, 4> shadow;        // camera-relative → shadow clip, per cascade
    std::array<glm::vec4, 4> shadowCascade; // x: texel world size, y: far split
    glm::vec4 shadowParams;      // x: cascades (0 = off), y: atlas texels, z: atlas slot, w: strength
    glm::vec4 lighting;          // x: hemisphere ambient 0..1, y: sun specular
    glm::vec4 post;              // x: bloom, y: saturation, z: contrast, w: vignette
};

struct PushConstants {
    u64 frame;     // GpuFrame*
    u64 vertices;  // GpuVertex*
    u64 instances; // GpuInstance*
    u64 materials; // GpuMaterial*
    u32 material;
    u32 cascade; // shadow pass
    std::array<u32, 2> pad;
};
static_assert(sizeof(PushConstants) == 48);

inline u32 packOctNormal(glm::vec3 n) {
    const float l1 = std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
    if (l1 <= 0.0f) return 0;
    n /= l1;
    glm::vec2 e(n.x, n.y);
    if (n.z < 0.0f) {
        e = glm::vec2((1.0f - std::abs(n.y)) * (n.x >= 0.0f ? 1.0f : -1.0f), (1.0f - std::abs(n.x)) * (n.y >= 0.0f ? 1.0f : -1.0f));
    }
    auto q = [](float f) { return u32(i32(std::round(glm::clamp(f, -1.0f, 1.0f) * 32767.0f)) & 0xFFFF); };
    return q(e.x) | (q(e.y) << 16);
}

} // namespace ots
