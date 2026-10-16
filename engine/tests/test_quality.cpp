// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>

#include "midgard/render/quality.h"
#include "midgard/render/renderer.h"

using namespace midgard::render;

TEST_CASE("the phone tier spends less than the desktop tier") {
    const QualitySettings desktop = quality_settings(Quality::Desktop);
    const QualitySettings phone = quality_settings(Quality::Phone);
    CHECK(desktop.tier == Quality::Desktop);
    CHECK(phone.tier == Quality::Phone);
    CHECK(phone.shadows.cascades > 0);  // phones have sun shadows too
    CHECK(phone.shadows.cascades < desktop.shadows.cascades);
    CHECK(phone.shadows.resolution < desktop.shadows.resolution);
    CHECK(phone.shadows.distance < desktop.shadows.distance);
    CHECK(phone.max_texture_size < desktop.max_texture_size);
    CHECK(desktop.soft_shadows);
    CHECK_FALSE(phone.soft_shadows);

    // The phone's cascades still cover its whole shadow distance.
    const glm::mat4 view = glm::lookAt(glm::vec3(0), glm::vec3(0, -0.2f, -1), glm::vec3(0, 1, 0));
    const glm::mat4 projection = perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 500.0f);
    const ShadowCascades s =
        fit_cascades(view, projection, {}, {-0.4f, -1, -0.6f}, {0, 1, 0}, phone.shadows);
    REQUIRE(s.count == phone.shadows.cascades);
    CHECK(s.far_depth[size_t(s.count - 1)] == doctest::Approx(phone.shadows.distance));
}

TEST_CASE("quality tiers by name") {
    for (Quality q : {Quality::Desktop, Quality::Phone}) CHECK(parse_quality(quality_name(q)) == q);
    CHECK_FALSE(parse_quality("ultra").has_value());
#ifdef __ANDROID__
    CHECK(default_quality() == Quality::Phone);
#else
    CHECK(default_quality() == Quality::Desktop);
#endif
}
