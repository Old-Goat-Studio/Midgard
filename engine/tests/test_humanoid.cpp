// SPDX-License-Identifier: Apache-2.0
// The humanoid rig and retargeting (midgard/humanoid.h; milestone 5.13): bones found by name
// in the common rigs' naming, a designer's choices over them; clips retargeted between
// humanoids of other proportions and other joint axes, checked against poses worked out by
// hand; and the Quaternius library's clips on the Superhero, as characters play them.
#include <doctest/doctest.h>

#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include "midgard/animation.h"
#include "midgard/characters.h"
#include "midgard/gltf_skin.h"
#include "midgard/humanoid.h"

using midgard::HumanBone;

namespace {

// A humanoid's joints named as `names` gives them (in HumanBone order, plus extras), `scale`
// times the size of a 1.8 m figure, each joint's axes turned by `twist` from the model's
// (as a rig from another tool might have them).
struct Figure {
    midgard::RawSkeleton skeleton;
    std::vector<int> index;  // by HumanBone
};

Figure figure(const std::vector<std::string>& names, float scale,
              const glm::quat& twist = glm::quat(1, 0, 0, 0)) {
    // Model-space positions (metres for scale 1) and parents, by bone.
    struct Bone {
        HumanBone bone;
        HumanBone parent;  // (Hips: none)
        glm::vec3 at;
    };
    using B = HumanBone;
    const std::vector<Bone> bones = {
        {B::Hips, B::Hips, {0, 1.0f, 0}},
        {B::Spine, B::Hips, {0, 1.1f, 0}},
        {B::Chest, B::Spine, {0, 1.3f, 0}},
        {B::Neck, B::Chest, {0, 1.5f, 0}},
        {B::Head, B::Neck, {0, 1.6f, 0}},
        {B::LeftShoulder, B::Chest, {0.05f, 1.45f, 0}},
        {B::LeftUpperArm, B::LeftShoulder, {0.2f, 1.45f, 0}},
        {B::LeftLowerArm, B::LeftUpperArm, {0.5f, 1.45f, 0}},
        {B::LeftHand, B::LeftLowerArm, {0.75f, 1.45f, 0}},
        {B::RightShoulder, B::Chest, {-0.05f, 1.45f, 0}},
        {B::RightUpperArm, B::RightShoulder, {-0.2f, 1.45f, 0}},
        {B::RightLowerArm, B::RightUpperArm, {-0.5f, 1.45f, 0}},
        {B::RightHand, B::RightLowerArm, {-0.75f, 1.45f, 0}},
        {B::LeftUpperLeg, B::Hips, {0.1f, 0.95f, 0}},
        {B::LeftLowerLeg, B::LeftUpperLeg, {0.1f, 0.5f, 0}},
        {B::LeftFoot, B::LeftLowerLeg, {0.1f, 0.08f, 0}},
        {B::RightUpperLeg, B::Hips, {-0.1f, 0.95f, 0}},
        {B::RightLowerLeg, B::RightUpperLeg, {-0.1f, 0.5f, 0}},
        {B::RightFoot, B::RightLowerLeg, {-0.1f, 0.08f, 0}},
    };
    Figure f;
    f.index.assign(midgard::kHumanBones, -1);
    for (const Bone& b : bones) {
        const int j = int(f.skeleton.names.size());
        f.index[size_t(b.bone)] = j;
        f.skeleton.names.push_back(names[size_t(b.bone)]);
        const bool root = b.bone == B::Hips;
        const int p = root ? -1 : f.index[size_t(b.parent)];
        f.skeleton.parents.push_back(p);
        // Every joint's axes are `twist` in the model, so locally: the root's twist, the rest
        // none; offsets are in the parent's (twisted) axes.
        midgard::JointTransform t;
        if (root) {
            t.translation = scale * b.at;
            t.rotation = twist;
        } else {
            const glm::vec3 parent_at = [&] {
                for (const Bone& o : bones)
                    if (o.bone == b.parent) return o.at;
                return glm::vec3(0.0f);
            }();
            t.translation = glm::inverse(twist) * (scale * (b.at - parent_at));
        }
        f.skeleton.rest.push_back(t);
    }
    return f;
}

std::vector<std::string> unreal_names() {
    return {"pelvis",     "spine_01",   "spine_02",   "spine_03", "neck_01",    "head",
            "clavicle_l", "upperarm_l", "lowerarm_l", "hand_l",   "clavicle_r", "upperarm_r",
            "lowerarm_r", "hand_r",     "thigh_l",    "calf_l",   "foot_l",     "ball_l",
            "thigh_r",    "calf_r",     "foot_r",     "ball_r"};
}

std::vector<std::string> mixamo_names() {
    return {"mixamorig:Hips",         "mixamorig:Spine",         "mixamorig:Spine1",
            "mixamorig:Spine2",       "mixamorig:Neck",          "mixamorig:Head",
            "mixamorig:LeftShoulder", "mixamorig:LeftArm",       "mixamorig:LeftForeArm",
            "mixamorig:LeftHand",     "mixamorig:RightShoulder", "mixamorig:RightArm",
            "mixamorig:RightForeArm", "mixamorig:RightHand",     "mixamorig:LeftUpLeg",
            "mixamorig:LeftLeg",      "mixamorig:LeftFoot",      "mixamorig:LeftToeBase",
            "mixamorig:RightUpLeg",   "mixamorig:RightLeg",      "mixamorig:RightFoot",
            "mixamorig:RightToeBase"};
}

// Where a joint is in the model once posed (the clip at `time`, built for the skeleton).
glm::vec3 posed_at(const midgard::RawSkeleton& raw, const midgard::RawClip& clip, float time,
                   int joint) {
    const midgard::Skeleton s = midgard::Skeleton::build(raw);
    const midgard::Clip c = midgard::Clip::build(s, clip);
    midgard::Pose pose(s);
    pose.sample(c, time, false);
    return glm::vec3(pose.model()[size_t(s.find(raw.names[size_t(joint)]))][3]);
}

bool near(const glm::vec3& a, const glm::vec3& b, float within = 3e-3f) {
    return glm::length(a - b) < within;
}

}  // namespace

