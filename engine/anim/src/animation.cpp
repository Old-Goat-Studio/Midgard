// SPDX-License-Identifier: Apache-2.0
#include "midgard/animation.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>

namespace midgard {

namespace {

ozz::math::Float3 to_ozz(const glm::vec3& v) {
    return {v.x, v.y, v.z};
}
ozz::math::Quaternion to_ozz(const glm::quat& q) {
    return {q.x, q.y, q.z, q.w};
}

template <class T>
std::vector<std::uint8_t> save_object(const T& object) {
    ozz::io::MemoryStream stream;
    ozz::io::OArchive archive(&stream);
    archive << object;
    std::vector<std::uint8_t> out(stream.Size());
    stream.Seek(0, ozz::io::Stream::kSet);
    stream.Read(out.data(), out.size());
    return out;
}

template <class T>
void load_object(std::span<const std::uint8_t> bytes, T& object, const char* what) {
    ozz::io::MemoryStream stream;
    stream.Write(bytes.data(), bytes.size());
    stream.Seek(0, ozz::io::Stream::kSet);
    ozz::io::IArchive archive(&stream);
    if (!archive.TestTag<T>()) throw AnimationError(std::string("not a ") + what);
    archive >> object;
}

// Joints four to a SoA transform, and back.
void unpack(std::span<const ozz::math::SoaTransform> soa, int count,
            std::vector<JointTransform>& out) {
    out.resize(size_t(count));
    for (size_t s = 0; s < soa.size(); ++s) {
        float v[10][4];
        const ozz::math::SoaTransform& t = soa[s];
        ozz::math::StorePtrU(t.translation.x, v[0]);
        ozz::math::StorePtrU(t.translation.y, v[1]);
        ozz::math::StorePtrU(t.translation.z, v[2]);
        ozz::math::StorePtrU(t.rotation.x, v[3]);
        ozz::math::StorePtrU(t.rotation.y, v[4]);
        ozz::math::StorePtrU(t.rotation.z, v[5]);
        ozz::math::StorePtrU(t.rotation.w, v[6]);
        ozz::math::StorePtrU(t.scale.x, v[7]);
        ozz::math::StorePtrU(t.scale.y, v[8]);
        ozz::math::StorePtrU(t.scale.z, v[9]);
        for (int lane = 0; lane < 4; ++lane) {
            const size_t j = s * 4 + size_t(lane);
            if (j >= out.size()) break;
            out[j] = {{v[0][lane], v[1][lane], v[2][lane]},
                      glm::quat(v[6][lane], v[3][lane], v[4][lane], v[5][lane]),
                      {v[7][lane], v[8][lane], v[9][lane]}};
        }
    }
}

void pack(std::span<const JointTransform> joints, std::vector<ozz::math::SoaTransform>& soa) {
    const auto lane = [&](size_t s, int l) -> const JointTransform& {
        const size_t j = s * 4 + size_t(l);
        static const JointTransform kIdentity{};
        return j < joints.size() ? joints[j] : kIdentity;
    };
    for (size_t s = 0; s < soa.size(); ++s) {
        const JointTransform &a = lane(s, 0), &b = lane(s, 1), &c = lane(s, 2), &d = lane(s, 3);
        using ozz::math::simd_float4::Load;
        soa[s].translation = {
            Load(a.translation.x, b.translation.x, c.translation.x, d.translation.x),
            Load(a.translation.y, b.translation.y, c.translation.y, d.translation.y),
            Load(a.translation.z, b.translation.z, c.translation.z, d.translation.z)};
        soa[s].rotation = {Load(a.rotation.x, b.rotation.x, c.rotation.x, d.rotation.x),
                           Load(a.rotation.y, b.rotation.y, c.rotation.y, d.rotation.y),
                           Load(a.rotation.z, b.rotation.z, c.rotation.z, d.rotation.z),
                           Load(a.rotation.w, b.rotation.w, c.rotation.w, d.rotation.w)};
        soa[s].scale = {Load(a.scale.x, b.scale.x, c.scale.x, d.scale.x),
                        Load(a.scale.y, b.scale.y, c.scale.y, d.scale.y),
                        Load(a.scale.z, b.scale.z, c.scale.z, d.scale.z)};
    }
}

}  // namespace

// ---- skeletons ----------------------------------------------------------------------------

struct Skeleton::Impl {
    ozz::animation::Skeleton skeleton;
};

Skeleton::Skeleton() : impl_(std::make_unique<Impl>()) {}
Skeleton::Skeleton(Skeleton&&) noexcept = default;
Skeleton& Skeleton::operator=(Skeleton&&) noexcept = default;
Skeleton::~Skeleton() = default;

namespace {

void read_back(const ozz::animation::Skeleton& s, std::vector<std::string>& names,
               std::vector<int>& parents) {
    names.assign(s.joint_names().begin(), s.joint_names().end());
    parents.assign(s.joint_parents().begin(), s.joint_parents().end());
}

}  // namespace

Skeleton Skeleton::build(const RawSkeleton& raw) {
    const size_t n = raw.names.size();
    if (n == 0) throw AnimationError("a skeleton needs joints");
    if (raw.parents.size() != n || raw.rest.size() != n)
        throw AnimationError("a skeleton's names, parents and rest poses must match");
    std::map<std::string, size_t> seen;
    for (size_t i = 0; i < n; ++i) {
        if (!seen.emplace(raw.names[i], i).second)
            throw AnimationError("joint '" + raw.names[i] + "' is named twice");
        if (raw.parents[i] < -1 || raw.parents[i] >= int(n) || raw.parents[i] == int(i))
            throw AnimationError("joint '" + raw.names[i] + "' has no such parent");
    }
    // The tree, built from each root down (a joint never reached is in a loop).
    std::vector<std::vector<size_t>> children(n);
    std::vector<size_t> roots;
    for (size_t i = 0; i < n; ++i)
        (raw.parents[i] < 0 ? roots : children[size_t(raw.parents[i])]).push_back(i);
    size_t reached = 0;
    const std::function<void(size_t, ozz::animation::offline::RawSkeleton::Joint&)> fill =
        [&](size_t i, ozz::animation::offline::RawSkeleton::Joint& j) {
            ++reached;
            j.name = raw.names[i].c_str();
            j.transform.translation = to_ozz(raw.rest[i].translation);
            j.transform.rotation = to_ozz(glm::normalize(raw.rest[i].rotation));
            j.transform.scale = to_ozz(raw.rest[i].scale);
            j.children.resize(children[i].size());
            for (size_t c = 0; c < children[i].size(); ++c) fill(children[i][c], j.children[c]);
        };
    ozz::animation::offline::RawSkeleton tree;
    tree.roots.resize(roots.size());
    for (size_t r = 0; r < roots.size(); ++r) fill(roots[r], tree.roots[r]);
    if (reached != n) throw AnimationError("a skeleton's parents make a loop");
    if (!tree.Validate()) throw AnimationError("the skeleton is not valid");
    const auto built = ozz::animation::offline::SkeletonBuilder()(tree);
    if (!built) throw AnimationError("the skeleton could not be built (too many joints?)");
    Skeleton out;
    out.impl_->skeleton = std::move(*built);
    read_back(out.impl_->skeleton, out.names_, out.parents_);
    return out;
}

Skeleton Skeleton::load(std::span<const std::uint8_t> bytes) {
    Skeleton out;
    load_object(bytes, out.impl_->skeleton, "skeleton");
    read_back(out.impl_->skeleton, out.names_, out.parents_);
    return out;
}

std::vector<std::uint8_t> Skeleton::save() const {
    return save_object(impl_->skeleton);
}

int Skeleton::joint_count() const {
    return impl_->skeleton.num_joints();
}

int Skeleton::find(const std::string& name) const {
    const auto it = std::find(names_.begin(), names_.end(), name);
    return it == names_.end() ? -1 : int(it - names_.begin());
}

// ---- clips --------------------------------------------------------------------------------

struct Clip::Impl {
    ozz::animation::Animation animation;
};

Clip::Clip() : impl_(std::make_unique<Impl>()) {}
Clip::Clip(Clip&&) noexcept = default;
Clip& Clip::operator=(Clip&&) noexcept = default;
Clip::~Clip() = default;

Clip Clip::build(const Skeleton& skeleton, const RawClip& raw) {
    if (!(raw.duration > 0.0f)) throw AnimationError("clip '" + raw.name + "' has no length");
    ozz::animation::offline::RawAnimation a;
    a.duration = raw.duration;
    a.name = raw.name.c_str();
    a.tracks.resize(size_t(skeleton.joint_count()));
    // Joints without a track keep their rest pose (a key at the start).
    const ozz::animation::Skeleton& s = skeleton.impl().skeleton;
    for (int j = 0; j < s.num_joints(); ++j) {
        const ozz::math::SoaTransform& soa = s.joint_rest_poses()[size_t(j / 4)];
        const int lane = j % 4;
        float t[4][4];
        ozz::math::StorePtrU(soa.translation.x, t[0]);
        ozz::math::StorePtrU(soa.translation.y, t[1]);
        ozz::math::StorePtrU(soa.translation.z, t[2]);
        float r[4][4];
        ozz::math::StorePtrU(soa.rotation.x, r[0]);
        ozz::math::StorePtrU(soa.rotation.y, r[1]);
        ozz::math::StorePtrU(soa.rotation.z, r[2]);
        ozz::math::StorePtrU(soa.rotation.w, r[3]);
        float sc[3][4];
        ozz::math::StorePtrU(soa.scale.x, sc[0]);
        ozz::math::StorePtrU(soa.scale.y, sc[1]);
        ozz::math::StorePtrU(soa.scale.z, sc[2]);
        auto& track = a.tracks[size_t(j)];
        track.translations.push_back({0.0f, {t[0][lane], t[1][lane], t[2][lane]}});
        track.rotations.push_back({0.0f, {r[0][lane], r[1][lane], r[2][lane], r[3][lane]}});
        track.scales.push_back({0.0f, {sc[0][lane], sc[1][lane], sc[2][lane]}});
    }
    for (const RawTrack& rt : raw.tracks) {
        const int j = skeleton.find(rt.joint);
        if (j < 0) continue;
        auto& track = a.tracks[size_t(j)];
        if (!rt.translations.empty()) {
            track.translations.clear();
            for (const auto& k : rt.translations)
                track.translations.push_back(
                    {std::clamp(k.time, 0.0f, raw.duration), to_ozz(k.value)});
        }
        if (!rt.rotations.empty()) {
            track.rotations.clear();
            for (const auto& k : rt.rotations)
                track.rotations.push_back(
                    {std::clamp(k.time, 0.0f, raw.duration), to_ozz(glm::normalize(k.value))});
        }
        if (!rt.scales.empty()) {
            track.scales.clear();
            for (const auto& k : rt.scales)
                track.scales.push_back({std::clamp(k.time, 0.0f, raw.duration), to_ozz(k.value)});
        }
    }
    if (!a.Validate()) throw AnimationError("clip '" + raw.name + "': its keys are out of order");
    const auto built = ozz::animation::offline::AnimationBuilder()(a);
    if (!built) throw AnimationError("clip '" + raw.name + "' could not be built");
    Clip out;
    out.impl_->animation = std::move(*built);
    out.name_ = raw.name;
    return out;
}

Clip Clip::load(std::span<const std::uint8_t> bytes) {
    Clip out;
    load_object(bytes, out.impl_->animation, "clip");
    out.name_ = out.impl_->animation.name();
    return out;
}

std::vector<std::uint8_t> Clip::save() const {
    return save_object(impl_->animation);
}

float Clip::duration() const {
    return impl_->animation.duration();
}

int Clip::joint_count() const {
    return impl_->animation.num_tracks();
}

// ---- poses --------------------------------------------------------------------------------

struct Pose::Impl {
    ozz::animation::SamplingJob::Context context;
    // sample_local's: a context for each clip (they keep where the last sample was).
    std::map<const ozz::animation::Animation*,
             std::unique_ptr<ozz::animation::SamplingJob::Context>>
        contexts;
    std::vector<ozz::math::SoaTransform> scratch;
    std::vector<ozz::math::SoaTransform> locals;
    std::vector<ozz::math::Float4x4> models;
};

Pose::Pose(const Skeleton& skeleton) : impl_(std::make_unique<Impl>()), skeleton_(&skeleton) {
    const ozz::animation::Skeleton& s = skeleton.impl().skeleton;
    impl_->context.Resize(s.num_joints());
    impl_->locals.resize(size_t(s.num_soa_joints()));
    impl_->models.resize(size_t(s.num_joints()));
    impl_->scratch.resize(size_t(s.num_soa_joints()));
    rest();
    unpack(impl_->locals, s.num_joints(), rest_local_);
}
Pose::Pose(Pose&&) noexcept = default;
Pose& Pose::operator=(Pose&&) noexcept = default;
Pose::~Pose() = default;

void Pose::rest() {
    offset_ = glm::vec3(0.0f);
    const auto rest = skeleton_->impl().skeleton.joint_rest_poses();
    std::copy(rest.begin(), rest.end(), impl_->locals.begin());
    to_model();
}

void Pose::sample(const Clip& clip, float time, bool loop) {
    offset_ = glm::vec3(0.0f);
    if (clip.joint_count() != skeleton_->joint_count())
        throw AnimationError("clip '" + clip.name() + "' is for another skeleton");
    const float d = clip.duration();
    float ratio = d > 0.0f ? time / d : 0.0f;
    ratio = loop ? ratio - std::floor(ratio) : std::clamp(ratio, 0.0f, 1.0f);
    ozz::animation::SamplingJob job;
    job.animation = &clip.impl().animation;
    job.context = &impl_->context;
    job.ratio = ratio;
    job.output = ozz::make_span(impl_->locals);
    if (!job.Run()) throw AnimationError("clip '" + clip.name() + "' could not be sampled");
    to_model();
}

void Pose::sample_local(const Clip& clip, float time, bool loop, std::vector<JointTransform>& out) {
    if (clip.joint_count() != skeleton_->joint_count())
        throw AnimationError("clip '" + clip.name() + "' is for another skeleton");
    auto& context = impl_->contexts[&clip.impl().animation];
    if (!context) {
        if (impl_->contexts.size() > 64) {  // (clips come and go: keep a few)
            impl_->contexts.clear();
            return sample_local(clip, time, loop, out);
        }
        context = std::make_unique<ozz::animation::SamplingJob::Context>(skeleton_->joint_count());
    }
    const float d = clip.duration();
    float ratio = d > 0.0f ? time / d : 0.0f;
    ratio = loop ? ratio - std::floor(ratio) : std::clamp(ratio, 0.0f, 1.0f);
    ozz::animation::SamplingJob job;
    job.animation = &clip.impl().animation;
    job.context = context.get();
    job.ratio = ratio;
    job.output = ozz::make_span(impl_->scratch);
    if (!job.Run()) throw AnimationError("clip '" + clip.name() + "' could not be sampled");
    unpack(impl_->scratch, skeleton_->joint_count(), out);
}

std::vector<JointTransform> Pose::local() const {
    std::vector<JointTransform> out;
    unpack(impl_->locals, skeleton_->joint_count(), out);
    return out;
}

void Pose::set_local(std::span<const JointTransform> joints) {
    if (int(joints.size()) != skeleton_->joint_count())
        throw AnimationError("a pose needs a transform for each joint");
    pack(joints, impl_->locals);
    to_model();
}

void Pose::shift(const glm::vec3& by) {
    const glm::vec3 change = by - offset_;
    offset_ = by;
    for (glm::mat4& m : model_) m[3] += glm::vec4(change, 0.0f);
}

void Pose::to_model() {
    ozz::animation::LocalToModelJob job;
    job.skeleton = &skeleton_->impl().skeleton;
    job.input = ozz::make_span(impl_->locals);
    job.output = ozz::make_span(impl_->models);
    if (!job.Run()) throw AnimationError("the pose could not be made");
    model_.resize(impl_->models.size());
    for (size_t i = 0; i < impl_->models.size(); ++i) {
        for (int c = 0; c < 4; ++c)
            ozz::math::StorePtrU(impl_->models[i].cols[c], &model_[i][c][0]);
        model_[i][3] += glm::vec4(offset_, 0.0f);
    }
}

}  // namespace midgard
