#pragma once
// Sun / moon positions and the sky's orientation for a date, a local time and a place.
// Low-precision formulas (about 0.01° for the sun, about 1° for the moon): plenty for a sky.
//
// World axes: x east, y up, z north (openBVE's left-handed frame with the track's initial
// direction taken as north); `northAzimuth` rotates that if the route faces elsewhere.
#include "phx/core/Types.hpp"

#include <glm/glm.hpp>

namespace ots {

struct SkyPlace {
    double latitude = 36.35;   // degrees north (default: central Korea)
    double longitude = 127.38; // degrees east
    double utcOffset = 9.0;    // hours (KST)
    double northAzimuth = 0.0; // degrees: world +z is this far clockwise from north
};

struct CivilDate {
    int year = 2026, month = 6, day = 21;
};

struct SkyPositions {
    glm::dvec3 sun{0.0, 1.0, 0.0};  // world direction towards the sun
    glm::dvec3 moon{0.0, -1.0, 0.0};
    double moonIllumination = 0.0;  // lit fraction 0..1
    double sunAltitude = 0.0;       // degrees
    double moonAltitude = 0.0;
    // world direction → equatorial (J2000 RA/Dec unit vector); rows are orthonormal
    glm::dmat3 equatorialFromWorld{1.0};
};

// Julian date of a local civil time (seconds since local midnight may exceed a day).
double julianDate(const CivilDate& date, double localSeconds, double utcOffsetHours);
SkyPositions skyPositions(const CivilDate& date, double localSeconds, const SkyPlace& place);
// Today's date from the system clock (local time).
CivilDate today();
// "YYYY-MM-DD"; nullopt-like: returns false on a malformed string.
bool parseDate(const char* text, CivilDate& out);

} // namespace ots
