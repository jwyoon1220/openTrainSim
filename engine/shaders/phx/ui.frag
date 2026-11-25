#version 460
#include "bindless.glsl"
layout(push_constant, scalar) uniform Push { uint64_t vertices; uint width; uint height; uint atlas; } pc;
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUv;
layout(location = 2) flat in uvec4 vClip;
layout(location = 3) flat in uint vTexture;
layout(location = 0) out vec4 outColor;
void main() {
    vec2 p = gl_FragCoord.xy;
    if (p.x < float(vClip.x) || p.y < float(vClip.y) || p.x >= float(vClip.z) || p.y >= float(vClip.w)) discard;
    if (vTexture == 0u) {
        // font atlas: coverage (solid shapes sample its white texel)
        outColor = vec4(vColor.rgb, vColor.a * texture(gTextures[nonuniformEXT(pc.atlas)], vUv).r);
    } else {
        outColor = vColor * texture(gTextures[nonuniformEXT(vTexture)], vUv);
    }
}
