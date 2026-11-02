// SPDX-License-Identifier: Apache-2.0
#include "midgard/ik.h"

#include <algorithm>
#include <cmath>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

namespace midgard {

namespace {

glm::vec3 position(const glm::mat4& m) {
    return glm::vec3(m[3]);
}

glm::quat rotation(const glm::mat4& m) {
    glm::mat3 r(m);
    for (int c = 0; c < 3; ++c) r[c] = glm::normalize(r[c]);  // (scale out)
    return glm::normalize(glm::quat_cast(r));
}

glm::mat4 joint_matrix(const JointTransform& t) {
    glm::mat4 m = glm::mat4_cast(t.rotation);
    m[0] *= t.scale.x;
    m[1] *= t.scale.y;
    m[2] *= t.scale.z;
    m[3] = glm::vec4(t.translation, 1.0f);
    return m;
}

// A joint's rotation in the model (the joint itself, or its parent's: none for a root).
glm::quat rotation_of(std::span<const JointTransform> local, std::span<const int> parents,
                      int joint) {
    return joint < 0 ? glm::quat(1, 0, 0, 0) : rotation(joint_model(local, parents, joint));
}

// Turns a joint by `q` (a rotation in the model, about the joint).
void turn(std::vector<JointTransform>& local, std::span<const int> parents, int joint,
          const glm::quat& q) {
    const glm::quat world = rotation_of(local, parents, joint);
    const glm::quat parent = rotation_of(local, parents, parents[size_t(joint)]);
    local[size_t(joint)].rotation = glm::normalize(glm::inverse(parent) * q * world);
}

// The shortest turn from one direction to another, at most `limit` radians of it.
glm::quat between(const glm::vec3& from, const glm::vec3& to, float limit) {
    const glm::vec3 a = glm::normalize(from), b = glm::normalize(to);
    glm::quat q = glm::rotation(a, b);
    const float angle = glm::angle(q);
    if (angle > limit && angle > 1e-6f) q = glm::slerp(glm::quat(1, 0, 0, 0), q, limit / angle);
    return q;
}

glm::vec3 any_perpendicular(const glm::vec3& v) {
    const glm::vec3 other = std::abs(v.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
    return glm::normalize(glm::cross(v, other));
}

void blend(std::vector<JointTransform>& local,
           std::initializer_list<std::pair<int, glm::quat>> before, float weight) {
    if (weight >= 1.0f) return;
    for (const auto& [j, a] : before) {
        glm::quat b = local[size_t(j)].rotation;
        if (glm::dot(a, b) < 0.0f) b = -b;
        local[size_t(j)].rotation = glm::normalize(glm::slerp(a, b, std::max(0.0f, weight)));
    }
}

}  // namespace

std::vector<glm::mat4> model_of(std::span<const JointTransform> local,
                                std::span<const int> parents) {
    std::vector<glm::mat4> out(local.size());
    for (size_t j = 0; j < local.size(); ++j) {
        const glm::mat4 m = joint_matrix(local[j]);
        const int p = parents[j];
        out[j] = p < 0 ? m : out[size_t(p)] * m;
    }
    return out;
}

glm::mat4 joint_model(std::span<const JointTransform> local, std::span<const int> parents,
                      int joint) {
    glm::mat4 m = joint_matrix(local[size_t(joint)]);
    for (int p = parents[size_t(joint)]; p >= 0; p = parents[size_t(p)])
        m = joint_matrix(local[size_t(p)]) * m;
    return m;
}

bool two_bone_ik(std::vector<JointTransform>& local, std::span<const int> parents, int upper,
                 int lower, int end, const glm::vec3& target, const glm::vec3& pole, float weight) {
    if (parents[size_t(end)] != lower || parents[size_t(lower)] != upper) return false;
    if (weight <= 0.0f) return true;
    const glm::quat upper_before = local[size_t(upper)].rotation;
    const glm::quat lower_before = local[size_t(lower)].rotation;
    const auto at = [&](int j) { return position(joint_model(local, parents, j)); };
    glm::vec3 a = at(upper), b = at(lower), c = at(end);
    const float lab = glm::length(b - a), lcb = glm::length(c - b);
    if (lab < 1e-6f || lcb < 1e-6f) return true;
    // 1. The middle joint bent so the limb is as long as the way to the target.
    const float reach =
        std::clamp(glm::length(target - a), std::abs(lab - lcb) + 1e-4f, lab + lcb - 1e-4f);
    const float want = std::acos(
        std::clamp((lab * lab + lcb * lcb - reach * reach) / (2.0f * lab * lcb), -1.0f, 1.0f));
    const float now =
        std::acos(std::clamp(glm::dot(glm::normalize(a - b), glm::normalize(c - b)), -1.f, 1.f));
    glm::vec3 axis = glm::cross(a - b, c - b);
    if (glm::length(axis) < 1e-6f) {  // straight: any axis across it (step 3 turns it about)
        axis = glm::cross(c - a, pole - a);
        if (glm::length(axis) < 1e-6f) axis = any_perpendicular(c - a);
    }
    turn(local, parents, lower, glm::angleAxis(want - now, glm::normalize(axis)));
    // 2. The upper joint turned so the end is on the way to the target.
    c = at(end);
    if (glm::length(target - a) > 1e-6f)
        turn(local, parents, upper, between(c - a, target - a, 4.0f));
    // 3. Turned about that way so the middle joint bends towards the pole.
    b = at(lower);
    c = at(end);
    const glm::vec3 k = glm::length(c - a) > 1e-6f ? glm::normalize(c - a) : glm::vec3(0, 1, 0);
    const glm::vec3 bend = (b - a) - k * glm::dot(b - a, k);
    const glm::vec3 toward = (pole - a) - k * glm::dot(pole - a, k);
    if (glm::length(bend) > 1e-5f && glm::length(toward) > 1e-5f) {
        const glm::vec3 u = glm::normalize(bend), v = glm::normalize(toward);
        const float angle = std::atan2(glm::dot(glm::cross(u, v), k), glm::dot(u, v));
        turn(local, parents, upper, glm::angleAxis(angle, k));
    }
    blend(local, {{upper, upper_before}, {lower, lower_before}}, weight);
    return true;
}

void aim(std::vector<JointTransform>& local, std::span<const int> parents, int joint,
         const glm::vec3& forward, const glm::vec3& target, float limit_radians, float weight) {
    if (weight <= 0.0f) return;
    const glm::mat4 model = joint_model(local, parents, joint);
    const glm::vec3 from = rotation(model) * forward;
    const glm::vec3 to = target - position(model);
    if (glm::length(to) < 1e-6f || glm::length(from) < 1e-6f) return;
    const glm::quat q = between(from, to, limit_radians);
    turn(local, parents, joint,
         glm::slerp(glm::quat(1, 0, 0, 0), q, std::clamp(weight, 0.0f, 1.0f)));
}

void orient(std::vector<JointTransform>& local, std::span<const int> parents, int joint,
            const glm::vec3& axis, const glm::vec3& direction, float limit_radians, float weight) {
    if (weight <= 0.0f || glm::length(direction) < 1e-6f) return;
    const glm::vec3 from = rotation_of(local, parents, joint) * axis;
    const glm::quat q = between(from, direction, limit_radians);
    turn(local, parents, joint,
         glm::slerp(glm::quat(1, 0, 0, 0), q, std::clamp(weight, 0.0f, 1.0f)));
}

}  // namespace midgard
