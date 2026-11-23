#version 460
#include "common.glsl"
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vCur;
layout(location = 2) in vec4 vPrev;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outMotion;
void main() {
    Frame fr = FrameRef(pc.frame).f;
    uint slot = uint(fr.misc.x);
    vec4 c = slot != 0u ? texture(gTextures[nonuniformEXT(slot)], vUv) : vec4(fr.fogColor.rgb, 1.0);
    outColor = vec4(c.rgb, fr.misc.y);
    outMotion = motionUv(vCur, vPrev);
}
