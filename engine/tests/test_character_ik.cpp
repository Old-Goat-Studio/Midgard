// SPDX-License-Identifier: Apache-2.0
// Characters' inverse kinematics (midgard/characters.h; milestone 5.13): the Superhero's feet on
// a 20 degree slope as high above it as its animation lifts them, along its own up (the slope
// and the character turned on their side too, as on a planet); its head looking at a point
// within its limit; a hand reaching a point.
#include <doctest/doctest.h>

#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include "midgard/camera_rig.h"
#include "midgard/characters.h"
#include "midgard/gltf_skin.h"
#include "midgard/humanoid.h"

using nlohmann::json;

namespace {

std::vector<std::uint8_t> read(const std::string& name) {
    std::ifstream in(std::string(MIDGARD_ASSETS_DIR "/third_party/characters/quaternius/") + name,
                     std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
}

struct World {
    std::vector<std::uint8_t> hero = read("superhero_male.glb"),
                              library = read("animation_library.glb");
    midgard::Scene scene;
    midgard::Characters characters;
    midgard::Skeleton skeleton = midgard::Skeleton::build(midgard::load_skinned_glb(hero).skeleton);
    // (As Characters has it: the model turned to face the entity's -Z.)
    glm::mat4 armature = glm::rotate(glm::mat4(1.0f), glm::pi<float>(), glm::vec3(0, 1, 0)) *
                         midgard::load_skinned_glb(hero).armature;

    entt::entity add(const char* id, const json& character, const glm::vec3& rotation = {}) {
        json c = character;
        c["model"] = "asset:hero.glb";
        c["clips"] = "asset:library.glb";
        json transform = json::object();
        transform["rotation"] = {rotation.x, rotation.y, rotation.z};
        json components = json::object();
        components["transform"] = transform;
        components["character"] = c;
        scene.put_entity(id, id, "", components);
        const entt::entity e = scene.find(id);
        const auto problems = characters.load(scene, [&](const std::string& asset) {
            return asset == "asset:hero.glb" ? hero : library;
        });
        CHECK(problems.empty());
        return e;
    }
    // A joint where the world has it.
    glm::dvec3 joint(entt::entity e, const char* name) const {
        const glm::vec4 p = armature * characters.pose(e)->model()[size_t(skeleton.find(name))][3];
        return glm::dvec3(scene.world_matrix(e) * glm::dvec4(glm::dvec3(p), 1.0));
    }
};

// A plane through the origin: the ground, its normal `n`.
midgard::Characters::Ground plane(glm::dvec3 n) {
    n = glm::normalize(n);
    return [n](entt::entity, const glm::dvec3& from, const glm::vec3& direction,
               double distance) -> std::optional<midgard::Characters::GroundHit> {
        const double along = glm::dot(glm::dvec3(direction), n);
        if (std::abs(along) < 1e-9) return std::nullopt;
        const double t = -glm::dot(from, n) / along;
        if (t < 0.0 || t > distance) return std::nullopt;
        return midgard::Characters::GroundHit{from + t * glm::dvec3(direction), glm::vec3(n)};
    };
}

}  // namespace

TEST_CASE("feet stand on a slope as high as their animation lifts them, along the local up") {
    // Upright, and turned on its side (its up along world -X, as on the side of a planet): the
    // ground rising 20 degrees ahead of the character either way.
    for (const glm::vec3 turned : {glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 90.0f)}) {
        CAPTURE(turned.z);
        World w;
        const json idle = {{"clip", "Idle_Loop"}};
        const entt::entity flat = w.add("flat", idle, turned);  // (no IK: as animated)
        json standing = idle;
        standing["ik"] = {{"feet", true}};
        const entt::entity sloped = w.add("sloped", standing, turned);
        const glm::dmat3 frame(w.scene.world_matrix(sloped));
        const glm::dvec3 up = glm::normalize(frame[1]);
        const double slope = glm::radians(20.0);
        const glm::dvec3 normal = frame * glm::dvec3(0.0, std::cos(slope), -std::sin(slope));
        w.characters.set_ground(plane(normal));
        w.characters.update(w.scene, 0.0f);
        for (const char* foot : {"foot_l", "foot_r"}) {
            CAPTURE(foot);
            const double animated = glm::dot(w.joint(flat, foot), up);  // above its own ground
            // Above the slope, along up: the ground under the foot is where the plane is.
            const glm::dvec3 at = w.joint(sloped, foot);
            const double ground =
                -glm::dot(at - up * glm::dot(at, up), normal) / glm::dot(up, normal);
            CHECK(glm::dot(at, up) - ground == doctest::Approx(animated).epsilon(0.01));
        }
        // The legs kept their length (the hips lowered for the lower foot instead).
        const auto shin = [&](entt::entity e) {
            return glm::length(w.joint(e, "foot_l") - w.joint(e, "calf_l"));
        };
        CHECK(shin(sloped) == doctest::Approx(shin(flat)).epsilon(0.001));
        CHECK(glm::dot(w.joint(sloped, "pelvis"), up) < glm::dot(w.joint(flat, "pelvis"), up));
    }
}

