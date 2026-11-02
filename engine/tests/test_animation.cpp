// SPDX-License-Identifier: Apache-2.0
// Skeletal animation (midgard/animation.h, midgard/gltf_skin.h; milestone 5.13): a skinned
// glTF read into a skeleton, clips and skinned vertices; poses sampled against poses worked out
// by hand; skeletons and clips saved and loaded; bad skeletons refused.
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "midgard/animation.h"
#include "midgard/gltf_skin.h"
#include "midgard/pak.h"

namespace {

// A three-joint chain (j0 at the origin, j1 a metre up, j2 a metre above that) under an
// armature moved 5 m along X, one triangle skinned to it (a vertex on each joint), and a clip
// "Bend" turning j1 from rest to 90 degrees about Z over a second.
std::vector<std::uint8_t> chain_glb() {
    std::vector<std::uint8_t> bin;
    const auto put = [&](const void* p, size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        bin.insert(bin.end(), b, b + n);
    };
    const float positions[9] = {0, 0, 0, 0.1f, 1, 0, 0, 2, 0};  // 0: 36 bytes
    put(positions, sizeof positions);
    const std::uint16_t joints[12] = {0, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0};  // 36: 24
    put(joints, sizeof joints);
    const float weights[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};  // 60: 48
    put(weights, sizeof weights);
    for (int j = 0; j < 3; ++j) {  // 108: 192 (inverse binds: translate by -j up)
        const glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(0, -float(j), 0));
        put(&m[0][0], 64);
    }
    const float times[2] = {0, 1};  // 300: 8
    put(times, sizeof times);
    const float s = std::sqrt(0.5f);
    const float turns[8] = {0, 0, 0, 1, 0, 0, s, s};  // 308: 32
    put(turns, sizeof turns);
    const std::string json = R"({
      "asset": {"version": "2.0"},
      "scene": 0, "scenes": [{"nodes": [0]}],
      "nodes": [
        {"name": "Armature", "translation": [5, 0, 0], "children": [1, 4]},
        {"name": "j0", "children": [2]},
        {"name": "j1", "translation": [0, 1, 0], "children": [3]},
        {"name": "j2", "translation": [0, 1, 0]},
        {"name": "Body", "mesh": 0, "skin": 0}],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "JOINTS_0": 1,
                                                  "WEIGHTS_0": 2}}]}],
      "skins": [{"joints": [1, 2, 3], "inverseBindMatrices": 3}],
      "animations": [{"name": "Bend", "channels": [{"sampler": 0,
                       "target": {"node": 2, "path": "rotation"}}],
                      "samplers": [{"input": 4, "output": 5, "interpolation": "LINEAR"}]}],
      "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
         "min": [0, 0, 0], "max": [0.1, 2, 0]},
        {"bufferView": 1, "componentType": 5123, "count": 3, "type": "VEC4"},
        {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC4"},
        {"bufferView": 3, "componentType": 5126, "count": 3, "type": "MAT4"},
        {"bufferView": 4, "componentType": 5126, "count": 2, "type": "SCALAR",
         "min": [0], "max": [1]},
        {"bufferView": 5, "componentType": 5126, "count": 2, "type": "VEC4"}],
      "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": 36},
        {"buffer": 0, "byteOffset": 36, "byteLength": 24},
        {"buffer": 0, "byteOffset": 60, "byteLength": 48},
        {"buffer": 0, "byteOffset": 108, "byteLength": 192},
        {"buffer": 0, "byteOffset": 300, "byteLength": 8},
        {"buffer": 0, "byteOffset": 308, "byteLength": 32}],
      "buffers": [{"byteLength": 340}]})";
    std::string padded = json;
    while (padded.size() % 4) padded += ' ';
    std::vector<std::uint8_t> out;
    const auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(std::uint8_t(v >> (8 * i)));
    };
    u32(0x46546C67);  // glTF
    u32(2);
    u32(std::uint32_t(12 + 8 + padded.size() + 8 + bin.size()));
    u32(std::uint32_t(padded.size()));
    u32(0x4E4F534A);  // JSON
    out.insert(out.end(), padded.begin(), padded.end());
    u32(std::uint32_t(bin.size()));
    u32(0x004E4942);  // BIN
    out.insert(out.end(), bin.begin(), bin.end());
    return out;
}

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
