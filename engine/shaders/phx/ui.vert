#version 460
#include "bindless.glsl"
// UI triangles (phx::UiRenderer): phx::ui::Vertex, pixel coordinates, top-left origin.
struct UiVertex { vec2 pos; vec2 uv; uint rgba; uint clipMin; uint clipMax; uint texture; };
layout(buffer_reference, scalar) readonly buffer UiVertices { UiVertex v[]; };
layout(push_constant, scalar) uniform Push { uint64_t vertices; uint width; uint height; uint atlas; } pc;
layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUv;
layout(location = 2) flat out uvec4 vClip;   // x0 y0 x1 y1 (pixels)
layout(location = 3) flat out uint vTexture; // 0 = font atlas coverage
void main() {
    UiVertex v = UiVertices(pc.vertices).v[gl_VertexIndex];
    vec2 size = vec2(float(pc.width), float(pc.height));
    gl_Position = vec4(v.pos / size * 2.0 - 1.0, 0.0, 1.0);
    vColor = unpackUnorm4x8(v.rgba); // already display-encoded (UNORM target)
    vUv = v.uv;
    vClip = uvec4(v.clipMin & 0xFFFFu, v.clipMin >> 16, v.clipMax & 0xFFFFu, v.clipMax >> 16);
    vTexture = v.texture;
}