TEST_CASE("a character looks at a point within its limit, and reaches out a hand") {
    World w;
    json c = {{"clip", "Idle_Loop"}};
    c["ik"] = {{"look_limit", 60}};
    const entt::entity e = w.add("hero", c);
    const entt::entity twin = w.add("twin", c);  // (the same, looking at nothing)
    w.characters.update(w.scene, 0.0f);
    const glm::dvec3 head = w.joint(e, "Head");
    const auto rotation = [&](entt::entity who) {
        return glm::mat3(w.armature *
                         w.characters.pose(who)->model()[size_t(w.skeleton.find("Head"))]);
    };
    // Where the head faces: its own axis that faces forward (the entity's -Z) as animated, now.
    const auto facing = [&] {
        const glm::vec3 axis = glm::inverse(rotation(twin)) * glm::vec3(0, 0, -1);
        return glm::normalize(glm::dvec3(rotation(e) * axis));
    };
    // A point 45 degrees to its left (it faces -Z, so its left is -X), at head height: looked
    // at, within 60.
    const glm::dvec3 target = head + glm::dvec3(-1.0, 0.0, -1.0);
    w.characters.look_at(e, target);
    for (int i = 0; i < 60; ++i) w.characters.update(w.scene, 1.0f / 30.0f);  // (eased in)
    const glm::dvec3 wanted = glm::normalize(target - w.joint(e, "Head"));
    CHECK(glm::degrees(std::acos(std::clamp(glm::dot(facing(), wanted), -1.0, 1.0))) < 3.0);
    // Behind it: only as far as the limit allows.
    w.characters.look_at(e, head + glm::dvec3(0.0, 0.0, 1.0));
    for (int i = 0; i < 60; ++i) w.characters.update(w.scene, 1.0f / 30.0f);
    const glm::dvec3 ahead = glm::normalize(
        glm::dvec3(rotation(twin) * (glm::inverse(rotation(twin)) * glm::vec3(0, 0, -1))));
    const double turned = glm::degrees(std::acos(std::clamp(glm::dot(facing(), ahead), -1.0, 1.0)));
    CHECK(turned < 61.0);
    CHECK(turned > 40.0);
    // A hand to a point in front of its chest.
    const glm::dvec3 shoulder = w.joint(e, "upperarm_r");
    const glm::dvec3 point = shoulder + glm::dvec3(-0.1, -0.2, -0.35);
    w.characters.look_at(e, std::nullopt);
    w.characters.reach(e, true, point);
    for (int i = 0; i < 40; ++i) w.characters.update(w.scene, 1.0f / 30.0f);
    CHECK(glm::length(w.joint(e, "hand_r") - point) < 0.01);
    w.characters.reach(e, true, std::nullopt);  // let go: back where the animation has it
    for (int i = 0; i < 60; ++i) w.characters.update(w.scene, 1.0f / 30.0f);
    CHECK(glm::length(w.joint(e, "hand_r") - point) > 0.1);
}

TEST_CASE("a character faces where its entity faces (-Z), as a walking player does") {
    // A person facing f (up u) has its left hand on the side cross(u, f).
    const auto left_side = [](World& w, entt::entity e) {
        return glm::normalize(w.joint(e, "upperarm_l") - w.joint(e, "upperarm_r"));
    };
    World w;
    const json idle = {{"clip", "Idle_Loop"}};
    const entt::entity still = w.add("still", idle);  // facing -Z: its left is -X
    w.characters.update(w.scene, 0.0f);
    CHECK(glm::dot(left_side(w, still), glm::dvec3(-1, 0, 0)) > 0.9);
    // Turned the way a player walking along +X is turned: its left is then -Z.
    const glm::vec3 walking = midgard::upright_rotation(glm::vec3(1, 0, 0), glm::vec3(0, 1, 0));
    const entt::entity walker = w.add("walker", idle, walking);
    w.characters.update(w.scene, 0.0f);
    CHECK(glm::dot(left_side(w, walker), glm::dvec3(0, 0, -1)) > 0.9);
}
