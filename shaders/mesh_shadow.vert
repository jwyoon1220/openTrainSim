#version 460
// Sun shadow map: depth of opaque / alpha-tested geometry, one cascade per pass (pc.cascade).
#include "common.glsl"
layout(location = 0) out vec2 vUv;
layout(location = 1) flat out float vRepeat;

void main() {
    Vertex v = VertexRef(pc.vertices).v[gl_VertexIndex];
    Instance inst = InstanceRef(pc.instances).i[gl_InstanceIndex];
    vec4 p = vec4(v.pos, 1.0);
    vec3 world = vec3(dot(inst.row0, p), dot(inst.row1, p), dot(inst.row2, p));
    vUv = v.uv + inst.params.xy;
    vRepeat = inst.params.w;
    gl_Position = FrameRef(pc.frame).f.shadow[pc.cascade] * vec4(world, 1.0);
}
