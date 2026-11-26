#version 460
#include "common.glsl"
// Physical sky: one full-screen triangle at the far plane; the fragment shader rebuilds the
// view direction from Frame.invViewProj.
layout(location = 0) out vec2 vNdc;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
    vNdc = p;
    gl_Position = vec4(p, 0.0, 1.0); // farthest depth (reverse-Z)
}
