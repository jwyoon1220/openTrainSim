// Fragment body shared by mesh.frag (BLEND_PASS 0: opaque / alpha-tested) and
// mesh_blend.frag (BLEND_PASS 1: blended, sorted).
#include "common.glsl"
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vView;
layout(location = 3) in float vBrightness;
layout(location = 4) in vec4 vCur;
layout(location = 5) in vec4 vPrev;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outMotion; // masked off in the blend pass / absent with MSAA




void main() {
    Frame fr = FrameRef(pc.frame).f;
    Material m = MaterialRef(pc.materials).m[pc.material];
    vec4 c = unpackUnorm4x8(m.color);
    if ((m.flags & MAT_TEXTURED) != 0u) c *= texture(gTextures[nonuniformEXT(m.texture)], vUv, fr.misc.w);

    // openBVE lighting: colour × min(ambient + diffuse·max(N·L, 0), 1) + emissive
    vec3 n = normalize(vNormal);
    if (!gl_FrontFacing) n = -n;
    float lambert = max(dot(n, fr.lightDir.xyz), 0.0);
    vec3 light = min(fr.ambient.rgb + fr.diffuse.rgb * lambert, vec3(1.0)) * vBrightness;
    vec3 rgb = c.rgb * light + unpackUnorm4x8(m.emissive).rgb;

    float dist = length(vView);
    if ((m.flags & MAT_ADDITIVE) != 0u) {
        rgb = c.rgb * vBrightness + unpackUnorm4x8(m.emissive).rgb;
        float h = float(m.flags >> 16);
        if (h > 0.0) c.a *= dist * dist / (dist * dist + h * h); // openBVE glow (division exponent 2)
    }
    if (fr.fog.z > 0.0) {
        float f = clamp((fr.fog.y - dist) / max(fr.fog.y - fr.fog.x, 1e-3), 0.0, 1.0);
        rgb = mix(fr.fogColor.rgb, rgb, f);
    }
    // openBVE "smooth" transparency: translucent materials are drawn twice — the fully opaque
    // texels here with depth writes, the rest in the sorted blend pass without.
    const float kOpaque = 0.996;
    bool twoPass = (m.flags & MAT_BLEND) != 0u && (m.flags & MAT_ADDITIVE) == 0u;
    if (BLEND_PASS == 0) {
        if (twoPass) {
            if (c.a < kOpaque) discard;
            c.a = 1.0;
        } else if ((m.flags & MAT_ALPHATEST) != 0u) {
            if (fr.fog.w > 0.5) {
                // alpha to coverage: sharpen so edges stay crisp at distance
                c.a = clamp((c.a - 0.5) / max(fwidth(c.a), 1e-4) + 0.5, 0.0, 1.0);
                if (c.a <= 0.0) discard;
            } else if (c.a < 0.5) {
                discard;
            }
        } else {
            c.a = 1.0;
        }
    } else {
        if (twoPass && c.a >= kOpaque) discard; // already drawn opaque
        if (c.a <= 0.002) discard;
    }
    outColor = vec4(rgb, c.a);
    outMotion = motionUv(vCur, vPrev);
}
