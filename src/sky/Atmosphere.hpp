#pragma once
// Physically based sky: single scattering through a Rayleigh + Mie + ozone atmosphere over a
// spherical Earth (the Earth's shadow gives the twilight sequence: golden sunset, the pink
// Belt of Venus over the dark blue shadow, civil → nautical → astronomical twilight, night).
// The same model runs in shaders/sky.glsl; this CPU copy derives the scene lighting, fog
// colour and exposure from it so objects and sky agree.
#include "phx/core/Types.hpp"
#include "sky/Astronomy.hpp"

#include <glm/glm.hpp>

namespace ots {

namespace atmosphere {
inline constexpr double kEarthRadius = 6360e3, kTopRadius = 6460e3, kViewerHeight = 200.0;
inline constexpr glm::dvec3 kRayleigh{5.802e-6, 13.558e-6, 33.1e-6};
inline constexpr double kRayleighHeight = 8000.0;
inline constexpr double kMieScatter = 3.996e-6, kMieExtinction = 4.40e-6, kMieHeight = 1200.0, kMieG = 0.8;
inline constexpr glm::dvec3 kOzone{0.650e-6, 1.881e-6, 0.085e-6}; // absorption at 25 km
inline constexpr double kSunIlluminance = 20.0;                   // arbitrary radiance units
inline constexpr double kMoonIlluminance = kSunIlluminance * 2.5e-6; // full moon ≈ 1 / 400 000
inline constexpr glm::dvec3 kAirglow{0.8e-6, 1.0e-6, 1.6e-6};       // night sky floor
inline constexpr double kMsRaise = 0.08, kMsWeight = 0.18;           // multiple-scattering approximation
inline constexpr double kMsOzonePath = 3.5e5;                         // m of ozone-peak air at a grazing sun
} // namespace atmosphere

// Radiance (scene-referred) arriving from `dir` with the sun / moon at the given directions.
glm::dvec3 skyRadiance(const glm::dvec3& dir, const glm::dvec3& sun, const glm::dvec3& moon, double moonIllumination);
// Transmittance from the viewer to the top of the atmosphere along `dir` (0 below the horizon).
glm::dvec3 skyTransmittance(const glm::dvec3& dir);

struct SkyLighting {
    glm::vec3 lightDirection{0, 1, 0}; // towards the main light (sun, else moon)
    glm::vec3 ambient{0.6f}, diffuse{0.6f};
    glm::vec3 fogColor{0.7f}; // horizon away from the sun
    glm::vec3 fogSun{0.7f};   // horizon towards the sun
    float exposure = 1.0f;     // sky radiance → display
    float shadowStrength = 1.0f;
    float starVisibility = 0.0f;
};

// Scene lighting for openBVE's display-referred model: the route's own ambient / diffuse are the
// daytime values, scaled and tinted by the light that actually arrives.
SkyLighting skyLighting(const SkyPositions& pos, const glm::vec3& routeAmbient, const glm::vec3& routeDiffuse);

} // namespace ots
