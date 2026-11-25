#version 460
#include "common.glsl"
// Output transform. openBVE lighting is display-referred already (LDR), so this is not a tone
// curve but a light grade: bloom around bright areas (sky, lamps), a gentle S-curve, saturation
// and a faint vignette (all from Frame.post; zero = off). TEX_HDR is the scene, or the DLSS /
// TAA / FSR / SSAA output.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    Frame fr = FrameRef(pc.frame).f;
    vec3 c = texture(gTextures[TEX_HDR], vUV).rgb;
    if (fr.post.x > 0.0) {
        // the bloom chain sums its 6 levels: average, then only what is near white glows
        vec3 b = texture(gTextures[TEX_BLOOM0], vUV).rgb / 6.0;
        c += fr.post.x * 1.5 * max(b - 0.7, 0.0);
    }
    c = clamp(c, 0.0, 1.0);
    if (fr.post.z > 0.0) c = mix(c, c * c * (3.0 - 2.0 * c), fr.post.z);
    if (fr.post.y != 1.0) c = clamp(mix(vec3(dot(c, vec3(0.2126, 0.7152, 0.0722))), c, fr.post.y), 0.0, 1.0);
    if (fr.post.w > 0.0) {
        vec2 d = vUV - 0.5;
        c *= 1.0 - fr.post.w * smoothstep(0.2, 0.75, dot(d, d) * 2.0);
    }
    outColor = vec4(c, 1.0);
}
