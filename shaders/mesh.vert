#version 460
#include "common.glsl"
layout(location = 0) out vec2 vUv;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vView;   // camera-relative position
layout(location = 3) out float vBrightness;
layout(location = 4) out vec4 vCur;
layout(location = 5) out vec4 vPrev;
layout(location = 6) flat out float vRepeat;

void main() {
    Vertex v = VertexRef(pc.vertices).v[gl_VertexIndex];
    Instance inst = InstanceRef(pc.instances).i[gl_InstanceIndex];
    vec4 p = vec4(v.pos, 1.0);
    vec3 world = vec3(dot(inst.row0, p), dot(inst.row1, p), dot(inst.row2, p));
    mat3 rot = transpose(mat3(inst.row0.xyz, inst.row1.xyz, inst.row2.xyz));
    vNormal = rot * octDecode(v.normal);
    vUv = v.uv + inst.params.xy;
    vRepeat = inst.params.w;
    vView = world;
    vBrightness = inst.params.z;
    Frame fr = FrameRef(pc.frame).f;
    gl_Position = fr.viewProj * vec4(world, 1.0);
    vCur = fr.viewProjNoJitter * vec4(world, 1.0);
    vPrev = fr.prevViewProj * vec4(world + inst.prev.xyz, 1.0);
}
