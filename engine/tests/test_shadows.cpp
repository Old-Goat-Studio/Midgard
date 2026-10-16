// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "midgard/render/renderer.h"
#include "midgard/render/shadows.h"

using namespace midgard::render;

namespace {

const glm::mat4 kProjection = perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 500.0f);

glm::mat4 view_towards(const glm::vec3& direction, const glm::vec3& up = {0, 1, 0}) {
    return glm::lookAt(glm::vec3(0.0f), direction, up);
}

bool finite(const glm::mat4& m) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!std::isfinite(m[c][r])) return false;
    return true;
}

// Where a world point lands in a cascade's shadow map, in texels.
glm::dvec2 texel_of(const ShadowCascades& s, int c, const glm::dvec3& world,
                    const glm::dvec3& origin, uint32_t resolution) {
    const glm::vec4 p = s.view_proj[size_t(c)] * glm::vec4(glm::vec3(world - origin), 1.0f);
    return (glm::dvec2(p.x, p.y) * 0.5 + 0.5) * double(resolution);
}

}  // namespace

TEST_CASE("cascades cover the view out to the shadow distance, nearest first") {
    ShadowSettings settings;
    const glm::vec3 sun = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.6f));
    const glm::mat4 view = view_towards({0.3f, -0.2f, -1.0f});
    const ShadowCascades s = fit_cascades(view, kProjection, {}, sun, {0, 1, 0}, settings);
    REQUIRE(s.count == 4);
    for (int c = 0; c < 4; ++c) {
        CHECK(finite(s.view_proj[size_t(c)]));
        CHECK(s.texel_size[size_t(c)] > 0.0f);
        if (c > 0) {
            CHECK(s.far_depth[size_t(c)] > s.far_depth[size_t(c - 1)]);
            CHECK(s.texel_size[size_t(c)] > s.texel_size[size_t(c - 1)]);  // sharper nearby
        }
    }
    CHECK(s.far_depth[3] == doctest::Approx(settings.distance));
    CHECK(s.texel_size[0] < 0.01f);  // a few millimetres near the camera at 2048 texels

    // Every point of the view within each slice falls inside that cascade's map.
    const glm::mat4 inverse_view = glm::inverse(view);
    const glm::mat4 inverse_projection = glm::inverse(kProjection);
    float previous = 0.1f;
    for (int c = 0; c < 4; ++c) {
        for (float depth : {previous, s.far_depth[size_t(c)]})
            for (float x : {-1.0f, 0.0f, 1.0f})
                for (float y : {-1.0f, 0.0f, 1.0f}) {
                    glm::vec4 ray = inverse_projection * glm::vec4(x, y, 1.0f, 1.0f);
                    const glm::vec3 dir = glm::vec3(ray) / ray.w;
                    const glm::vec3 p(inverse_view * glm::vec4(dir / -dir.z * depth, 1.0f));
                    const glm::vec4 q = s.view_proj[size_t(c)] * glm::vec4(p, 1.0f);
                    CHECK(std::abs(q.x) <= 1.0f);
                    CHECK(std::abs(q.y) <= 1.0f);
                    CHECK(q.z >= 0.0f);
                    CHECK(q.z <= 1.0f);
                }
        previous = s.far_depth[size_t(c)];
    }
}

TEST_CASE("cascades keep their size as the camera turns, and move in whole texels") {
    ShadowSettings settings;
    const glm::vec3 sun = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.6f));
    const glm::dvec3 a(1234.25, 10.0, -567.5), b = a + glm::dvec3(0.37, 0.02, -0.81);
    const ShadowCascades sa =
        fit_cascades(view_towards({0, 0, -1}), kProjection, a, sun, {0, 1, 0}, settings);
    const ShadowCascades turned =
        fit_cascades(view_towards({1, -0.3f, 0.2f}), kProjection, a, sun, {0, 1, 0}, settings);
    const ShadowCascades sb =
        fit_cascades(view_towards({0, 0, -1}), kProjection, b, sun, {0, 1, 0}, settings);
    const glm::dvec3 world(1240.0, 9.0, -570.0);  // a fixed point near both cameras
    for (int c = 0; c < 4; ++c) {
        CHECK(turned.texel_size[size_t(c)] == sa.texel_size[size_t(c)]);
        // The point sits at the same place within a texel after the camera moved, so its
        // shadow doesn't shimmer.
        const glm::dvec2 ta = texel_of(sa, c, world, a, settings.resolution);
        const glm::dvec2 tb = texel_of(sb, c, world, b, settings.resolution);
        const glm::dvec2 frac_a = ta - glm::floor(ta), frac_b = tb - glm::floor(tb);
        CHECK(std::abs(frac_a.x - frac_b.x) < 0.02);
        CHECK(std::abs(frac_a.y - frac_b.y) < 0.02);
    }
}

TEST_CASE("the light's axes follow the local up, wherever the sun is") {
    ShadowSettings settings;
    // On a planet's side, "up" is along x. The sun straight overhead, straight below, or
    // along the old world up: no degenerate axes.
    for (const glm::vec3& up : {glm::vec3(1, 0, 0), glm::vec3(0, -1, 0), glm::vec3(0.6f, 0.8f, 0)})
        for (const glm::vec3& sun : {-up, up, glm::vec3(0, -1, 0), glm::vec3(0.3f, 0.1f, -1)}) {
            const ShadowCascades s =
                fit_cascades(view_towards({0, 0, -1}, up), kProjection, {}, sun, up, settings);
            REQUIRE(s.count == 4);
            for (int c = 0; c < 4; ++c) CHECK(finite(s.view_proj[size_t(c)]));
        }

    // With the sun overhead (up along z here), a point straight above another lands on the same
    // texel, nearer the sun: shadows fall straight down, whichever way "up" points.
    const glm::vec3 up(0.0f, 0.0f, 1.0f);
    const ShadowCascades s =
        fit_cascades(view_towards({1, 0, 0}, up), kProjection, {}, -up, up, settings);
    const glm::vec4 low = s.view_proj[1] * glm::vec4(8, 1, 0, 1);
    const glm::vec4 high = s.view_proj[1] * glm::vec4(8, 1, 3, 1);
    CHECK(low.x == doctest::Approx(high.x).epsilon(1e-4));
    CHECK(low.y == doctest::Approx(high.y).epsilon(1e-4));
    CHECK(high.z < low.z);  // higher is nearer the sun
}

TEST_CASE("casters are kept or skipped per cascade, and shadows can be off") {
    ShadowSettings settings;
    const glm::vec3 sun = glm::normalize(glm::vec3(0.0f, -1.0f, -0.2f));
    const ShadowCascades s =
        fit_cascades(view_towards({0, 0, -1}), kProjection, {}, sun, {0, 1, 0}, settings);
    CHECK(casts_into(s, 0, {0, 0, -1}, 0.5f));
    CHECK(casts_into(s, 0, {0, 30, -1}, 0.5f));         // high above, towards the sun: casts
    CHECK_FALSE(casts_into(s, 0, {200, 0, -1}, 0.5f));  // far to the side
    CHECK_FALSE(casts_into(s, 0, {0, -60, -1}, 0.5f));  // deep below everything it could shade
    CHECK_FALSE(casts_into(s, 5, {0, 0, -1}, 0.5f));

    settings.cascades = 0;
    CHECK(fit_cascades(view_towards({0, 0, -1}), kProjection, {}, sun, {0, 1, 0}, settings).count ==
          0);
}
