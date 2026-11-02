// SPDX-License-Identifier: Apache-2.0
// Inverse kinematics (midgard/ik.h; milestone 5.13): a limb reaching targets near and out of
// reach, bending towards its pole, its bones keeping their lengths; a head aiming within its
// limit; a foot lined up with a slope; and weights blending them in.
#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include <glm/gtc/quaternion.hpp>

#include "midgard/ik.h"

namespace {

// A leg: hip at the origin, knee 0.5 m below, foot 0.5 m below that, toe 0.15 m ahead (+Z).
struct Leg {
    std::vector<midgard::JointTransform> local{
        {{0, 1, 0}}, {{0, -0.5f, 0}}, {{0, -0.5f, 0}}, {{0, 0, 0.15f}}};
    std::vector<int> parents{-1, 0, 1, 2};
    glm::vec3 at(int j) const { return glm::vec3(midgard::model_of(local, parents)[size_t(j)][3]); }
};

bool near(const glm::vec3& a, const glm::vec3& b, float within = 1e-3f) {
    return glm::length(a - b) < within;
}

}  // namespace

TEST_CASE("a limb reaches its target, bending towards the pole, its bones the same length") {
    Leg leg;
    const glm::vec3 target(0, 0.3f, 0.2f);  // up and forward of the straight foot
    REQUIRE(midgard::two_bone_ik(leg.local, leg.parents, 0, 1, 2, target, {0, 0.5f, 1}));
    CHECK(near(leg.at(2), target));
    CHECK(glm::length(leg.at(1) - leg.at(0)) == doctest::Approx(0.5f));
    CHECK(glm::length(leg.at(2) - leg.at(1)) == doctest::Approx(0.5f));
    CHECK(leg.at(1).z > 0.2f);  // the knee forward, towards the pole
    // The pole behind: the knee bends back instead.
    Leg back;
    midgard::two_bone_ik(back.local, back.parents, 0, 1, 2, target, {0, 0.5f, -1});
    CHECK(near(back.at(2), target));
    CHECK(back.at(1).z < 0.0f);
    // Out of reach: straight at it.
    Leg far;
    midgard::two_bone_ik(far.local, far.parents, 0, 1, 2, {2, 1, 0}, {0, 1, 1});
    CHECK(near(far.at(2), {1, 1, 0}, 2e-3f));
    // Half its weight: part of the way; none: as it was.
    Leg half;
    midgard::two_bone_ik(half.local, half.parents, 0, 1, 2, target, {0, 0.5f, 1}, 0.5f);
    CHECK(glm::length(half.at(2) - target) > 0.05f);
    CHECK(glm::length(half.at(2) - target) < glm::length(glm::vec3(0, 0, 0) - target));
    Leg none;
    midgard::two_bone_ik(none.local, none.parents, 0, 1, 2, target, {0, 0.5f, 1}, 0.0f);
    CHECK(near(none.at(2), {0, 0, 0}));
    // Not a chain: refused, unchanged.
    CHECK_FALSE(midgard::two_bone_ik(none.local, none.parents, 0, 2, 3, target, {0, 0, 1}));
}

TEST_CASE("a limb whose joints are turned and scaled still reaches") {
    Leg leg;
    leg.local[0].rotation = glm::angleAxis(0.4f, glm::normalize(glm::vec3(1, 0.3f, 0.2f)));
    leg.local[1].rotation = glm::angleAxis(-0.7f, glm::vec3(1, 0, 0));
    const glm::vec3 target = leg.at(0) + glm::vec3(0.3f, -0.6f, 0.25f);
    midgard::two_bone_ik(leg.local, leg.parents, 0, 1, 2, target, leg.at(0) + glm::vec3(0, 0, 1));
    CHECK(near(leg.at(2), target));
}

TEST_CASE("a head aims at a target, no further than its limit") {
    // A neck at the origin, a head 0.2 m up, looking along +Z.
    std::vector<midgard::JointTransform> local{{{0, 1.5f, 0}}, {{0, 0.2f, 0}}};
    const std::vector<int> parents{-1, 0};
    const auto looking = [&] {
        const glm::mat4 m = midgard::model_of(local, parents)[1];
        return glm::normalize(glm::vec3(m * glm::vec4(0, 0, 1, 0)));
    };
    midgard::aim(local, parents, 1, {0, 0, 1}, {1, 1.7f, 1}, glm::radians(80.0f));
    CHECK(near(looking(), glm::normalize(glm::vec3(1, 0, 1))));  // 45 degrees: within
    local[1].rotation = glm::quat(1, 0, 0, 0);
    midgard::aim(local, parents, 1, {0, 0, 1}, {0, 1.7f, -1}, glm::radians(60.0f));
    CHECK(glm::degrees(std::acos(glm::dot(looking(), glm::vec3(0, 0, 1)))) ==
          doctest::Approx(60.0f).epsilon(0.01));           // behind it: as far as the limit
    CHECK(local[1].translation == glm::vec3(0, 0.2f, 0));  // (only turned)
}

TEST_CASE("a foot lined up with a slope") {
    Leg leg;
    const glm::vec3 slope = glm::normalize(glm::vec3(0, 1, -0.364f));  // 20 degrees, rising +Z
    midgard::orient(leg.local, leg.parents, 2, {0, 1, 0}, slope, glm::radians(45.0f));
    const glm::vec3 toe = leg.at(3) - leg.at(2);
    CHECK(glm::dot(glm::normalize(toe), slope) == doctest::Approx(0.0f).epsilon(1e-3));
    CHECK(toe.y > 0.0f);                // the toe up the slope
    CHECK(near(leg.at(2), {0, 0, 0}));  // the foot where it was
}
