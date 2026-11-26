#include "sky/Atmosphere.hpp"

#include <algorithm>
#include <cmath>

namespace ots {

using namespace atmosphere;

namespace {

constexpr double kPi = 3.14159265358979323846;

// distance along `dir` from `origin` to the sphere of `radius` (far hit), or -1
double raySphereFar(const glm::dvec3& origin, const glm::dvec3& dir, double radius) {
    const double b = glm::dot(origin, dir);
    const double c = glm::dot(origin, origin) - radius * radius;
    const double disc = b * b - c;
    if (disc < 0.0) return -1.0;
    return -b + std::sqrt(disc);
}

// true if the ray hits the ground ahead
bool hitsGround(const glm::dvec3& origin, const glm::dvec3& dir) {
    const double b = glm::dot(origin, dir);
    const double c = glm::dot(origin, origin) - kEarthRadius * kEarthRadius;
    const double disc = b * b - c;
    return disc >= 0.0 && -b - std::sqrt(disc) > 0.0;
}

glm::dvec3 extinctionAt(double h) {
    const double r = std::exp(-h / kRayleighHeight);
    const double m = std::exp(-h / kMieHeight);
    const double o = std::max(0.0, 1.0 - std::abs(h - 25000.0) / 15000.0);
    return kRayleigh * r + glm::dvec3(kMieExtinction * m) + kOzone * o;
}

// optical depth from `p` towards `dir` up to the top of the atmosphere
glm::dvec3 opticalDepthToTop(const glm::dvec3& p, const glm::dvec3& dir, int steps) {
    if (hitsGround(p, dir)) return glm::dvec3(1e9);
    const double len = raySphereFar(p, dir, kTopRadius);
    const double ds = len / steps;
    glm::dvec3 tau(0.0);
    for (int i = 0; i < steps; ++i) {
        const glm::dvec3 q = p + dir * ((i + 0.5) * ds);
        tau += extinctionAt(glm::length(q) - kEarthRadius) * ds;
    }
    return tau;
}

double rayleighPhase(double mu) { return 3.0 / (16.0 * kPi) * (1.0 + mu * mu); }
double miePhase(double mu) {
    const double g = kMieG, g2 = g * g;
    return 3.0 / (8.0 * kPi) * (1.0 - g2) * (1.0 + mu * mu) / ((2.0 + g2) * std::pow(1.0 + g2 - 2.0 * g * mu, 1.5));
}

glm::dvec3 expv(const glm::dvec3& v) { return {std::exp(-v.x), std::exp(-v.y), std::exp(-v.z)}; }

double luminance(const glm::dvec3& c) { return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z; }

// Partial adaptation: a 10⁶ drop in sky brightness becomes a ~10² drop on screen, so dusk
// darkens progressively and night stays dark (stars stand out) instead of turning grey.
// `adapt` weights the zenith over the horizon so the afterglow low in the west does not keep
// the rest of the sky black.
double exposureFor(const glm::dvec3& zenith, const glm::dvec3& horizon) {
    const double adapt = 0.7 * luminance(zenith) + 0.3 * luminance(horizon);
    constexpr double kDay = 0.3; // adaptation luminance of a clear day sky (these units)
    return std::clamp(3.2 * std::pow(kDay / std::max(adapt, 1e-9), 0.74), 0.5, 3.0e4);
}

} // namespace

glm::dvec3 skyTransmittance(const glm::dvec3& dir) {
    const glm::dvec3 origin(0.0, kEarthRadius + kViewerHeight, 0.0);
    return expv(opticalDepthToTop(origin, glm::normalize(dir), 24));
}

glm::dvec3 skyRadiance(const glm::dvec3& dirIn, const glm::dvec3& sun, const glm::dvec3& moon, double moonIllumination) {
    const glm::dvec3 dir = glm::normalize(dirIn);
    const glm::dvec3 origin(0.0, kEarthRadius + kViewerHeight, 0.0);
    // below the horizon: march to the ground instead (the haze between viewer and ground)
    double len = raySphereFar(origin, dir, kTopRadius);
    {
        const double b = glm::dot(origin, dir);
        const double c = glm::dot(origin, origin) - kEarthRadius * kEarthRadius;
        const double disc = b * b - c;
        if (disc >= 0.0) {
            const double t = -b - std::sqrt(disc);
            if (t > 0.0) len = t;
        }
    }
    constexpr int kSteps = 24;
    const double ds = len / kSteps;
    const double muSun = glm::dot(dir, sun), muMoon = glm::dot(dir, moon);
    const double prS = rayleighPhase(muSun), pmS = miePhase(muSun);
    const double prM = rayleighPhase(muMoon), pmM = miePhase(muMoon);
    const double moonE = kMoonIlluminance * moonIllumination;
    const double msTwilight = std::clamp(0.5 - sun.y * 8.0, 0.0, 1.0);
    glm::dvec3 tauView(0.0), sum(0.0);
    for (int i = 0; i < kSteps; ++i) {
        const glm::dvec3 p = origin + dir * ((i + 0.5) * ds);
        const double h = glm::length(p) - kEarthRadius;
        const double r = std::exp(-h / kRayleighHeight), m = std::exp(-h / kMieHeight);
        tauView += extinctionAt(h) * (0.5 * ds);
        const glm::dvec3 tSun = expv(tauView + opticalDepthToTop(p, sun, 8));
        const glm::dvec3 tMoon = moonE > 0.0 ? expv(tauView + opticalDepthToTop(p, moon, 8)) : glm::dvec3(0.0);
        // Multiple scattering, approximated: light scattered by the still-lit air higher up
        // reaches air already in the Earth's shadow (keeps the twilight zenith deep blue
        // instead of black). Isotropic, through a "raised" sun.
        // Once the sun is down that light has crossed the ozone layer at a grazing angle, which
        // filters out red and green (Chappuis band): the blue of twilight.
        const glm::dvec3 raised = glm::normalize(sun + glm::dvec3(0.0, kMsRaise, 0.0));
        const glm::dvec3 tMs = expv(tauView + opticalDepthToTop(p, raised, 6) + kOzone * (kMsOzonePath * msTwilight));
        sum += tMs * (kSunIlluminance * kMsWeight / (4.0 * kPi)) * (kRayleigh * r) * ds;
        sum += (tSun * kSunIlluminance * 1.0) * (kRayleigh * r * prS + glm::dvec3(kMieScatter * m * pmS)) * ds;
        sum += (tMoon * moonE) * (kRayleigh * r * prM + glm::dvec3(kMieScatter * m * pmM)) * ds;
        tauView += extinctionAt(h) * (0.5 * ds);
    }
    // Three-channel rendering overstates red in the twilight sky away from the horizon (the
    // spectral ozone absorption it stands for is broad): pull it towards blue as in photographs.
    const double twilight = std::clamp(0.3 - sun.y * 12.0, 0.0, 1.0);
    const double up = std::clamp((dir.y - 0.03) / 0.57, 0.0, 1.0);
    const double k = twilight * up * up * (3.0 - 2.0 * up);
    return (sum + kAirglow) * glm::dvec3(1.0 - 0.55 * k, 1.0 + 0.10 * k, 1.0 + 0.05 * k);
}

SkyLighting skyLighting(const SkyPositions& pos, const glm::vec3& routeAmbient, const glm::vec3& routeDiffuse) {
    SkyLighting out;
    const glm::dvec3 sun = pos.sun, moon = pos.moon;
    const double moonIll = pos.moonIllumination;

    // sky brightness the eye adapts to: zenith and the horizon all around
    const glm::dvec3 zenith = skyRadiance({0, 1, 0}, sun, moon, moonIll);
    glm::dvec3 horizon(0.0), irradiance(0.0);
    for (int k = 0; k < 8; ++k) {
        const double a = k * kPi / 4.0;
        const glm::dvec3 h = skyRadiance({std::cos(a), 0.035, std::sin(a)}, sun, moon, moonIll);
        const glm::dvec3 u = skyRadiance({std::cos(a) * 0.7, 0.7, std::sin(a) * 0.7}, sun, moon, moonIll);
        const glm::dvec3 l = skyRadiance({std::cos(a) * 0.94, 0.34, std::sin(a) * 0.94}, sun, moon, moonIll);
        horizon += h / 8.0;
        irradiance += (l * 0.4 + u * 0.6) / 8.0;
    }
    irradiance = (irradiance * 0.7 + zenith * 0.3) * kPi; // cosine-weighted sky dome, roughly
    out.exposure = float(exposureFor(zenith, horizon));

    // direct light: sun, else moon (dimmer, bluish after adaptation)
    const glm::dvec3 tSun = skyTransmittance(sun) * std::clamp((sun.y + 0.01) / 0.03, 0.0, 1.0);
    const glm::dvec3 tMoon = skyTransmittance(moon) * std::clamp((moon.y + 0.01) / 0.03, 0.0, 1.0);
    const glm::dvec3 sunE = tSun * kSunIlluminance * std::max(sun.y, 0.0);
    const glm::dvec3 moonE = tMoon * kMoonIlluminance * moonIll * std::max(moon.y, 0.0) * glm::dvec3(0.85, 0.92, 1.0);
    const bool sunMain = luminance(sunE) >= luminance(moonE);
    out.lightDirection = glm::vec3(sunMain ? sun : moon);
    // normal-incidence light for openBVE's N·L model (the cosine is applied by the shader)
    const glm::dvec3 direct = sunMain ? tSun * kSunIlluminance : tMoon * kMoonIlluminance * moonIll * glm::dvec3(0.85, 0.92, 1.0);

    // Normalise by a reference day (sun 45° up) so the route's own values hold at midday.
    static const glm::dvec3 refSun = glm::normalize(glm::dvec3(0.0, 1.0, 1.0));
    static const glm::dvec3 refDirect = skyTransmittance(refSun) * kSunIlluminance;
    static const glm::dvec3 refAmbient = [] {
        glm::dvec3 irr(0.0);
        const glm::dvec3 z = skyRadiance({0, 1, 0}, refSun, {0, -1, 0}, 0.0);
        for (int k = 0; k < 8; ++k) {
            const double a = k * kPi / 4.0;
            irr += (skyRadiance({std::cos(a) * 0.94, 0.34, std::sin(a) * 0.94}, refSun, {0, -1, 0}, 0.0) * 0.4 +
                    skyRadiance({std::cos(a) * 0.7, 0.7, std::sin(a) * 0.7}, refSun, {0, -1, 0}, 0.0) * 0.6) / 8.0;
        }
        return (irr * 0.7 + z * 0.3) * kPi;
    }();
    static const double refExposure = [] {
        const glm::dvec3 z = skyRadiance({0, 1, 0}, refSun, {0, -1, 0}, 0.0);
        glm::dvec3 h(0.0);
        for (int k = 0; k < 8; ++k) h += skyRadiance({std::cos(k * kPi / 4.0), 0.035, std::sin(k * kPi / 4.0)}, refSun, {0, -1, 0}, 0.0) / 8.0;
        return exposureFor(z, h);
    }();
    const double gain = out.exposure / refExposure;
    // per channel: at the reference day the route's own colours come out unchanged
    const glm::dvec3 d = direct * gain / refDirect;
    glm::dvec3 a = irradiance * gain / refAmbient;
    // Single scattering overstates the forward (Mie) glow around the sun; multiple scattering
    // and the eye's loss of colour in dim light make sky light much more neutral. Keep half of
    // the hue.
    a = glm::mix(glm::dvec3(luminance(a)), a, 0.3);
    out.diffuse = glm::clamp(glm::vec3(d) * routeDiffuse, glm::vec3(0.0f), glm::vec3(1.5f));
    // A floor for moonless nights: starlight, airglow and the eye's own adaptation keep shapes
    // readable against the sky (scotopic: bluish, colourless).
    const glm::dvec3 nightFloor = glm::dvec3(0.10, 0.115, 0.15);
    out.ambient = glm::clamp(glm::max(glm::vec3(a) * routeAmbient, glm::vec3(nightFloor) * routeAmbient / 0.63f), glm::vec3(0.0f), glm::vec3(1.5f));
    out.shadowStrength = float(std::clamp(luminance(d) / std::max(luminance(d) + luminance(a) * 0.6, 1e-6), 0.0, 1.0));

    // fog fades distant objects into the horizon sky: one colour towards the sun's azimuth, one
    // away from it (the shader blends by view direction)
    const glm::dvec3 flat = glm::length(glm::dvec2(sun.x, sun.z)) > 1e-6 ? glm::normalize(glm::dvec3(sun.x, 0.0, sun.z)) : glm::dvec3(0, 0, 1);
    const glm::dvec3 hSun = skyRadiance(glm::dvec3(flat.x, 0.035, flat.z), sun, moon, moonIll);
    const glm::dvec3 hAway = skyRadiance(glm::dvec3(-flat.x, 0.035, -flat.z), sun, moon, moonIll);
    out.fogSun = glm::vec3(glm::dvec3(1.0) - expv(hSun * double(out.exposure)));
    out.fogColor = glm::vec3(glm::dvec3(1.0) - expv(hAway * double(out.exposure)));
    out.starVisibility = float(std::clamp(1.0 - luminance(zenith) / 2e-3, 0.0, 1.0));
    return out;
}

} // namespace ots
