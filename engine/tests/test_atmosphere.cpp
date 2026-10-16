// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include <cmath>

#include "midgard/render/atmosphere.h"
#include "midgard/render/mesh_data.h"

using namespace midgard::render;

namespace {

float radians(float degrees) {
    return degrees * 3.14159265f / 180.0f;
}

glm::vec3 linear(const glm::vec3& srgb, float brightness = 1.0f) {
    return glm::vec3(srgb_to_linear(srgb.r), srgb_to_linear(srgb.g), srgb_to_linear(srgb.b)) *
           brightness;
}

float warmth(const glm::vec3& srgb) {  // red over blue
    const glm::vec3 c = linear(srgb);
    return c.r / std::max(c.b, 1e-6f);
}

}  // namespace

TEST_CASE("the analytic sky is blue by day, warm at sunset and dark at night") {
    const AnalyticSky noon = analytic_sky(radians(60.0f));
    const AnalyticSky sunset = analytic_sky(radians(2.0f));
    const AnalyticSky night = analytic_sky(radians(-20.0f));

    const glm::vec3 zenith = linear(noon.colors.zenith);
    CHECK(zenith.b > zenith.g);
    CHECK(zenith.g > zenith.r);
    // The horizon is paler (less saturated) than the zenith.
    CHECK(warmth(noon.colors.horizon) > warmth(noon.colors.zenith));

    // At sunset the horizon turns warm, brightest under the sun.
    CHECK(warmth(sunset.colors.horizon) > warmth(noon.colors.horizon));
    CHECK(linear(sunset.colors.horizon).r > linear(sunset.colors.horizon_away).r);
    CHECK(sunset.colors.brightness < noon.colors.brightness);

    // Sunlight: white and full with the sun high, reddened and weaker low down, none at night.
    CHECK(noon.sunlight_intensity == doctest::Approx(1.0).epsilon(0.1));
    CHECK(sunset.sunlight_intensity < 0.5f);
    CHECK(warmth(sunset.sunlight) > warmth(noon.sunlight));
    CHECK(night.sunlight_intensity == 0.0f);
    CHECK(night.colors.brightness < noon.colors.brightness / 20.0f);
    CHECK(night.colors.brightness > 0.0f);  // never quite black
}

TEST_CASE("the analytic sky is the same every time") {
    const AnalyticSky a = analytic_sky(radians(25.0f));
    const AnalyticSky b = analytic_sky(radians(25.0f));
    CHECK(a.colors.zenith == b.colors.zenith);
    CHECK(a.colors.brightness == b.colors.brightness);
    CHECK(a.sunlight == b.sunlight);
}

TEST_CASE("fog thickens with distance and thins with height") {
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    Fog fog;
    CHECK(fog_depth(fog, {0, 0, -100}, up) == 0.0f);  // no fog by default

    fog.density = 0.01f;
    CHECK(fog_depth(fog, {0, 0, -100}, up) == doctest::Approx(1.0));  // the same everywhere
    CHECK(fog_depth(fog, {0, 50, -100}, up) == doctest::Approx(0.01 * std::sqrt(12500.0)));

    fog.height_falloff = 0.1f;
    fog.camera_height = 5.0f;
    // Level: the density at the camera's height all the way.
    CHECK(fog_depth(fog, {0, 0, -100}, up) == doctest::Approx(0.01 * std::exp(-0.5) * 100));
    // Looking down into it is thicker than looking up out of it.
    const float down = fog_depth(fog, {0, -4, -100}, up);
    const float upwards = fog_depth(fog, {0, 4, -100}, up);
    CHECK(down > fog_depth(fog, {0, 0, -100}, up));
    CHECK(upwards < fog_depth(fog, {0, 0, -100}, up));
    // A sideways "up" (a planet's side): the same fog, turned.
    CHECK(fog_depth(fog, {4, 0, -100}, {-1, 0, 0}) == doctest::Approx(down));

    // Far below the fog's base or far above it: no overflow.
    fog.camera_height = -5000.0f;
    CHECK(fog_depth(fog, {0, -500, -100}, up) == doctest::Approx(1e6));  // all fog
    fog.camera_height = 5000.0f;
    CHECK(fog_depth(fog, {0, 500, -100}, up) >= 0.0f);
}
