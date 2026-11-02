// SPDX-License-Identifier: Apache-2.0
// Skeletal animation (milestone 5.13; ozz-animation): skeletons, clips and poses.
//
// A skeleton is joints in a tree, each with its rest pose (translation, rotation and scale
// relative to its parent). A clip moves some of them over time. A pose samples a clip at a
// moment (looping or not) into each joint's transform relative to the model ("model space"),
// which skinning multiplies by the joints' inverse bind matrices.
//
// Skeletons and clips are built from raw data (an importer's: midgard/gltf_skin.h) and saved
// as bytes (ozz's archives) for game.pak. ozz orders joints depth first; names (unique in a
// skeleton) are how anything else refers to them.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace midgard {

class AnimationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct JointTransform {
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

// A skeleton as an importer reads it: joints in any order, each naming its parent's index
// (-1: a root) and with its rest pose.
struct RawSkeleton {
    std::vector<std::string> names;
    std::vector<int> parents;
    std::vector<JointTransform> rest;
};

template <class T>
struct AnimationKey {
    float time = 0.0f;  // seconds
    T value{};
};

// A clip as an importer reads it: tracks by joint name (joints without one stay at rest).
struct RawTrack {
    std::string joint;
    std::vector<AnimationKey<glm::vec3>> translations;
    std::vector<AnimationKey<glm::quat>> rotations;
    std::vector<AnimationKey<glm::vec3>> scales;
};
struct RawClip {
    std::string name;
    float duration = 0.0f;  // seconds
    std::vector<RawTrack> tracks;
};

class Skeleton {
public:
    // Throws AnimationError (no joints, a parent out of range or a loop, repeated names).
    static Skeleton build(const RawSkeleton& raw);
    static Skeleton load(std::span<const std::uint8_t> bytes);
    std::vector<std::uint8_t> save() const;

    Skeleton(Skeleton&&) noexcept;
    Skeleton& operator=(Skeleton&&) noexcept;
    ~Skeleton();

    int joint_count() const;
    const std::vector<std::string>& names() const { return names_; }
    const std::vector<int>& parents() const { return parents_; }  // in this skeleton's order
    int find(const std::string& name) const;                      // -1 if there is none

    struct Impl;
    const Impl& impl() const { return *impl_; }

private:
    Skeleton();
    std::unique_ptr<Impl> impl_;
    std::vector<std::string> names_;
    std::vector<int> parents_;
};

class Clip {
public:
    // Throws AnimationError (keys out of order or outside the duration). Tracks for joints
    // the skeleton doesn't have are left out.
    static Clip build(const Skeleton& skeleton, const RawClip& raw);
    static Clip load(std::span<const std::uint8_t> bytes);
    std::vector<std::uint8_t> save() const;

    Clip(Clip&&) noexcept;
    Clip& operator=(Clip&&) noexcept;
    ~Clip();

    const std::string& name() const { return name_; }
    float duration() const;   // seconds
    int joint_count() const;  // (must be its skeleton's)

    struct Impl;
    const Impl& impl() const { return *impl_; }

private:
    Clip();
    std::unique_ptr<Impl> impl_;
    std::string name_;
};

// A skeleton's pose: sampled from a clip, or at rest, in model space.
class Pose {
public:
    explicit Pose(const Skeleton& skeleton);
    Pose(Pose&&) noexcept;
    Pose& operator=(Pose&&) noexcept;
    ~Pose();

    // The clip at `time` seconds (wrapped round its duration if `loop`, else held at its end).
    // Throws AnimationError if the clip isn't for this skeleton.
    void sample(const Clip& clip, float time, bool loop = true);
    void rest();
    // Each joint's transform relative to the model, in the skeleton's order.
    const std::vector<glm::mat4>& model() const { return model_; }

    // For blending (midgard/anim_graph.h): a clip at `time`, each joint relative to its parent
    // (the pose itself unchanged); the rest pose so; and a pose made from joints so.
    void sample_local(const Clip& clip, float time, bool loop, std::vector<JointTransform>& out);
    const std::vector<JointTransform>& rest_local() const { return rest_local_; }
    std::vector<JointTransform> local() const;  // the pose's own (for IK: midgard/ik.h)
    void set_local(std::span<const JointTransform> joints);
    // Moves the whole pose by `by` from where its joints put it (the motion a clip's root has,
    // kept out of it), until the next sample or rest; set_local keeps it.
    void shift(const glm::vec3& by);
    const glm::vec3& offset() const { return offset_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    const Skeleton* skeleton_;
    std::vector<glm::mat4> model_;
    std::vector<JointTransform> rest_local_;
    glm::vec3 offset_{0.0f};
    void to_model();
};

}  // namespace midgard
