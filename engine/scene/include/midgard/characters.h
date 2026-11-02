// SPDX-License-Identifier: Apache-2.0
// Characters (milestone 5.13): entities with a character component, animated and drawn skinned.
//
//   "character": {"model": "asset:characters/hero.glb",   a skinned glTF in the package
//                 "clips": "asset:characters/moves.glb",  where its clips come from (none: the
//                                                         model's own), by joint name
//                 "clip": "Walk_Loop", "speed": 1.0}      what it plays, and how fast
//
// Each model is read once: its skeleton, clips, inverse bind matrices and skinned mesh (on the
// GPU once uploaded). Each update moves every character's clip on and samples its pose; each
// frame draws it skinned, with the entity's material.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "midgard/animation.h"
#include "midgard/materials.h"
#include "midgard/render/renderer.h"
#include "midgard/scene.h"

namespace midgard {

class Characters {
public:
    // A content id's bytes (from game.pak); throws if it isn't there.
    using Bytes = std::function<std::vector<std::uint8_t>(const std::string& asset)>;

    // Reads the models and clips the scene's characters name that aren't read yet. Returns what
    // went wrong (each once): a missing file, one that isn't skinned, a clip it doesn't have.
    std::vector<std::string> load(const Scene& scene, const Bytes& bytes);
    // The skinned meshes of models read but not on the GPU yet.
    void upload(render::Renderer& renderer);
    // The renderer went (a viewport detached): every mesh is uploaded again to the next one.
    void renderer_gone();
    // A file changed (the editor saved it again): models and clips read from it go, and are
    // read again when next asked for.
    void forget(const std::string& asset);
    // Every character's clip moved on by dt (times its speed), and its pose sampled.
    void update(const Scene& scene, float dt);
    // Their draws (characters whose model is on the GPU), except `hidden`.
    void draws(const Scene& scene, const glm::dvec3& origin, const MaterialCache& materials,
               entt::entity hidden, std::vector<render::DrawItem>& out) const;
    // A scene was left: the characters' states go (entity handles are used again); the models
    // stay, for a scene coming back.
    void clear_states() { states_.clear(); }

    // For tests and tools: a character's pose (its skeleton's joints in model space), and the
    // clip it plays; none if it has none.
    const Pose* pose(entt::entity e) const;
    std::string clip_of(entt::entity e) const;

private:
    struct Model {
        Skeleton skeleton;
        std::vector<Clip> clips;  // its own
        std::vector<int> skin_to_skeleton;
        std::vector<glm::mat4> inverse_binds;
        glm::mat4 armature{1.0f};
        render::MeshData mesh;  // (kept: a viewport's renderer comes and goes)
        std::vector<glm::u16vec4> joints;
        std::vector<glm::vec4> weights;
        render::MeshId gpu = 0;
        bool uploaded = false;
    };
    struct State {
        std::string model, clips, clip;
        const Clip* playing = nullptr;
        std::shared_ptr<Model> m;
        std::unique_ptr<Pose> pose;
        float time = 0.0f;
        std::vector<glm::mat4> palette;  // the skin's joint matrices (draws point at them)
    };
    const Clip* find_clip(const State& s);

    std::map<std::string, std::shared_ptr<Model>> models_;  // by content id
    // Clips from another file, built for a model's skeleton: by (clips, model).
    std::map<std::pair<std::string, std::string>, std::vector<Clip>> libraries_;
    std::map<std::string, std::string> failed_;  // content id -> why
    std::unordered_map<entt::entity, State> states_;
};

}  // namespace midgard