TEST_CASE("humanoid bones are found by name in the common rigs") {
    const auto check = [](const std::vector<std::string>& names) {
        midgard::RawSkeleton s = figure(names, 1.0f).skeleton;
        const midgard::HumanoidMap m = midgard::map_humanoid(s);
        CHECK(m.missing().empty());
        for (HumanBone b : {HumanBone::Hips, HumanBone::Head, HumanBone::LeftUpperArm,
                            HumanBone::RightLowerLeg, HumanBone::LeftFoot, HumanBone::Chest})
            CHECK(s.names[size_t(m[b])] == names[size_t(b)]);
    };
    // Figure's joints in HumanBone order: the names given, with upper chest and toes left out.
    check(unreal_names());
    check(mixamo_names());
    check({"hips",       "spine",       "chest",     "upper_chest", "neck",       "head",
           "shoulder.L", "upper_arm.L", "forearm.L", "hand.L",      "shoulder.R", "upper_arm.R",
           "forearm.R",  "hand.R",      "thigh.L",   "shin.L",      "foot.L",     "toe.L",
           "thigh.R",    "shin.R",      "foot.R",    "toe.R"});  // Blender's
    check({"Bip01 Pelvis",    "Bip01 Spine",  "Bip01 Spine1",     "Bip01 Spine2",
           "Bip01 Neck",      "Bip01 Head",   "Bip01 L Clavicle", "Bip01 L UpperArm",
           "Bip01 L Forearm", "Bip01 L Hand", "Bip01 R Clavicle", "Bip01 R UpperArm",
           "Bip01 R Forearm", "Bip01 R Hand", "Bip01 L Thigh",    "Bip01 L Calf",
           "Bip01 L Foot",    "Bip01 L Toe0", "Bip01 R Thigh",    "Bip01 R Calf",
           "Bip01 R Foot",    "Bip01 R Toe0"});  // 3ds Max's Biped
}

TEST_CASE("the spine's joints, fingers left out, and a designer's choices") {
    midgard::RawSkeleton s;
    const auto add = [&](const std::string& name, int parent) {
        s.names.push_back(name);
        s.parents.push_back(parent);
        s.rest.push_back({});
        return int(s.names.size()) - 1;
    };
    const int hips = add("Hips", -1);
    const int s1 = add("Spine", hips);
    const int s2 = add("Spine1", s1);
    const int s3 = add("Spine2", s2);
    add("LeftHandIndex1", s3);  // a finger: not the hand
    add("index_01_l", s3);
    add("LeftToe_End", hips);
    const int head = add("Head", s3);
    const int helmet = add("Helmet", head);
    midgard::HumanoidMap m = midgard::map_humanoid(s);
    CHECK(m[HumanBone::Hips] == hips);
    CHECK(m[HumanBone::Spine] == s1);
    CHECK(m[HumanBone::Chest] == s2);
    CHECK(m[HumanBone::UpperChest] == s3);
    CHECK(m[HumanBone::Head] == head);
    CHECK(m[HumanBone::LeftHand] == -1);
    CHECK(m[HumanBone::LeftToes] == -1);
    CHECK(m.missing().size() == 12);  // arms and legs
    // A designer's choices win: the helmet as the head, and no upper chest.
    m = midgard::map_humanoid(s, {{"head", "Helmet"}, {"upper_chest", ""}});
    CHECK(m[HumanBone::Head] == helmet);
    CHECK(m[HumanBone::UpperChest] == -1);
    m = midgard::map_humanoid(s, {{"chest", "Spine"}});  // a joint is one bone
    CHECK(m[HumanBone::Chest] == s1);
    CHECK(m[HumanBone::Spine] == -1);
    CHECK_THROWS_AS(midgard::map_humanoid(s, {{"tail", "Hips"}}), midgard::AnimationError);
    CHECK_THROWS_AS(midgard::map_humanoid(s, {{"head", "Hat"}}), midgard::AnimationError);
    CHECK(std::string(midgard::human_bone_name(HumanBone::LeftUpperArm)) == "left_upper_arm");
    CHECK(midgard::human_bone_from_name("right_toes") == HumanBone::RightToes);
}

