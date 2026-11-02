// SPDX-License-Identifier: Apache-2.0
// Skeletal animation (midgard/animation.h, midgard/gltf_skin.h; milestone 5.13): a skinned
// glTF read into a skeleton, clips and skinned vertices; poses sampled against poses worked out
// by hand; skeletons and clips saved and loaded; bad skeletons refused.
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include "midgard/animation.h"
#include "midgard/characters.h"
#include "midgard/gltf_skin.h"
#include "midgard/pak.h"
#include "skinned_chain.h"

namespace {

glm::vec3 position(const midgard::Pose& pose, const midgard::Skeleton& s, const char* joint) {
    return glm::vec3(pose.model()[size_t(s.find(joint))][3]);
}

// Within 2 mm: ozz keeps clips compressed (translations as half floats, rotations quantised),
// so sampled poses are that close to the keys, not exact.
bool near(const glm::vec3& a, const glm::vec3& b) {
    return glm::length(a - b) < 2e-3f;
}

}  // namespace

TEST_CASE("a skinned glTF: its skeleton, clips and skinned vertices") {
    const std::vector<std::uint8_t> glb = chain_glb();
    const midgard::SkinnedModel model = midgard::load_skinned_glb(glb);
    CHECK(model.skeleton.names == std::vector<std::string>{"j0", "j1", "j2"});
    CHECK(model.skeleton.parents == std::vector<int>{-1, 0, 1});
    CHECK(near(model.skeleton.rest[2].translation, {0, 1, 0}));
    CHECK(near(glm::vec3(model.armature[3]), {5, 0, 0}));
    REQUIRE(model.clips.size() == 1);
    CHECK(model.clips[0].name == "Bend");
    CHECK(model.clips[0].duration == 1.0f);
    REQUIRE(model.clips[0].tracks.size() == 1);
    CHECK(model.clips[0].tracks[0].joint == "j1");
    CHECK(model.mesh.vertices.size() == 3);
    CHECK(model.mesh.indices.size() == 3);
    CHECK(model.joints[2] == glm::u16vec4(2, 0, 0, 0));
    CHECK(model.weights[1] == glm::vec4(1, 0, 0, 0));
    // Not skinned: refused.
    CHECK_THROWS_AS(midgard::load_skinned_glb(std::vector<std::uint8_t>{1, 2, 3}),
                    midgard::AssetError);
}

TEST_CASE("poses sampled from a clip match poses worked out by hand") {
    const midgard::SkinnedModel model = midgard::load_skinned_glb(chain_glb());
    const midgard::BuiltAnimation built = midgard::build_animation(model);
    const midgard::Skeleton& s = built.skeleton;
    REQUIRE(s.joint_count() == 3);
    REQUIRE(built.clips.size() == 1);
    const midgard::Clip& bend = built.clips[0];
    CHECK(bend.duration() == doctest::Approx(1.0f));
    midgard::Pose pose(s);
    CHECK(near(position(pose, s, "j2"), {0, 2, 0}));  // at rest
    // Half way: j1 turned 45 degrees about Z, so j2 is a metre from j1 that way.
    const float h = std::sqrt(0.5f);
    pose.sample(bend, 0.5f);
    CHECK(near(position(pose, s, "j1"), {0, 1, 0}));
    CHECK(near(position(pose, s, "j2"), {-h, 1 + h, 0}));
    pose.sample(bend, 1.5f);  // looping: the same
    CHECK(near(position(pose, s, "j2"), {-h, 1 + h, 0}));
    pose.sample(bend, 1.5f, false);  // held at the end: 90 degrees
    CHECK(near(position(pose, s, "j2"), {-1, 1, 0}));
    // Skinning: the vertex on j2 follows it.
    const int j2 = built.skin_to_skeleton[2];
    const glm::vec4 skinned =
        pose.model()[size_t(j2)] * model.inverse_binds[2] * glm::vec4(0, 2, 0, 1);
    CHECK(near(glm::vec3(skinned), {-1, 1, 0}));
    pose.rest();
    CHECK(near(position(pose, s, "j2"), {0, 2, 0}));
}

TEST_CASE("skeletons and clips are saved and loaded unchanged") {
    const midgard::BuiltAnimation built =
        midgard::build_animation(midgard::load_skinned_glb(chain_glb()));
    const midgard::Skeleton s = midgard::Skeleton::load(built.skeleton.save());
    CHECK(s.names() == built.skeleton.names());
    CHECK(s.parents() == built.skeleton.parents());
    const midgard::Clip clip = midgard::Clip::load(built.clips[0].save());
    CHECK(clip.name() == "Bend");
    midgard::Pose a(built.skeleton), b(s);
    a.sample(built.clips[0], 0.3f);
    b.sample(clip, 0.3f);
    for (int j = 0; j < 3; ++j) CHECK(a.model()[size_t(j)] == b.model()[size_t(j)]);
    CHECK_THROWS_AS(midgard::Skeleton::load(built.clips[0].save()), midgard::AnimationError);
}

