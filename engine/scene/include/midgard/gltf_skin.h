// SPDX-License-Identifier: Apache-2.0
// Skinned glTF models (milestone 5.13): a character's skeleton (the joints of its first skin),
// the clips that move them, and its skinned meshes, as midgard/animation.h builds skeletons
// and clips from.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include "midgard/animation.h"
#include "midgard/render/mesh_data.h"

namespace midgard {

struct SkinnedModel {
    // The skin's joints, in the skin's order (which the meshes' joint indices use), with each
    // one's inverse bind matrix.
    RawSkeleton skeleton;
    std::vector<glm::mat4> inverse_binds;
    // Above the skeleton: the transform of the node its roots hang from, in the model.
    glm::mat4 armature{1.0f};
    std::vector<RawClip> clips;
    // Every skinned triangle, in its bind pose, with four joints (by skin index) and their
    // weights (adding to 1) a vertex. Unskinned meshes in the file are left out.
    render::MeshData mesh;
    std::vector<glm::u16vec4> joints;
    std::vector<glm::vec4> weights;
};

// A .glb (or a .gltf whose buffers are embedded). Throws AssetError: not glTF, no skin, a
// clip that can't be read.
SkinnedModel load_skinned_glb(std::span<const std::uint8_t> bytes);
// A .gltf beside its .bin files (or a .glb).
SkinnedModel load_skinned_gltf_file(const std::filesystem::path& file);

// The model's skeleton and clips, built (skeleton joints are matched to the skin's by name).
// Throws AnimationError.
struct BuiltAnimation {
    Skeleton skeleton;
    std::vector<Clip> clips;
    // For each skin joint, its index in `skeleton` (skinning: model[skin_to_skeleton[j]] times
    // inverse_binds[j]).
    std::vector<int> skin_to_skeleton;
};
BuiltAnimation build_animation(const SkinnedModel& model);

}  // namespace midgard
