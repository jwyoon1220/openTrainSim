#include "sky/Astronomy.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace ots {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

double wrapDegrees(double a) {
    a = std::fmod(a, 360.0);
    return a < 0.0 ? a + 360.0 : a;
}

glm::dvec3 equatorialVector(double ra, double dec) {
    return {std::cos(dec) * std::cos(ra), std::cos(dec) * std::sin(ra), std::sin(dec)};
}

// ecliptic longitude / latitude (radians) → equatorial unit vector
glm::dvec3 eclipticToEquatorial(double lambda, double beta, double epsilon) {
    const glm::dvec3 e(std::cos(beta) * std::cos(lambda), std::cos(beta) * std::sin(lambda), std::sin(beta));
    return {e.x, e.y * std::cos(epsilon) - e.z * std::sin(epsilon), e.y * std::sin(epsilon) + e.z * std::cos(epsilon)};
}

} // namespace

double julianDate(const CivilDate& date, double localSeconds, double utcOffsetHours) {
    // Fliegel & Van Flandern: Julian day number at noon, then the fraction from midnight UT.
    const int a = (14 - date.month) / 12;
    const int y = date.year + 4800 - a;
    const int m = date.month + 12 * a - 3;
    const long jdn = date.day + (153 * m + 2) / 5 + 365L * y + y / 4 - y / 100 + y / 400 - 32045;
    const double ut = localSeconds / 3600.0 - utcOffsetHours;
    return double(jdn) - 0.5 + ut / 24.0;
}

SkyPositions skyPositions(const CivilDate& date, double localSeconds, const SkyPlace& place) {
    const double jd = julianDate(date, localSeconds, place.utcOffset);
    const double n = jd - 2451545.0;
    const double epsilon = (23.439 - 0.0000004 * n) * kDeg;

    // sun (Astronomical Almanac low-precision formula)
    const double L = wrapDegrees(280.460 + 0.9856474 * n);
    const double g = wrapDegrees(357.528 + 0.9856003 * n) * kDeg;
    const double lambdaSun = (L + 1.915 * std::sin(g) + 0.020 * std::sin(2.0 * g)) * kDeg;
    const glm::dvec3 sunEq = eclipticToEquatorial(lambdaSun, 0.0, epsilon);

    // moon (main periodic terms)
    const double L0 = wrapDegrees(218.316 + 13.176396 * n);
    const double M = wrapDegrees(134.963 + 13.064993 * n) * kDeg;
    const double F = wrapDegrees(93.272 + 13.229350 * n) * kDeg;
    const double D = wrapDegrees(297.850 + 12.190749 * n) * kDeg;
    const double lambdaMoon = (L0 + 6.289 * std::sin(M) + 1.274 * std::sin(2.0 * D - M) + 0.658 * std::sin(2.0 * D) - 0.186 * std::sin(g)) * kDeg;
    const double betaMoon = 5.128 * std::sin(F) * kDeg;
    const glm::dvec3 moonEq = eclipticToEquatorial(lambdaMoon, betaMoon, epsilon);

    // local sidereal time
    const double gmstHours = 18.697374558 + 24.06570982441908 * n;
    const double theta = wrapDegrees(gmstHours * 15.0 + place.longitude) * kDeg;
    const double phi = place.latitude * kDeg;

    // equatorial axes in local east / north / up at sidereal angle theta:
    // hour angle H = theta − RA; H = 0 is the meridian (south), H = 6h the west point.
    const glm::dvec3 pole(0.0, std::cos(phi), std::sin(phi));      // e, n, u
    const glm::dvec3 meridian(0.0, -std::sin(phi), std::cos(phi)); // equator at H = 0
    const glm::dvec3 west(-1.0, 0.0, 0.0);
    const glm::dvec3 ax = std::cos(theta) * meridian + std::sin(theta) * west;
    const glm::dvec3 ay = std::sin(theta) * meridian - std::cos(theta) * west;
    // ENU → world (x east, y up, z north), then the route's own rotation about the vertical
    const double az = place.northAzimuth * kDeg;
    auto toWorld = [&](const glm::dvec3& enu) {
        const glm::dvec3 w(enu.x, enu.z, enu.y);
        return glm::dvec3(w.x * std::cos(az) - w.z * std::sin(az), w.y, w.x * std::sin(az) + w.z * std::cos(az));
    };
    const glm::dmat3 worldFromEq(toWorld(ax), toWorld(ay), toWorld(pole)); // columns
    SkyPositions out;
    out.equatorialFromWorld = glm::transpose(worldFromEq);
    out.sun = glm::normalize(worldFromEq * sunEq);
    out.moon = glm::normalize(worldFromEq * moonEq);
    out.sunAltitude = std::asin(std::clamp(out.sun.y, -1.0, 1.0)) / kDeg;
    out.moonAltitude = std::asin(std::clamp(out.moon.y, -1.0, 1.0)) / kDeg;
    // phase from the sun–moon elongation
    const double elongation = std::acos(std::clamp(glm::dot(sunEq, moonEq), -1.0, 1.0));
    out.moonIllumination = 0.5 * (1.0 - std::cos(elongation));
    return out;
}

CivilDate today() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday};
}

bool parseDate(const char* text, CivilDate& out) {
    CivilDate d;
    if (std::sscanf(text, "%d-%d-%d", &d.year, &d.month, &d.day) != 3) return false;
    if (d.month < 1 || d.month > 12 || d.day < 1 || d.day > 31) return false;
    out = d;
    return true;
}

} // namespace ots
