#version 460
#include "common.glsl"
#include "sky.glsl"
// Physical sky + NASA star map (J2000 equatorial) + ESA Gaia Milky Way (galactic), sun and
// moon discs; everything scene-referred, then one exposure for the eye's adaptation.
layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outMotion;

// J2000 equatorial → galactic (IAU)
const mat3 kGalFromEq = mat3(-0.0548755604,  0.4941094279, -0.8676661490,   // columns
                             -0.8734370902, -0.4448296300, -0.1980763734,
                             -0.4838350155,  0.7469822445,  0.4559837762);

// Equirectangular lookup: centre at longitude 0, longitude increasing to the left (sky maps).
// Gradients come from the continuous direction so the wrap seam does not pick the last mip.
vec3 sampleSkyMap(uint slot, vec3 v) {
    float lon = atan(v.y, v.x);
    float lat = asin(clamp(v.z, -1.0, 1.0));
    vec2 uv = vec2(0.5 - lon / (2.0 * kPiSky), 0.5 - lat / kPiSky);
    vec2 dx = dFdx(uv), dy = dFdy(uv);
    dx.x -= round(dx.x);
    dy.x -= round(dy.x);
    return textureGrad(gTextures[nonuniformEXT(slot)], uv, dx, dy).rgb;
}

void main() {
    Frame fr = FrameRef(pc.frame).f;
    vec4 far = fr.invViewProj * vec4(vNdc, 0.5, 1.0);
    vec3 dir = normalize(far.xyz / far.w);
    vec3 sun = fr.sunDir.xyz, moon = fr.moonDir.xyz;
    float moonIll = fr.sunDir.w;

    // Below the horizon (high cameras): distant hazy ground, the horizon sky darkened, instead of
    // black.
    float below = clamp(-dir.y * 6.0, 0.0, 1.0);
    vec3 skyDir = dir.y < 0.0 ? normalize(vec3(dir.x, 0.0, dir.z)) : dir;
    vec3 viewT;
    vec3 L = skyRadiance(skyDir, sun, moon, moonIll, viewT);
    if (dir.y < 0.0) {
        L *= mix(1.0, 0.25, below);
        viewT = vec3(0.0);
    }

    // sun disc (0.27° radius) with limb darkening
    float cosSun = dot(dir, sun);
    const float kSunCos = 0.99998890; // cos(0.27°)
    if (cosSun > kSunCos) {
        float r = sqrt(max(1.0 - cosSun, 0.0) / (1.0 - kSunCos));
        L += viewT * kSunE * 2.0e4 * (1.0 - 0.6 * (1.0 - sqrt(max(1.0 - r * r, 0.0))));
    }
    // moon disc (0.26°), lit side towards the sun
    float cosMoon = dot(dir, moon);
    const float kMoonCos = 0.99998970;
    if (cosMoon > kMoonCos && viewT.g > 0.0) {
        vec3 side = normalize(cross(moon, vec3(0.0, 1.0, 0.0)) + 1e-5);
        vec3 up = normalize(cross(side, moon));
        vec2 q = vec2(dot(dir - moon, side), dot(dir - moon, up)) / sqrt(2.0 * (1.0 - kMoonCos));
        vec3 n = vec3(q, sqrt(max(1.0 - dot(q, q), 0.0)));        // disc normal, z towards us
        vec3 s = normalize(vec3(dot(sun, side), dot(sun, up), -dot(sun, moon))); // sun in disc frame
        float lit = smoothstep(-0.05, 0.08, dot(n, s));
        L += viewT * kSunE * 2.5e-6 * 4.6e4 * (0.03 + lit) * vec3(0.95, 0.93, 0.88);
    }

    // stars and the Milky Way, dimmed by the air in front of them
    if (fr.sky.z > 0.0 || fr.sky.w > 0.0) {
        vec3 eq = mat3(fr.equatorialFromWorld) * dir;
        vec3 stars = vec3(0.0);
        uint starSlot = uint(fr.skyMaps.x), gaiaSlot = uint(fr.skyMaps.y);
        if (starSlot != 0u && fr.sky.z > 0.0) {
            vec3 c = sampleSkyMap(starSlot, eq);
            vec3 c2 = c * c;
            stars += c2 * c2 * fr.skyMaps.z * fr.sky.z * 8e-5;
        }
        if (gaiaSlot != 0u && fr.sky.w > 0.0) {
            vec3 c = sampleSkyMap(gaiaSlot, kGalFromEq * eq);
            stars += c * c * fr.sky.w * 1.2e-5; // display-encoded PNG: roughly linearise
        }
        L += stars * viewT;
    }

    vec3 c = vec3(1.0) - exp(-L * fr.sky.y);
    // a little dither so the dark gradients of twilight do not band
    float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    c += (n - 0.5) / 255.0;
    outColor = vec4(max(c, 0.0), 1.0);
    vec4 cur = fr.viewProjNoJitter * vec4(dir, 0.0);
    vec4 prev = fr.prevViewProj * vec4(dir, 0.0);
    outMotion = motionUv(cur, prev);
}
