#version 460
#include "bindless.glsl"
// FXAA on the tonemapped (display-referred) image: edge detection on luma, search along
// the edge, blend across it; plus sub-pixel aliasing removal.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
vec3 tap(vec2 uv) { return texture(gTextures[TEX_LDR], uv).rgb; }
void main() {
    vec2 px = 1.0 / vec2(textureSize(gTextures[TEX_LDR], 0));
    vec2 uv = gl_FragCoord.xy * px;
    vec3 cM = tap(uv);
    float lM = luma(cM);
    float lN = luma(tap(uv + vec2(0, -px.y))), lS = luma(tap(uv + vec2(0, px.y)));
    float lE = luma(tap(uv + vec2(px.x, 0))), lW = luma(tap(uv + vec2(-px.x, 0)));
    float lMin = min(lM, min(min(lN, lS), min(lE, lW))), lMax = max(lM, max(max(lN, lS), max(lE, lW)));
    float range = lMax - lMin;
    if (range < max(0.0312, lMax * 0.125)) { outColor = vec4(cM, 1.0); return; }
    float lNW = luma(tap(uv + vec2(-px.x, -px.y))), lNE = luma(tap(uv + vec2(px.x, -px.y)));
    float lSW = luma(tap(uv + vec2(-px.x, px.y))), lSE = luma(tap(uv + vec2(px.x, px.y)));
    float lNS = lN + lS, lWE = lW + lE;
    float edgeH = abs(-2.0 * lW + lNW + lSW) + 2.0 * abs(-2.0 * lM + lNS) + abs(-2.0 * lE + lNE + lSE);
    float edgeV = abs(-2.0 * lN + lNW + lNE) + 2.0 * abs(-2.0 * lM + lWE) + abs(-2.0 * lS + lSW + lSE);
    bool horizontal = edgeH >= edgeV;
    float l1 = horizontal ? lN : lW, l2 = horizontal ? lS : lE;
    float g1 = abs(l1 - lM), g2 = abs(l2 - lM);
    bool steep1 = g1 >= g2;
    float gScaled = 0.25 * max(g1, g2);
    float stepLen = horizontal ? px.y : px.x;
    float lLocal;
    if (steep1) { stepLen = -stepLen; lLocal = 0.5 * (l1 + lM); } else { lLocal = 0.5 * (l2 + lM); }
    vec2 cur = uv;
    if (horizontal) cur.y += stepLen * 0.5; else cur.x += stepLen * 0.5;
    vec2 off = horizontal ? vec2(px.x, 0) : vec2(0, px.y);
    const float kSteps[10] = float[](1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 4.0, 8.0);
    vec2 uv1 = cur - off, uv2 = cur + off;
    float e1 = luma(tap(uv1)) - lLocal, e2 = luma(tap(uv2)) - lLocal;
    bool r1 = abs(e1) >= gScaled, r2 = abs(e2) >= gScaled;
    for (int i = 0; i < 10 && !(r1 && r2); ++i) {
        if (!r1) { uv1 -= off * kSteps[i]; e1 = luma(tap(uv1)) - lLocal; r1 = abs(e1) >= gScaled; }
        if (!r2) { uv2 += off * kSteps[i]; e2 = luma(tap(uv2)) - lLocal; r2 = abs(e2) >= gScaled; }
    }
    float d1 = horizontal ? uv.x - uv1.x : uv.y - uv1.y, d2 = horizontal ? uv2.x - uv.x : uv2.y - uv.y;
    bool dir1 = d1 < d2;
    float dMin = min(d1, d2), len = d1 + d2;
    float pixelOffset = -dMin / len + 0.5;
    bool smaller = lM < lLocal;
    bool correct = ((dir1 ? e1 : e2) < 0.0) != smaller;
    float finalOffset = correct ? pixelOffset : 0.0;
    float lAvg = (2.0 * (lNS + lWE) + lNW + lNE + lSW + lSE) / 12.0;
    float sub = clamp(abs(lAvg - lM) / range, 0.0, 1.0);
    sub = (-2.0 * sub + 3.0) * sub * sub;
    finalOffset = max(finalOffset, sub * sub * 0.75);
    vec2 fuv = uv;
    if (horizontal) fuv.y += finalOffset * stepLen; else fuv.x += finalOffset * stepLen;
    outColor = vec4(tap(fuv), 1.0);
}
