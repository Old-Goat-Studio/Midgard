// SPDX-License-Identifier: Apache-2.0
#include "midgard/humanoid.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>

namespace midgard {

namespace {

constexpr const char* kNames[kHumanBones] = {
    "hips",           "spine",           "chest",           "upper_chest",     "neck",
    "head",           "left_shoulder",   "left_upper_arm",  "left_lower_arm",  "left_hand",
    "right_shoulder", "right_upper_arm", "right_lower_arm", "right_hand",      "left_upper_leg",
    "left_lower_leg", "left_foot",       "left_toes",       "right_upper_leg", "right_lower_leg",
    "right_foot",     "right_toes",
};

enum class Side { None, Left, Right };

// A joint's name as words: lower case, split at anything not a letter or digit, at a change of
// case (UpperArm) and between letters and digits (Spine1); a namespace (mixamorig:) and rig
// prefixes (Bip01, DEF-) left out.
std::vector<std::string> words(std::string name) {
    if (const size_t colon = name.rfind(':'); colon != std::string::npos)
        name = name.substr(colon + 1);
    std::vector<std::string> out;
    std::string w;
    const auto flush = [&] {
        if (!w.empty()) out.push_back(w);
        w.clear();
    };
    for (size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (!std::isalnum(c)) {
            flush();
            continue;
        }
        if (!w.empty()) {
            const unsigned char p = static_cast<unsigned char>(name[i - 1]);
            const bool case_change = std::isupper(c) && std::islower(p);
            const bool digit_change = (std::isdigit(c) != 0) != (std::isdigit(p) != 0);
            if (case_change || digit_change) flush();
        }
        w.push_back(char(std::tolower(c)));
    }
    flush();
    std::erase_if(out, [](const std::string& s) {
        return s == "mixamorig" || s == "bip" || s == "def" || s == "org" || s == "jnt" ||
               s == "bone";
    });
    return out;
}

// What a joint's name says: its side and the rest of its words, run together without numbers
// ("upperarm_l": left, "upperarm"; "LeftUpLeg": left, "upleg").
struct Reading {
    Side side = Side::None;
    std::string core;
};
Reading read(const std::string& name) {
    Reading r;
    for (const std::string& w : words(name)) {
        if (w == "l" || w == "left")
            r.side = Side::Left;
        else if (w == "r" || w == "right")
            r.side = Side::Right;
        else if (!std::isdigit(static_cast<unsigned char>(w[0])))
            r.core += w;
    }
    return r;
}

// The bone a joint's name names (not the spine's: they're counted), by its side.
std::optional<HumanBone> bone_of(const Reading& r) {
    using B = HumanBone;
    const auto sided = [&](B left, B right) -> std::optional<B> {
        if (r.side == Side::Left) return left;
        if (r.side == Side::Right) return right;
        return std::nullopt;
    };
    const auto is = [&](std::initializer_list<const char*> names) {
        return std::any_of(names.begin(), names.end(), [&](const char* n) { return r.core == n; });
    };
    if (r.side == Side::None) {
        if (is({"hips", "hip", "pelvis"})) return B::Hips;
        if (is({"chest"})) return B::Chest;
        if (is({"upperchest"})) return B::UpperChest;
        if (is({"neck"})) return B::Neck;
        if (is({"head"})) return B::Head;
        return std::nullopt;
    }
    if (is({"shoulder", "clavicle", "collar", "collarbone"}))
        return sided(B::LeftShoulder, B::RightShoulder);
    if (is({"upperarm", "arm", "uparm"})) return sided(B::LeftUpperArm, B::RightUpperArm);
    if (is({"lowerarm", "forearm", "elbow"})) return sided(B::LeftLowerArm, B::RightLowerArm);
    if (is({"hand", "wrist"})) return sided(B::LeftHand, B::RightHand);
    if (is({"upperleg", "upleg", "thigh"})) return sided(B::LeftUpperLeg, B::RightUpperLeg);
    if (is({"lowerleg", "leg", "calf", "shin", "knee"}))
        return sided(B::LeftLowerLeg, B::RightLowerLeg);
    if (is({"foot", "ankle"})) return sided(B::LeftFoot, B::RightFoot);
    if (is({"toes", "toe", "toebase", "ball"})) return sided(B::LeftToes, B::RightToes);
    return std::nullopt;
}

std::vector<int> depths(const std::vector<int>& parents) {
    std::vector<int> d(parents.size(), -1);
    const std::function<int(int)> depth = [&](int j) -> int {
        if (d[size_t(j)] >= 0) return d[size_t(j)];
        const int p = parents[size_t(j)];
        d[size_t(j)] = 0;  // (a loop stops here)
        d[size_t(j)] = p < 0 || p >= int(parents.size()) ? 0 : depth(p) + 1;
        return d[size_t(j)];
    };
    for (int j = 0; j < int(parents.size()); ++j) depth(j);
    return d;
}

// Joints in an order with parents before their children.
std::vector<int> parents_first(const std::vector<int>& parents) {
    const std::vector<int> d = depths(parents);
    std::vector<int> order(parents.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = int(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return d[a] < d[b]; });
    return order;
}

struct Rigid {  // a joint in model space: where it is and how it's turned
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

// Each joint relative to the model, from each relative to its parent.
std::vector<Rigid> to_model(const std::vector<int>& parents, const std::vector<int>& order,
                            const std::vector<JointTransform>& local) {
    std::vector<Rigid> out(local.size());
    for (int j : order) {
        const JointTransform& l = local[size_t(j)];
        const int p = parents[size_t(j)];
        if (p < 0) {
            out[size_t(j)] = {l.translation, l.rotation, l.scale};
            continue;
        }
        const Rigid& pp = out[size_t(p)];
        out[size_t(j)] = {pp.position + pp.rotation * (pp.scale * l.translation),
                          glm::normalize(pp.rotation * l.rotation), pp.scale * l.scale};
    }
    return out;
}

template <class T>
T at(const std::vector<AnimationKey<T>>& keys, float time, const T& rest) {
    if (keys.empty()) return rest;
    if (time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;
    const auto next =
        std::upper_bound(keys.begin(), keys.end(), time,
                         [](float t, const AnimationKey<T>& k) { return t < k.time; });
    const auto prev = next - 1;
    const float span = next->time - prev->time;
    const float f = span > 0.0f ? (time - prev->time) / span : 0.0f;
    if constexpr (std::is_same_v<T, glm::quat>)
        return glm::slerp(prev->value, next->value, f);
    else
        return glm::mix(prev->value, next->value, f);
}

}  // namespace

const char* human_bone_name(HumanBone bone) {
    return kNames[size_t(bone)];
}

std::optional<HumanBone> human_bone_from_name(const std::string& name) {
    for (int b = 0; b < kHumanBones; ++b)
        if (name == kNames[b]) return HumanBone(b);
    return std::nullopt;
}

bool human_bone_required(HumanBone bone) {
    switch (bone) {
        case HumanBone::Chest:
        case HumanBone::UpperChest:
        case HumanBone::Neck:
        case HumanBone::LeftShoulder:
        case HumanBone::RightShoulder:
        case HumanBone::LeftToes:
        case HumanBone::RightToes:
            return false;
        default:
            return true;
    }
}

std::vector<HumanBone> HumanoidMap::missing() const {
    std::vector<HumanBone> out;
    for (int b = 0; b < kHumanBones; ++b)
        if (joint[size_t(b)] < 0 && human_bone_required(HumanBone(b))) out.push_back(HumanBone(b));
    return out;
}

HumanoidMap map_humanoid(const RawSkeleton& skeleton,
                         const std::map<std::string, std::string>& chosen) {
    HumanoidMap map;
    const std::vector<int> depth = depths(skeleton.parents);
    // The shallowest joint naming each bone; the spine's joints, outermost first.
    std::vector<int> spine;
    for (int j = 0; j < int(skeleton.names.size()); ++j) {
        const Reading r = read(skeleton.names[size_t(j)]);
        if (r.side == Side::None && r.core == "spine") {
            spine.push_back(j);
            continue;
        }
        const std::optional<HumanBone> b = bone_of(r);
        if (!b) continue;
        int& slot = map.joint[size_t(*b)];
        if (slot < 0 || depth[size_t(j)] < depth[size_t(slot)]) slot = j;
    }
    std::stable_sort(spine.begin(), spine.end(),
                     [&](int a, int b) { return depth[size_t(a)] < depth[size_t(b)]; });
    // Spine, Spine1, Spine2 (or spine_01..spine_05): the first, the second (unless a joint is
    // called chest) and the last of three or more (unless one is called upper chest).
    if (!spine.empty()) map.joint[size_t(HumanBone::Spine)] = spine.front();
    if (spine.size() >= 2 && map[HumanBone::Chest] < 0)
        map.joint[size_t(HumanBone::Chest)] = spine[1];
    if (spine.size() >= 3 && map[HumanBone::UpperChest] < 0)
        map.joint[size_t(HumanBone::UpperChest)] = spine.back();
    // A designer's choices, over what the names said.
    for (const auto& [bone_name, joint_name] : chosen) {
        const std::optional<HumanBone> b = human_bone_from_name(bone_name);
        if (!b) throw AnimationError("there is no humanoid bone called '" + bone_name + "'");
        int j = -1;
        if (!joint_name.empty()) {
            const auto it = std::find(skeleton.names.begin(), skeleton.names.end(), joint_name);
            if (it == skeleton.names.end())
                throw AnimationError("the skeleton has no joint called '" + joint_name + "'");
            j = int(it - skeleton.names.begin());
            for (int& other : map.joint)
                if (other == j) other = -1;  // (a joint is one bone)
        }
        map.joint[size_t(*b)] = j;
    }
    return map;
}

RawClip retarget(const RawClip& clip, const RawSkeleton& from, const HumanoidMap& from_map,
                 const RawSkeleton& to, const HumanoidMap& to_map, float fps) {
    const auto check = [](const HumanoidMap& m, const char* which) {
        const std::vector<HumanBone> missing = m.missing();
        if (missing.empty()) return;
        std::string list;
        for (HumanBone b : missing)
            list += std::string(list.empty() ? "" : ", ") + human_bone_name(b);
        throw AnimationError(std::string("the ") + which + " skeleton isn't a humanoid: no " +
                             list);
    };
    check(from_map, "clip's");
    check(to_map, "character's");
    if (!(fps > 0.0f)) throw AnimationError("retargeting needs a positive frame rate");

    const std::vector<int> from_order = parents_first(from.parents);
    const std::vector<int> to_order = parents_first(to.parents);
    const std::vector<Rigid> from_rest = to_model(from.parents, from_order, from.rest);
    const std::vector<Rigid> to_rest = to_model(to.parents, to_order, to.rest);

    // Each of the character's joints follows one of the clip's: its bone's, or one of its name.
    std::vector<int> source(to.names.size(), -1);
    for (size_t j = 0; j < to.names.size(); ++j) {
        const auto it = std::find(from.names.begin(), from.names.end(), to.names[j]);
        if (it != from.names.end()) source[j] = int(it - from.names.begin());
    }
    for (int b = 0; b < kHumanBones; ++b) {
        const int tj = to_map.joint[size_t(b)];
        if (tj >= 0) source[size_t(tj)] = from_map.joint[size_t(b)];  // (-1: stays at rest)
    }
    const int from_hips = from_map[HumanBone::Hips];
    const int to_hips = to_map[HumanBone::Hips];
    // The hips move as far as the clip's do relative to its hips' height.
    const float from_height = from_rest[size_t(from_hips)].position.y;
    const float to_height = to_rest[size_t(to_hips)].position.y;
    const float stride = from_height > 1e-4f && to_height > 1e-4f ? to_height / from_height : 1.0f;

    // The clip's tracks by joint.
    std::vector<const RawTrack*> tracks(from.names.size(), nullptr);
    for (const RawTrack& t : clip.tracks) {
        const auto it = std::find(from.names.begin(), from.names.end(), t.joint);
        if (it != from.names.end()) tracks[size_t(it - from.names.begin())] = &t;
    }

    RawClip out;
    out.name = clip.name;
    out.duration = clip.duration;
    std::vector<RawTrack> made(to.names.size());
    for (size_t j = 0; j < made.size(); ++j) made[j].joint = to.names[j];
    const int frames =
        clip.duration > 0.0f ? std::max(1, int(std::ceil(clip.duration * fps - 1e-4f))) : 0;
    std::vector<JointTransform> local(from.names.size());
    std::vector<Rigid> turned(to.names.size());
    for (int f = 0; f <= frames; ++f) {
        const float time = std::min(clip.duration, float(f) / fps);
        for (size_t j = 0; j < local.size(); ++j) {
            const JointTransform& rest = from.rest[j];
            const RawTrack* t = tracks[j];
            local[j] = t ? JointTransform{at(t->translations, time, rest.translation),
                                          glm::normalize(at(t->rotations, time, rest.rotation)),
                                          at(t->scales, time, rest.scale)}
                         : rest;
        }
        const std::vector<Rigid> posed = to_model(from.parents, from_order, local);
        for (int j : to_order) {
            const int p = to.parents[size_t(j)];
            const glm::quat parent = p < 0 ? glm::quat(1, 0, 0, 0) : turned[size_t(p)].rotation;
            const int s = source[size_t(j)];
            // Turned in the model as its source is from rest, or at rest under its parent.
            const glm::quat rotation =
                s >= 0 ? glm::normalize(posed[size_t(s)].rotation *
                                        glm::inverse(from_rest[size_t(s)].rotation) *
                                        to_rest[size_t(j)].rotation)
                       : glm::normalize(parent * to.rest[size_t(j)].rotation);
            turned[size_t(j)].rotation = rotation;
            glm::quat l = glm::normalize(glm::inverse(parent) * rotation);
            if (!made[size_t(j)].rotations.empty() &&
                glm::dot(made[size_t(j)].rotations.back().value, l) < 0.0f)
                l = -l;  // (the short way round from the last key)
            made[size_t(j)].rotations.push_back({time, l});
        }
        // The hips' place: theirs at rest, moved as the clip's hips move (scaled).
        const glm::vec3 moved =
            to_rest[size_t(to_hips)].position +
            (posed[size_t(from_hips)].position - from_rest[size_t(from_hips)].position) * stride;
        // Into the hips' parent's frame (it may be turned; it stays where it rests).
        std::vector<JointTransform> to_local(to.names.size());
        for (size_t j = 0; j < to_local.size(); ++j)
            to_local[j] = {to.rest[j].translation, made[j].rotations.back().value,
                           to.rest[j].scale};
        const std::vector<Rigid> placed = to_model(to.parents, to_order, to_local);
        const int hp = to.parents[size_t(to_hips)];
        glm::vec3 translation = moved;
        if (hp >= 0) {
            const Rigid& pp = placed[size_t(hp)];
            translation = (glm::inverse(pp.rotation) * (moved - pp.position)) / pp.scale;
        }
        made[size_t(to_hips)].translations.push_back({time, translation});
    }
    for (RawTrack& t : made)
        if (!t.rotations.empty() || !t.translations.empty()) out.tracks.push_back(std::move(t));
    return out;
}

}  // namespace midgard
