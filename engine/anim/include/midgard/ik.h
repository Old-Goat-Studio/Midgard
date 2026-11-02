// SPDX-License-Identifier: Apache-2.0
// Inverse kinematics (milestone 5.13): poses bent to reach where they must.
//
// Each works on a pose's joints relative to their parents (`local`, as Pose::local gives them)
// and the skeleton's parents, in model space for its targets, and changes only the rotations of
// the joints it names (so bone lengths never change), blended in by `weight` (0 to 1).
//
//   two_bone_ik: a limb (upper, lower, end: a thigh, shin and foot; or an upper arm, forearm and
//                hand) bent so its end reaches a target, its middle joint bending towards a pole
//                (a knee forwards, an elbow back); out of reach, it points straight at it.
//   aim:         a joint turned so one of its own directions points at a target (a head
//                looking), no more than a limit from where the pose had it.
//   orient:      a joint turned so one of its own directions lines up with a direction (a foot's
//                sole with the ground's slope), no more than a limit.
#pragma once

#include <span>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "midgard/animation.h"

namespace midgard {

// Each joint relative to the model, from each relative to its parent (parents before children).
std::vector<glm::mat4> model_of(std::span<const JointTransform> local,
                                std::span<const int> parents);
// One joint relative to the model (its parents' chain multiplied: cheaper than all of them).
glm::mat4 joint_model(std::span<const JointTransform> local, std::span<const int> parents,
                      int joint);

// Returns false if the joints don't make a chain (end's parent is lower, lower's is upper).
bool two_bone_ik(std::vector<JointTransform>& local, std::span<const int> parents, int upper,
                 int lower, int end, const glm::vec3& target, const glm::vec3& pole,
                 float weight = 1.0f);

void aim(std::vector<JointTransform>& local, std::span<const int> parents, int joint,
         const glm::vec3& forward, const glm::vec3& target, float limit_radians,
         float weight = 1.0f);

void orient(std::vector<JointTransform>& local, std::span<const int> parents, int joint,
            const glm::vec3& axis, const glm::vec3& direction, float limit_radians,
            float weight = 1.0f);

}  // namespace midgard
