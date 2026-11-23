#ifndef OTS_COMMON_GLSL
#define OTS_COMMON_GLSL
// Mirrors src/render/GpuTypes.hpp (scalar layout).
#include "phx/bindless.glsl"

struct Vertex { vec3 pos; uint normal; vec2 uv; };
struct Instance { vec4 row0; vec4 row1; vec4 row2; vec4 params; vec4 prev; };
struct Material { uint texture; uint color; uint emissive; uint flags; };
struct Frame {
    mat4 viewProj;         // jittered
    mat4 viewProjNoJitter;
    mat4 prevViewProj;     // previous frame (unjittered), previous-camera-relative
    vec4 lightDir;
    vec4 ambient;
    vec4 diffuse;
    vec4 fog;        // start, end, enabled, alpha-to-coverage
    vec4 fogColor;
    vec4 background; // radius, y0, y1, repetitions
    vec4 misc;       // background slot, background alpha, time, mip bias
};
layout(buffer_reference, scalar) readonly buffer FrameRef { Frame f; };
layout(buffer_reference, scalar) readonly buffer VertexRef { Vertex v[]; };
layout(buffer_reference, scalar) readonly buffer InstanceRef { Instance i[]; };
layout(buffer_reference, scalar) readonly buffer MaterialRef { Material m[]; };

layout(push_constant, scalar) uniform Push {
    uint64_t frame;
    uint64_t vertices;
    uint64_t instances;
    uint64_t materials;
    uint material;
} pc;

const uint MAT_TEXTURED = 1u;
const uint MAT_ALPHATEST = 2u;
const uint MAT_BLEND = 4u;
const uint MAT_ADDITIVE = 8u;

vec3 octDecode(uint packed) {
    vec2 e = unpackSnorm2x16(packed);
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    float l = length(n);
    return l > 0.0 ? n / l : vec3(0, 1, 0);
}
// Motion vector (previous − current) in UV units from unjittered clip positions.
vec2 motionUv(vec4 cur, vec4 prev) { return (prev.xy / prev.w - cur.xy / cur.w) * 0.5; }
#endif
