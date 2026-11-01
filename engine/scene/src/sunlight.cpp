// SPDX-License-Identifier: Apache-2.0
#include "midgard/sunlight.h"

#include <algorithm>
#include <cmath>

namespace midgard {
namespace {

constexpr double kPi = 3.141592653589793;
constexpr double kTau = 2.0 * kPi;

double wrap01(double x) {
    return x - std::floor(x);
}

glm::dvec3 flat(const glm::dvec3& v, const glm::dvec3& up) {
    return v - up * glm::dot(v, up);
}

glm::dvec3 any_perpendicular(const glm::dvec3& up) {
    return glm::normalize(
        glm::cross(up, std::abs(up.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 0, 1)));
}

glm::dvec3 path(const glm::dvec3& up, glm::dvec3 noon, double phase) {
    if (glm::dot(noon, up) < 0.01) noon = up;  // placed below the horizon: overhead
    glm::dvec3 rising = glm::cross(up, noon);
    rising = glm::dot(rising, rising) >= 1e-16 ? glm::normalize(rising) : any_perpendicular(up);
    const double a = kTau * phase;
    return rising * std::cos(a) + noon * std::sin(a);
}

glm::dvec3 up_at(const std::optional<Body>& body, const glm::dvec3& p) {
    if (!body) return {0.0, 1.0, 0.0};
    // At a sphere's centre or on a cylinder's axis "up" is undefined: +Y, as the shape layer
    // has it. (The start, the scene's origin, is there when a body is centred on it.)
    const glm::dvec3 q = p - body->origin;
    switch (body->shape.kind()) {
        case Shape::Kind::BoundedArea:
        case Shape::Kind::FlatCylinder:
            return {0.0, 1.0, 0.0};
        case Shape::Kind::CurvedCylinder:
            if (q.x * q.x + q.y * q.y < 1e-18) return {0.0, 1.0, 0.0};
            return glm::normalize(glm::dvec3(-q.x, -q.y, 0.0));  // towards the axis (along Z)
        case Shape::Kind::SphereOutside:
            if (glm::dot(q, q) < 1e-18) return {0.0, 1.0, 0.0};
            return glm::normalize(q);
        default:
            if (glm::dot(q, q) < 1e-18) return {0.0, 1.0, 0.0};
            return -glm::normalize(q);
    }
}

}  // namespace

const char* sun_mode_name(SunMode mode) {
    return mode == SunMode::Latitude ? "latitude" : "uniform";
}

std::optional<SunMode> sun_mode_from_name(const std::string& name) {
    if (name == "uniform") return SunMode::Uniform;
    if (name == "latitude") return SunMode::Latitude;
    return std::nullopt;
}

double phase_of_time(double time, double daylight, double noon) {
    const double d = std::clamp(daylight, 0.01, 0.99);
    const double t = wrap01(time - noon + d / 2.0);
    if (t < d) return 0.5 * t / d;
    return 0.5 + 0.5 * (t - d) / (1.0 - d);
}

double time_of_phase(double phase, double daylight, double noon) {
    const double d = std::clamp(daylight, 0.01, 0.99);
    phase = wrap01(phase);
    const double t = phase < 0.5 ? phase / 0.5 * d : d + (phase - 0.5) / 0.5 * (1.0 - d);
    return wrap01(t + noon - d / 2.0);
}

LocalSun local_sun(const std::optional<Body>& body, const glm::dvec3& placed, const SunClock& clock,
                   const glm::dvec3& p) {
    LocalSun out;
    out.season = clock.season;
    out.autumn = clock.autumn;
    const glm::dvec3 up0 = up_at(body, glm::dvec3(0.0));
    const glm::dvec3 up_p = up_at(body, p);
    const Shape::Kind kind = body ? body->shape.kind() : Shape::Kind::BoundedArea;
    if (kind == Shape::Kind::BoundedArea || kind == Shape::Kind::FlatCylinder) {
        out.phase = phase_of_time(clock.time, clock.daylight, clock.noon);
        out.to_sun = path(up0, placed, out.phase);
        out.height = glm::dot(out.to_sun, up_p);
        return out;
    }

    glm::dvec3 axis, out0;
    double elev0 = kPi / 2;
    if (kind == Shape::Kind::CurvedCylinder) {
        axis = {0.0, 0.0, 1.0};
        out0 = -up0;
    } else {
        // The start's frame: north points away from the sun at noon.
        const glm::dvec3 h = flat(placed, up0);
        const glm::dvec3 north0 =
            glm::dot(h, h) > 1e-16 ? -glm::normalize(h) : any_perpendicular(up0);
        elev0 = std::asin(std::clamp(glm::dot(placed, up0), -1.0, 1.0));
        elev0 = std::clamp(elev0, kPi / 180.0, kPi / 2);
        if (kind == Shape::Kind::SphereOutside) {
            const double lat0 = kPi / 2 - elev0;  // how high the sun stands at noon
            axis = north0 * std::cos(lat0) + up0 * std::sin(lat0);
            out0 = up0;
        } else {  // sphere inside: the shades go round an axis through the start's north
            axis = north0;
            out0 = -up0;
        }
    }
    const glm::dvec3 e_y = glm::normalize(flat(out0, axis));  // outwards under the start
    const glm::dvec3 e_x = glm::cross(axis, e_y);             // east: the way the day moves
    glm::dvec3 q = glm::normalize(p - body->origin);
    if (kind == Shape::Kind::CurvedCylinder) q = glm::normalize(glm::dvec3(q.x, q.y, 0.0));
    const double longitude = std::atan2(glm::dot(q, e_x), glm::dot(q, e_y));

    if (kind == Shape::Kind::SphereOutside && clock.mode == SunMode::Latitude) {
        const double tilt = clock.tilt * kPi / 180.0;
        const double declination = tilt * std::sin(kTau * clock.year);
        const double hour = kTau * (clock.time - clock.noon);
        out.to_sun = (e_y * std::cos(hour) + e_x * -std::sin(hour)) * std::cos(declination) +
                     axis * std::sin(declination);
        const double latitude = std::asin(std::clamp(glm::dot(q, axis), -1.0, 1.0));
        if (clock.tilt > 0.0) {
            const double k = std::clamp(latitude / (kPi / 4), -1.0, 1.0);
            out.season = std::sin(kTau * clock.year) * k;
            out.autumn = -std::cos(kTau * clock.year) * k;  // after midsummer, towards winter
        }
        const double c = -std::tan(latitude) * std::tan(declination);
        const double h0 = c <= -1.0 ? kPi : (c >= 1.0 ? 0.0 : std::acos(c));
        double local_hour = std::fmod(hour + longitude + kPi, kTau);
        if (local_hour < 0.0) local_hour += kTau;
        local_hour -= kPi;
        if (h0 > 0.0 && std::abs(local_hour) <= h0) {
            out.phase = 0.25 + 0.25 * local_hour / h0;
        } else {
            const double night = kTau - 2.0 * h0;
            double since = std::fmod(local_hour - h0, kTau);
            if (since < 0.0) since += kTau;
            out.phase = night > 0.0 ? 0.5 + 0.5 * since / night : 0.5;
        }
        out.height = glm::dot(out.to_sun, up_p);
        return out;
    }

    out.phase = phase_of_time(clock.time + longitude / kTau, clock.daylight, clock.noon);
    if (kind == Shape::Kind::SphereOutside) {  // Uniform: the start's noon height, everywhere
        glm::dvec3 north = flat(axis, up_p);
        north = glm::dot(north, north) > 1e-16 ? glm::normalize(north) : any_perpendicular(up_p);
        const glm::dvec3 noon_dir = up_p * std::sin(elev0) - north * std::cos(elev0);
        out.to_sun = path(up_p, noon_dir, out.phase);
    } else {
        // Inside: the light comes straight down from the light rod on the axis (a cylinder) or
        // the light sphere at the centre (a hollow world). The shades going round give each
        // place its day: how far the light gets through is the height of a stylised sun, low
        // from the east at dawn, overhead at noon, low from the west at dusk.
        glm::dvec3 east = glm::cross(axis, q);
        east = glm::dot(east, east) > 1e-16 ? glm::normalize(east) : any_perpendicular(up_p);
        const double a = kTau * out.phase;
        out.height = glm::dot(east * std::cos(a) + up_p * std::sin(a), up_p);
        out.to_sun = up_p;
        return out;
    }
    out.height = glm::dot(out.to_sun, up_p);
    return out;
}

}  // namespace midgard
