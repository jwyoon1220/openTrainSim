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
    mat4 shadow[4];      // camera-relative → shadow clip, per cascade
    vec4 shadowCascade[4]; // x: texel world size, y: far split
    vec4 shadowParams;   // x: cascades (0 = off), y: atlas texel count, z: atlas slot, w: strength
    vec4 lighting;       // x: hemisphere ambient, y: sun specular, z: unused, w: unused
    vec4 post;           // x: bloom, y: saturation, z: contrast, w: vignette
    mat4 invViewProj;          // clip → camera-relative world
    mat4 equatorialFromWorld;  // world direction → J2000 equatorial (3×3)
    vec4 sunDir;               // xyz: towards the sun, w: moon lit fraction
    vec4 moonDir;
    vec4 sky;                  // x: physical sky on, y: exposure, z: stars, w: Milky Way (Gaia)
    vec4 skyMaps;              // x: star map slot, y: Gaia slot, z: star map HDR scale
    vec4 fogSun;               // physical sky: horizon (fog) colour towards the sun; fogColor = away
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
    uint cascade; // shadow pass
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
// Sun shadow (1 lit, 0 shadowed) at a camera-relative position with geometric normal n.
// Cascades live in a 2×2 atlas; 3×3 bilinear PCF (16 texel fetches).
float sunShadow(Frame fr, vec3 pos, vec3 n, float ndl) {
    int cascades = int(fr.shadowParams.x);
    if (cascades == 0) return 1.0;
    float size = fr.shadowParams.y;
    uint slot = uint(fr.shadowParams.z);
    for (int k = 0; k < cascades; ++k) {
        // normal offset grows at grazing angles
        vec3 p = pos + n * fr.shadowCascade[k].x * (1.5 + 2.0 * (1.0 - ndl));
        vec4 c = fr.shadow[k] * vec4(p, 1.0);
        float margin = 4.0 / (0.5 * size);
        if (any(greaterThan(abs(c.xy), vec2(1.0 - margin))) || c.z < 0.0 || c.z > 1.0) continue;
        vec2 uv = (c.xy * 0.5 + 0.5 + vec2(k & 1, k >> 1)) * 0.5;
        vec2 t = uv * size - 0.5;
        ivec2 b = ivec2(floor(t)) - 1;
        vec2 f = fract(t);
        float z = c.z - 0.0005 * fr.shadowCascade[k].x;
        float s[16];
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i)
                s[j * 4 + i] = z <= texelFetch(gTextures[nonuniformEXT(slot)], b + ivec2(i, j), 0).r ? 1.0 : 0.0;
        float sum = 0.0;
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i)
                sum += mix(mix(s[j * 4 + i], s[j * 4 + i + 1], f.x), mix(s[(j + 1) * 4 + i], s[(j + 1) * 4 + i + 1], f.x), f.y);
        float lit = sum / 9.0;
        // fade out towards the end of the shadowed range
        if (k == cascades - 1) lit = mix(lit, 1.0, smoothstep(0.85, 1.0, length(pos) / fr.shadowCascade[k].y));
        return mix(1.0, lit, fr.shadowParams.w);
    }
    return 1.0;
}

// Motion vector (previous − current) in UV units from unjittered clip positions.
vec2 motionUv(vec4 cur, vec4 prev) { return (prev.xy / prev.w - cur.xy / cur.w) * 0.5; }
#endif
