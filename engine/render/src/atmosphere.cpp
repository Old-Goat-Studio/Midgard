// SPDX-License-Identifier: Apache-2.0
#include "midgard/render/atmosphere.h"

#include <algorithm>
#include <cmath>

#include "midgard/render/mesh_data.h"

namespace midgard::render {

namespace {

// An Earth-like atmosphere (Bruneton and Neyret's constants), in metres.
constexpr double kPlanet = 6360e3;
constexpr double kTop = 6420e3;
constexpr double kRayleighHeight = 7994.0;
constexpr double kMieHeight = 1200.0;
const glm::dvec3 kRayleigh(5.802e-6, 13.558e-6, 33.1e-6);  // scattering per metre at sea level
constexpr double kMie = 3.996e-6;
constexpr double kMieExtinction = 4.40e-6;
// Ozone absorbs (orange and red most): without it horizons turn green and twilight grey.
const glm::dvec3 kOzone(0.650e-6, 1.881e-6, 0.085e-6);
constexpr double kMieG = 0.76;
constexpr double kPi = 3.14159265358979323846;
constexpr double kLow = 8.0 * kPi / 180.0;  // "just above the horizon", for its colours
constexpr int kViewSteps = 24;
constexpr int kLightSteps = 12;

// Where a ray from inside a sphere around the planet's centre leaves it.
double exit_distance(const glm::dvec3& origin, const glm::dvec3& dir, double radius) {
    const double b = glm::dot(origin, dir);
    const double c = glm::dot(origin, origin) - radius * radius;
    return -b + std::sqrt(std::max(b * b - c, 0.0));
}

// Where a ray first meets the ground, or a negative number if it never does.
double ground_distance(const glm::dvec3& origin, const glm::dvec3& dir) {
    const double b = glm::dot(origin, dir);
    const double c = glm::dot(origin, origin) - kPlanet * kPlanet;
    const double d = b * b - c;
    if (b > 0.0 || d < 0.0) return -1.0;
    return -b - std::sqrt(d);
}

// The relative density of air (Rayleigh), haze (Mie) and ozone at a height: ozone lies in a
// layer between 10 and 40 km.
glm::dvec3 densities(double h) {
    return {std::exp(-h / kRayleighHeight), std::exp(-h / kMieHeight),
            std::max(0.0, 1.0 - std::abs(h - 25e3) / 15e3)};
}

// Optical depths (density times metres) from a point to the top of the atmosphere; false if the
// ground is in the way.
bool depth_to_sky(const glm::dvec3& origin, const glm::dvec3& dir, glm::dvec3& depth) {
    if (ground_distance(origin, dir) > 0.0) return false;
    const double length = exit_distance(origin, dir, kTop);
    const double step = length / kLightSteps;
    depth = glm::dvec3(0.0);
    for (int i = 0; i < kLightSteps; ++i)
        depth += densities(glm::length(origin + dir * ((i + 0.5) * step)) - kPlanet) * step;
    return true;
}

glm::dvec3 transmittance(const glm::dvec3& depth) {
    return glm::exp(
        -(kRayleigh * depth.x + glm::dvec3(kMieExtinction * depth.y) + kOzone * depth.z));
}

struct Scattered {
    glm::dvec3 rayleigh{0.0}, mie{0.0};  // light scattered towards the camera, before the phase
};

// Single scattering along a view ray from just above the ground, for sunlight of strength 1.
Scattered scatter(const glm::dvec3& view, const glm::dvec3& to_sun) {
    const glm::dvec3 origin(0.0, 0.0, kPlanet + 2.0);
    double length = exit_distance(origin, view, kTop);
    const double ground = ground_distance(origin, view);
    if (ground > 0.0) length = ground;
    const double step = length / kViewSteps;
    Scattered out;
    glm::dvec3 seen(0.0);  // optical depth from the camera
    for (int i = 0; i < kViewSteps; ++i) {
        const glm::dvec3 p = origin + view * ((i + 0.5) * step);
        const glm::dvec3 density = densities(glm::length(p) - kPlanet);
        seen += density * (step * 0.5);
        glm::dvec3 to_light;
        if (depth_to_sky(p, to_sun, to_light)) {
            const glm::dvec3 t = transmittance(seen + to_light);
            out.rayleigh += t * density.x * step;
            out.mie += t * density.y * step;
        }
        seen += density * (step * 0.5);
    }
    out.rayleigh *= kRayleigh;
    out.mie *= kMie;
    return out;
}

double rayleigh_phase(double c) {
    return 3.0 / (16.0 * kPi) * (1.0 + c * c);
}
double mie_phase(double c, double g) {
    return (1.0 - g * g) / (4.0 * kPi * std::pow(1.0 + g * g - 2.0 * g * c, 1.5));
}

// Light from one direction, with Mie scattered evenly (the glow towards the sun is added apart).
glm::dvec3 sky_light(const glm::dvec3& view, const glm::dvec3& to_sun) {
    const Scattered s = scatter(view, to_sun);
    const double c = glm::dot(view, to_sun);
    return s.rayleigh * rayleigh_phase(c) + s.mie * (1.0 / (4.0 * kPi));
}

glm::dvec3 sun_at(double elevation) {
    return {std::cos(elevation), 0.0, std::sin(elevation)};
}

glm::dvec3 sunlight_at(double elevation) {
    glm::dvec3 depth;
    if (!depth_to_sky(glm::dvec3(0.0, 0.0, kPlanet + 2.0), sun_at(elevation), depth))
        return glm::dvec3(0.0);
    return transmittance(depth);
}

double luminance(const glm::dvec3& c) {
    return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
}

// Our units: sunlight of intensity 1 lights a white matte surface to 1. The sky is scaled so,
// with the sun 45 degrees up, its brightest part (the horizon below the sun) peaks at 1.1: pale
// there, rolled off a little by the tone mapper, and a deeper blue overhead.
double sky_scale() {
    static const double scale = [] {
        const glm::dvec3 h = sky_light({std::cos(kLow), 0.0, std::sin(kLow)}, sun_at(kPi / 4.0));
        return 1.1 / std::max({h.r, h.g, h.b});
    }();
    return scale;
}

// A colour split into sRGB (brightest channel 1) and its brightness.
void split(const glm::dvec3& linear, double brightness, glm::vec3& srgb) {
    const glm::dvec3 c = brightness > 0.0 ? linear / brightness : glm::dvec3(0.0);
    srgb = {linear_to_srgb(float(std::clamp(c.r, 0.0, 1.0))),
            linear_to_srgb(float(std::clamp(c.g, 0.0, 1.0))),
            linear_to_srgb(float(std::clamp(c.b, 0.0, 1.0)))};
}

double peak(const glm::dvec3& c) {
    return std::max({c.r, c.g, c.b});
}

}  // namespace

AnalyticSky analytic_sky(float sun_elevation) {
    const double e = sun_elevation;
    const glm::dvec3 to_sun = sun_at(e);
    const double scale = sky_scale();
    // A faint night sky, so it never goes quite black.
    const glm::dvec3 night(0.003, 0.005, 0.012);

    // Directions just above the horizon, towards the sun and away from it, and straight up.
    const glm::dvec3 zenith = sky_light({0.0, 0.0, 1.0}, to_sun) * scale + night;
    const glm::dvec3 toward =
        sky_light({std::cos(kLow), 0.0, std::sin(kLow)}, to_sun) * scale + night;
    const glm::dvec3 away =
        sky_light({-std::cos(kLow), 0.0, std::sin(kLow)}, to_sun) * scale + night;

    // Sunlight after the atmosphere, relative to the sun overhead.
    const glm::dvec3 sun = sunlight_at(e);
    const double overhead = luminance(sunlight_at(kPi / 2.0));
    const double intensity = luminance(sun) / overhead;

    // Ground beyond the world: a dull surface lit by the sun and the sky.
    const glm::dvec3 ground =
        0.15 * (sun / std::max(peak(sun), 1e-9) * intensity * std::max(std::sin(e), 0.0) +
                zenith * 0.6) +
        night * 0.5;

    // The glow: Mie light scattered forwards, beyond the even share already in the gradient,
    // looking along the horizon below a low or set sun.
    const glm::dvec3 glow_dir = sun_at(std::max(e, kLow));
    const Scattered s = scatter(glow_dir, to_sun);
    const glm::dvec3 glow = s.mie * (mie_phase(1.0, kMieG) - 1.0 / (4.0 * kPi)) * scale * 0.4;

    AnalyticSky out;
    SkyColors& c = out.colors;
    c.brightness = float(std::max({peak(zenith), peak(toward), peak(away), peak(ground)}));
    split(zenith, c.brightness, c.zenith);
    split(toward, c.brightness, c.horizon);
    split(away, c.brightness, c.horizon_away);
    split(ground, c.brightness, c.ground);
    c.glow_brightness = float(peak(glow));
    split(glow, peak(glow), c.glow);
    c.glow_tightness = float(kMieG);
    split(sun, peak(sun), c.sun);
    c.sun_brightness = float(50.0 * intensity);
    out.sunlight = c.sun;
    out.sunlight_intensity = float(intensity);
    return out;
}

float fog_depth(const Fog& fog, const glm::vec3& offset, const glm::vec3& up) {
    if (fog.density <= 0.0f) return 0.0f;
    const float length = glm::length(offset);
    const float k = fog.height_falloff;
    if (k <= 0.0f) return fog.density * length;
    // Density falls as exp(-k height): integrated along the ray from the camera's height.
    const float rise = glm::dot(offset, up) * k;
    const float base = fog.density * std::exp(std::clamp(-k * fog.camera_height, -80.0f, 80.0f));
    const float spread = std::abs(rise) < 1e-4f ? 1.0f - 0.5f * rise
                                                : (1.0f - std::exp(std::min(-rise, 80.0f))) / rise;
    return std::min(base * length * spread, 1e6f);  // beyond that it's all fog anyway
}

}  // namespace midgard::render