TEST_CASE("a clip retargeted to a humanoid of other proportions and joint axes") {
    // From a 1.8 m figure with model-aligned axes to a 2.7 m one whose joints' axes are turned
    // 90 degrees about Y (and named the Mixamo way).
    const Figure from = figure(unreal_names(), 1.0f);
    const glm::quat twist = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
    const Figure to = figure(mixamo_names(), 1.5f, twist);
    // The clip: the hips move 1 m forward (+Z) and the left upper arm turns down 90 degrees
    // (about Z, in its own axes: the model's), over a second.
    midgard::RawClip clip{"Wave", 1.0f, {}};
    const glm::quat down = glm::angleAxis(glm::radians(-90.0f), glm::vec3(0, 0, 1));
    clip.tracks.push_back({"pelvis", {{0, {0, 1, 0}}, {1, {0, 1, 1}}}, {}, {}});
    clip.tracks.push_back({"upperarm_l", {}, {{0, glm::quat(1, 0, 0, 0)}, {1, down}}, {}});
    const midgard::RawClip made =
        midgard::retarget(clip, from.skeleton, midgard::map_humanoid(from.skeleton), to.skeleton,
                          midgard::map_humanoid(to.skeleton), 30.0f);
    CHECK(made.name == "Wave");
    CHECK(made.duration == 1.0f);
    const auto at = [&](HumanBone b, float t) {
        return posed_at(to.skeleton, made, t, to.index[size_t(b)]);
    };
    // At the start: at rest.
    CHECK(near(at(HumanBone::LeftHand, 0), {1.125f, 2.175f, 0}));
    // At the end: the hips 1.5 m forward (as far as the clip's, for their height); the arm
    // straight down from the shoulder, at its own length (0.825 m to the hand).
    CHECK(near(at(HumanBone::Hips, 1), {0, 1.5f, 1.5f}));
    const glm::vec3 shoulder = at(HumanBone::LeftUpperArm, 1);
    CHECK(near(shoulder, {0.3f, 2.175f, 1.5f}));
    CHECK(near(at(HumanBone::LeftHand, 1), shoulder + glm::vec3(0, -0.825f, 0)));
    // Half way: 45 degrees down.
    const float h = std::sqrt(0.5f) * 0.825f;
    CHECK(near(at(HumanBone::LeftHand, 0.5f),
               at(HumanBone::LeftUpperArm, 0.5f) + glm::vec3(h, -h, 0)));
    // The other arm and the legs stay at rest, carried by the hips.
    CHECK(near(at(HumanBone::RightHand, 1), {-1.125f, 2.175f, 1.5f}));
    CHECK(near(at(HumanBone::LeftFoot, 1), {0.15f, 0.12f, 1.5f}));
}

TEST_CASE("retargeting needs humanoids") {
    const Figure whole = figure(unreal_names(), 1.0f);
    midgard::RawSkeleton chain;
    chain.names = {"a", "b"};
    chain.parents = {-1, 0};
    chain.rest.resize(2);
    const midgard::RawClip clip{"Nothing", 1.0f, {}};
    try {
        (void)midgard::retarget(clip, whole.skeleton, midgard::map_humanoid(whole.skeleton), chain,
                                midgard::map_humanoid(chain));
        FAIL("retargeted to a chain");
    } catch (const midgard::AnimationError& e) {
        CHECK(std::string(e.what()).find("no hips, spine, head") != std::string::npos);
    }
}

