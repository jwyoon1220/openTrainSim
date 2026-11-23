#version 460
#include "common.glsl"
// openBVE static background: 32-segment cylinder (radius, y0, y1), texture repeated around it,
// plus top / bottom caps that take the texture's edge rows.
layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vCur;
layout(location = 2) out vec4 vPrev;
const int N = 32;
void main() {
    Frame fr = FrameRef(pc.frame).f;
    int seg = gl_VertexIndex / 12;
    int k = gl_VertexIndex % 12;
    int j = (seg + 1) % N;
    float a0 = 2.61799387799149 - 3.14159265358979 / float(N) + float(seg) * 6.28318530717958 / float(N);
    float a1 = a0 + 6.28318530717958 / float(N);
    float r = fr.background.x, y0 = fr.background.y, y1 = fr.background.z, rep = fr.background.w;
    float u0 = 0.5 * rep / float(N) - float(seg) * rep / float(N);
    float u1 = u0 - rep / float(N);
    vec3 b0 = vec3(r * cos(a0), y0, r * sin(a0)), b1 = vec3(r * cos(a1), y0, r * sin(a1));
    vec3 t0 = vec3(b0.x, y1, b0.z), t1 = vec3(b1.x, y1, b1.z);
    vec3 p; vec2 uv;
    // wall: two triangles (t0 b0 b1) (t0 b1 t1); caps: (t1 t0 top) (b0 b1 bottom)
    if (k == 0) { p = t0; uv = vec2(u0, 0.005); }
    else if (k == 1) { p = b0; uv = vec2(u0, 0.995); }
    else if (k == 2) { p = b1; uv = vec2(u1, 0.995); }
    else if (k == 3) { p = t0; uv = vec2(u0, 0.005); }
    else if (k == 4) { p = b1; uv = vec2(u1, 0.995); }
    else if (k == 5) { p = t1; uv = vec2(u1, 0.005); }
    else if (k == 6) { p = t1; uv = vec2(u1, 0.005); }
    else if (k == 7) { p = t0; uv = vec2(u0, 0.005); }
    else if (k == 8) { p = vec3(0, 2.0 * y1, 0); uv = vec2(0.5 * (u0 + u1), 0.005); }
    else if (k == 9) { p = b0; uv = vec2(u0, 0.995); }
    else if (k == 10) { p = b1; uv = vec2(u1, 0.995); }
    else { p = vec3(0, 2.0 * y0, 0); uv = vec2(0.5 * (u0 + u1), 0.995); }
    vUv = uv;
    vec4 clip = fr.viewProj * vec4(p, 1.0);
    vCur = fr.viewProjNoJitter * vec4(p, 1.0);  // at infinity: rotation only
    vPrev = fr.prevViewProj * vec4(p, 1.0);
    gl_Position = vec4(clip.xy, 0.0, clip.w); // farthest depth (reverse-Z)
}
