#ifndef OTS_SKY_GLSL
#define OTS_SKY_GLSL
// Physically based sky; mirrors src/sky/Atmosphere.cpp (same constants and model).
// Single scattering, Rayleigh + Mie + ozone, spherical Earth: the Earth's shadow produces the
// twilight sequence by itself.

const float kPiSky = 3.14159265358979;
const float kEarthR = 6360e3, kTopR = 6460e3, kViewerH = 200.0;
const vec3 kRayleigh = vec3(5.802e-6, 13.558e-6, 33.1e-6);
const float kRayleighH = 8000.0;
const float kMieScatter = 3.996e-6, kMieExt = 4.40e-6, kMieH = 1200.0, kMieG = 0.8;
const vec3 kOzone = vec3(0.650e-6, 1.881e-6, 0.085e-6);
const float kSunE = 20.0;
const float kMoonE = kSunE * 2.5e-6;
const vec3 kAirglow = vec3(0.8e-6, 1.0e-6, 1.6e-6);
const float kMsRaise = 0.08, kMsWeight = 0.18, kMsOzonePath = 3.5e5;

vec3 skyExtinction(float h) {
    float o = max(0.0, 1.0 - abs(h - 25000.0) / 15000.0);
    return kRayleigh * exp(-h / kRayleighH) + vec3(kMieExt * exp(-h / kMieH)) + kOzone * o;
}

// far intersection with a sphere centred at the Earth's centre (−1: none)
float skySphereFar(vec3 o, vec3 d, float r) {
    float b = dot(o, d), c = dot(o, o) - r * r, disc = b * b - c;
    return disc < 0.0 ? -1.0 : -b + sqrt(disc);
}
float skyGroundHit(vec3 o, vec3 d) {
    float b = dot(o, d), c = dot(o, o) - kEarthR * kEarthR, disc = b * b - c;
    if (disc < 0.0) return -1.0;
    float t = -b - sqrt(disc);
    return t > 0.0 ? t : -1.0;
}

vec3 skyDepthToTop(vec3 p, vec3 d) {
    if (skyGroundHit(p, d) > 0.0) return vec3(1e9);
    float len = skySphereFar(p, d, kTopR);
    const int N = 6;
    float ds = len / float(N);
    vec3 tau = vec3(0.0);
    for (int i = 0; i < N; ++i) tau += skyExtinction(length(p + d * ((float(i) + 0.5) * ds)) - kEarthR) * ds;
    return tau;
}

float skyRayleighPhase(float mu) { return 3.0 / (16.0 * kPiSky) * (1.0 + mu * mu); }
float skyMiePhase(float mu) {
    float g = kMieG, g2 = g * g;
    return 3.0 / (8.0 * kPiSky) * (1.0 - g2) * (1.0 + mu * mu) / ((2.0 + g2) * pow(1.0 + g2 - 2.0 * g * mu, 1.5));
}

// in-scattered radiance along `dir`; `viewT` returns the transmittance to the top (for stars)
vec3 skyRadiance(vec3 dir, vec3 sun, vec3 moon, float moonIll, out vec3 viewT) {
    vec3 o = vec3(0.0, kEarthR + kViewerH, 0.0);
    float len = skySphereFar(o, dir, kTopR);
    float g = skyGroundHit(o, dir);
    if (g > 0.0) len = g;
    const int N = 16;
    float ds = len / float(N);
    float muS = dot(dir, sun), muM = dot(dir, moon);
    float prS = skyRayleighPhase(muS), pmS = skyMiePhase(muS);
    float prM = skyRayleighPhase(muM), pmM = skyMiePhase(muM);
    float moonE = kMoonE * moonIll;
    float msTwilight = clamp(0.5 - sun.y * 8.0, 0.0, 1.0);
    vec3 tau = vec3(0.0), sum = vec3(0.0);
    for (int i = 0; i < N; ++i) {
        vec3 p = o + dir * ((float(i) + 0.5) * ds);
        float h = length(p) - kEarthR;
        float r = exp(-h / kRayleighH), m = exp(-h / kMieH);
        vec3 ext = skyExtinction(h);
        tau += ext * (0.5 * ds);
        vec3 tS = exp(-(tau + skyDepthToTop(p, sun)));
        sum += tS * kSunE * (kRayleigh * r * prS + vec3(kMieScatter * m * pmS)) * ds;
        // multiple scattering approximation (see Atmosphere.cpp)
        vec3 tMs = exp(-(tau + skyDepthToTop(p, normalize(sun + vec3(0.0, kMsRaise, 0.0))) + kOzone * (kMsOzonePath * msTwilight)));
        sum += tMs * (kSunE * kMsWeight / (4.0 * kPiSky)) * (kRayleigh * r) * ds;
        if (moonE > 0.0) {
            vec3 tM = exp(-(tau + skyDepthToTop(p, moon)));
            sum += tM * moonE * (kRayleigh * r * prM + vec3(kMieScatter * m * pmM)) * ds;
        }
        tau += ext * (0.5 * ds);
    }
    viewT = g > 0.0 ? vec3(0.0) : exp(-tau);
    // twilight hue correction (see Atmosphere.cpp)
    float twilight = clamp(0.3 - sun.y * 12.0, 0.0, 1.0);
    float k = twilight * smoothstep(0.03, 0.6, dir.y);
    return (sum + kAirglow) * vec3(1.0 - 0.55 * k, 1.0 + 0.10 * k, 1.0 + 0.05 * k);
}

#endif
