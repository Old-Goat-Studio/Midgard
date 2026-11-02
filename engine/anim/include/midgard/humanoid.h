// SPDX-License-Identifier: Apache-2.0
// The humanoid rig and retargeting (milestone 5.13).
//
// A humanoid skeleton's joints are found among its own by name (`map_humanoid`): the Unreal
// mannequin's (pelvis, spine_01, upperarm_l, thigh_l...), Mixamo's (mixamorig:Hips, LeftArm,
// LeftUpLeg...) and Blender's (upper_arm.L...) are recognised; a designer can name any of them
// instead. A clip made for one humanoid then plays on another of other proportions
// (`retarget`): each joint turns from its own rest pose as the other's joint does (in the
// model's space, so joints whose axes differ still agree), the hips move as far as the other's
// do relative to its height, and every other joint keeps its own skeleton's length. Joints
// outside the rig (fingers, a tail) follow when both skeletons have one of that name.
//
// Rest poses must be alike (both T-poses, or both A-poses): turns are measured from them.
#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "midgard/animation.h"

namespace midgard {

enum class HumanBone {
    Hips,
    Spine,
    Chest,
    UpperChest,
    Neck,
    Head,
    LeftShoulder,
    LeftUpperArm,
    LeftLowerArm,
    LeftHand,
    RightShoulder,
    RightUpperArm,
    RightLowerArm,
    RightHand,
    LeftUpperLeg,
    LeftLowerLeg,
    LeftFoot,
    LeftToes,
    RightUpperLeg,
    RightLowerLeg,
    RightFoot,
    RightToes,
};
inline constexpr int kHumanBones = 22;
const char* human_bone_name(HumanBone bone);  // "hips", "left_upper_arm"...
std::optional<HumanBone> human_bone_from_name(const std::string& name);
// The bones a skeleton must have to be retargeted (the rest are optional: shoulders, upper
// chest, neck, toes).
bool human_bone_required(HumanBone bone);

// A humanoid's bones as joints of a skeleton (indices into a RawSkeleton; -1: none).
struct HumanoidMap {
    std::array<int, kHumanBones> joint;
    HumanoidMap() { joint.fill(-1); }
    int operator[](HumanBone b) const { return joint[size_t(b)]; }
    // The required bones it lacks (none: it can be retargeted).
    std::vector<HumanBone> missing() const;
};

// The bones found among the skeleton's joints by name, with `chosen` (bone name -> joint name,
// a designer's) taking precedence. Throws AnimationError for a chosen joint the skeleton
// hasn't got.
HumanoidMap map_humanoid(const RawSkeleton& skeleton,
                         const std::map<std::string, std::string>& chosen = {});

// `clip` (made for `from`) for `to`: sampled `fps` times a second. Throws AnimationError if
// either skeleton lacks a required bone.
RawClip retarget(const RawClip& clip, const RawSkeleton& from, const HumanoidMap& from_map,
                 const RawSkeleton& to, const HumanoidMap& to_map, float fps = 30.0f);

}  // namespace midgard