TEST_CASE("the Quaternius library's clips retargeted to the Superhero") {
    const std::string dir = MIDGARD_ASSETS_DIR "/third_party/characters/quaternius/";
    const midgard::SkinnedModel hero = midgard::load_skinned_gltf_file(dir + "superhero_male.glb");
    const midgard::SkinnedModel library =
        midgard::load_skinned_gltf_file(dir + "animation_library.glb");
    const midgard::HumanoidMap hero_map = midgard::map_humanoid(hero.skeleton);
    const midgard::HumanoidMap library_map = midgard::map_humanoid(library.skeleton);
    CHECK(hero_map.missing().empty());
    CHECK(library_map.missing().empty());
    CHECK(hero.skeleton.names[size_t(hero_map[HumanBone::Hips])] == "pelvis");
    CHECK(hero.skeleton.names[size_t(hero_map[HumanBone::UpperChest])] == "spine_03");
    CHECK(hero.skeleton.names[size_t(hero_map[HumanBone::LeftToes])] == "ball_l");
    const midgard::RawClip* walk = nullptr;
    for (const midgard::RawClip& c : library.clips)
        if (c.name == "Walk_Loop") walk = &c;
    REQUIRE(walk);
    const midgard::RawClip made =
        midgard::retarget(*walk, library.skeleton, library_map, hero.skeleton, hero_map);
    const midgard::Skeleton s = midgard::Skeleton::build(hero.skeleton);
    const midgard::Clip clip = midgard::Clip::build(s, made);
    CHECK(clip.duration() == doctest::Approx(walk->duration));
    // Through the walk the hero keeps its own proportions: its forearms the length of its own.
    midgard::Pose pose(s);
    const int elbow = s.find("lowerarm_l");
    const int wrist = s.find("hand_l");
    const auto length = [&] {
        return glm::length(glm::vec3(pose.model()[size_t(wrist)][3]) -
                           glm::vec3(pose.model()[size_t(elbow)][3]));
    };
    pose.rest();
    const float rest_length = length();
    for (float t = 0.0f; t < walk->duration; t += walk->duration / 7.0f) {
        pose.sample(clip, t);
        CHECK(length() == doctest::Approx(rest_length).epsilon(0.01));
    }
}

TEST_CASE("characters play another file's clips retargeted, or by name with a rig that isn't") {
    const std::string dir = MIDGARD_ASSETS_DIR "/third_party/characters/quaternius/";
    const auto read = [](const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
    };
    const std::vector<std::uint8_t> hero = read(dir + "superhero_male.glb");
    const std::vector<std::uint8_t> library = read(dir + "animation_library.glb");
    REQUIRE(!hero.empty());
    const midgard::Characters::Bytes bytes = [&](const std::string& asset) {
        return asset == "asset:hero.glb" ? hero : library;
    };
    midgard::Scene scene;
    const nlohmann::json walking = {
        {"model", "asset:hero.glb"}, {"clips", "asset:library.glb"}, {"clip", "Walk_Loop"}};
    scene.put_entity("a", "a", "", {{"transform", {}}, {"character", walking}});
    nlohmann::json no_hips = walking;  // (a designer said it has no hips: not a humanoid)
    no_hips["rig"] = {{"hips", ""}};
    scene.put_entity("b", "b", "", {{"transform", {}}, {"character", no_hips}});
    nlohmann::json wrong = walking;  // (a joint it hasn't got: said, and the names used)
    wrong["rig"] = {{"head", "Helmet"}};
    scene.put_entity("c", "c", "", {{"transform", {}}, {"character", wrong}});
    midgard::Characters characters;
    const std::vector<std::string> problems = characters.load(scene, bytes);
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].find("Helmet") != std::string::npos);
    characters.update(scene, 0.3f);
    const midgard::Skeleton s = midgard::Skeleton::build(midgard::load_skinned_glb(hero).skeleton);
    const auto forearm = [&](const midgard::Pose& pose) {
        return glm::length(glm::vec3(pose.model()[size_t(s.find("hand_l"))][3]) -
                           glm::vec3(pose.model()[size_t(s.find("lowerarm_l"))][3]));
    };
    midgard::Pose rest(s);
    // Retargeted: the hero's own forearms (0.244 m); by name: the library's (0.273 m).
    const midgard::Pose* a = characters.pose(scene.find("a"));
    const midgard::Pose* b = characters.pose(scene.find("b"));
    const midgard::Pose* c = characters.pose(scene.find("c"));
    REQUIRE((a && b && c));
    CHECK(forearm(*a) == doctest::Approx(forearm(rest)).epsilon(0.01));
    CHECK(forearm(*c) == doctest::Approx(forearm(rest)).epsilon(0.01));
    CHECK(forearm(*b) == doctest::Approx(0.2726).epsilon(0.01));
    CHECK(characters.clip_of(scene.find("a")) == "Walk_Loop");
}
