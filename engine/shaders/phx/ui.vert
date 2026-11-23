#version 460
#include "bindless.glsl"
// UI / overlay quads (phx::UiRenderer): 4 phx::ui::Vertex per quad, pixel coordinates.
struct OverlayVertex { vec3 pos; uint rgba; };
layout(buffer_reference, scalar) readonly buffer OverlayVertices { OverlayVertex v[]; };
layout(location = 0) out vec4 vColor;
layout(push_constant, scalar) uniform Push { uint64_t vertices; uint width; uint height; } pc;
const int kCorner[6] = int[](0, 1, 2, 0, 2, 3);
void main() {
    uint q = uint(gl_VertexIndex) / 6u;
    OverlayVertex v = OverlayVertices(pc.vertices).v[q * 4u + uint(kCorner[gl_VertexIndex % 6])];
    vec2 size = vec2(float(pc.width), float(pc.height));
    gl_Position = vec4(v.pos.xy / size * 2.0 - 1.0, 0.0, 1.0);
    vColor = unpackUnorm4x8(v.rgba); // already display-encoded (UNORM swapchain)
}