TEST_CASE("bad skeletons and clips are refused") {
    midgard::RawSkeleton loop{{"a", "b"}, {1, 0}, {{}, {}}};
    CHECK_THROWS_AS(midgard::Skeleton::build(loop), midgard::AnimationError);
    midgard::RawSkeleton twice{{"a", "a"}, {-1, 0}, {{}, {}}};
    CHECK_THROWS_AS(midgard::Skeleton::build(twice), midgard::AnimationError);
    CHECK_THROWS_AS(midgard::Skeleton::build({}), midgard::AnimationError);
    const midgard::Skeleton one = midgard::Skeleton::build({{"root"}, {-1}, {{}}});
    midgard::RawClip backwards{"back", 1.0f, {{"root", {{0.5f, {}}, {0.2f, {}}}, {}, {}}}};
    CHECK_THROWS_AS(midgard::Clip::build(one, backwards), midgard::AnimationError);
    midgard::RawClip empty{"none", 0.0f, {}};
    CHECK_THROWS_AS(midgard::Clip::build(one, empty), midgard::AnimationError);
}

TEST_CASE("characters play their clips, from their model or another file") {
    const std::vector<std::uint8_t> glb = chain_glb();
    const midgard::Characters::Bytes bytes = [&](const std::string& asset) {
        if (asset == "asset:chain.glb" || asset == "asset:moves.glb") return glb;
        if (asset == "asset:static.glb") return std::vector<std::uint8_t>{1, 2, 3};
        throw midgard::AssetError("not in the package");
    };
    midgard::Scene scene;
    const auto add = [&](const char* id, const nlohmann::json& character) {
        scene.put_entity(id, id, "", {{"transform", {}}, {"character", character}});
        return scene.find(id);
    };
    const entt::entity bend = add("e1", {{"model", "asset:chain.glb"}, {"clip", "Bend"}});
    const entt::entity fast = add("e2", {{"model", "asset:chain.glb"},
                                         {"clips", "asset:moves.glb"},
                                         {"clip", "Bend"},
                                         {"speed", 2.0}});
    add("e3", {{"model", "asset:missing.glb"}});
    add("e4", {{"model", "asset:static.glb"}});
    midgard::Characters characters;
    const std::vector<std::string> problems = characters.load(scene, bytes);
    REQUIRE(problems.size() == 2);                 // the missing file and the one that isn't glTF
    CHECK(characters.load(scene, bytes).empty());  // (each said once)
    characters.update(scene, 0.25f);
    REQUIRE(characters.pose(bend));
    CHECK(characters.clip_of(bend) == "Bend");
    CHECK(characters.clip_of(fast) == "Bend");
    // A quarter of a second in: a quarter of the way from rest to 90 degrees; the fast one half
    // way (45). ozz blends rotations linearly and normalises (not spherically), so between keys
    // the angle is that blend's.
    const midgard::Skeleton& s = midgard::Skeleton::build(
        midgard::load_skinned_glb(glb).skeleton);  // (the same order as theirs)
    const auto j2 = [&](entt::entity e) {
        return glm::vec3(characters.pose(e)->model()[size_t(s.find("j2"))][3]);
    };
    const auto blended = [](float t) {  // the angle ozz's blend gives t of the way to 90
        const glm::quat q =
            glm::normalize(glm::quat(1, 0, 0, 0) * (1.0f - t) +
                           glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1)) * t);
        return glm::angle(q);
    };
    const float a = blended(0.25f), b = blended(0.5f);
    CHECK(b == doctest::Approx(glm::radians(45.0f)));
    CHECK(glm::length(j2(bend) - glm::vec3(-std::sin(a), 1 + std::cos(a), 0)) < 2e-3f);
    CHECK(glm::length(j2(fast) - glm::vec3(-std::sin(b), 1 + std::cos(b), 0)) < 2e-3f);
    CHECK_FALSE(characters.pose(scene.find("e3")));  // (its model is missing: not animated)
    // Not drawn until uploaded.
    std::vector<midgard::render::DrawItem> draws;
    characters.draws(scene, glm::dvec3(0.0), midgard::MaterialCache{}, entt::null, draws);
    CHECK(draws.empty());
    // A character removed goes; others carry on.
    scene.remove_entity("e2");
    characters.update(scene, 0.25f);
    CHECK_FALSE(characters.pose(fast));
    CHECK(characters.clip_of(bend) == "Bend");
}
