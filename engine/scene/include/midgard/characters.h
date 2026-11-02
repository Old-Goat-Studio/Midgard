// SPDX-License-Identifier: Apache-2.0
// Characters (milestone 5.13): entities with a character component, animated and drawn skinned.
//
//   "character": {"model": "asset:characters/hero.glb",   a skinned glTF in the package
//                 "clips": "asset:characters/moves.glb",  where its clips come from (none: the
//                                                         model's own), by joint name
//                 "clip": "Walk_Loop", "speed": 1.0,      what it plays, and how fast
//                 "rig": {"head": "Helmet"},              its humanoid bones, where its joint
//                                                         names don't say (midgard/humanoid.h)
//                 "graph": {...},                         what it plays, decided as it plays
//                                                         (midgard/anim_graph.h; none: clip)
//                 "ik": {"feet": true, "look": "player",  its feet on the ground, what it looks
//                        "look_limit": 70},               at (midgard/ik.h; a humanoid's)
//                 "first_person": {"offset": [0, 0, 0]}}  the player's arms in first person:
//                                                         cut from its body and in its pose, or
//                                                         {"model", "clips", "graph", "eye"}: a
//                                                         rig of their own, kept in step
//
// Each model is read once: its skeleton, clips, inverse bind matrices and skinned mesh (on the
// GPU once uploaded). Clips from another file are retargeted to the model when both are
// humanoids (so they keep the model's proportions), else used by joint name. A character with
// a graph has its "speed" and "vertical_speed" set from how it moves; its graph's events and
// root motion are the game's to use after each update. Each update moves
// every character's clip on and samples its pose; each frame draws it skinned, with the entity's
// material.
#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "midgard/anim_graph.h"
#include "midgard/animation.h"
#include "midgard/humanoid.h"
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
    // The player's first-person arms, held at the camera (`camera`: the camera's own frame,
    // relative to the draws' origin), if it has them: they cast no shadow.
    void first_person_draws(const Scene& scene, entt::entity player, const glm::mat4& camera,
                            const MaterialCache& materials,
                            std::vector<render::DrawItem>& out) const;
    // Where they are drawn: their mesh, the model matrix and the skin's joint matrices (none:
    // the player has no arms yet).
    struct FirstPersonView {
        const render::MeshData* mesh = nullptr;
        glm::mat4 model{1.0f};
        std::span<const glm::mat4> skin;
        std::string state;  // its own graph's (first layer), if it has one
    };
    std::optional<FirstPersonView> first_person(const Scene& scene, entt::entity player,
                                                const glm::mat4& camera) const;
    // The ground for characters' feet: what a ray from a point along a direction meets first
    // (none: nothing within the distance), leaving out the character itself. Without it, feet
    // stay where their animation puts them.
    struct GroundHit {
        glm::dvec3 point{0.0};
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
    };
    using Ground = std::function<std::optional<GroundHit>(
        entt::entity self, const glm::dvec3& from, const glm::vec3& direction, double distance)>;
    void set_ground(Ground ground) { ground_ = std::move(ground); }
    // Who "look": "player" means.
    void set_player(entt::entity player) { player_ = player; }
    // Scripts': what a character looks at (a point, or an entity; neither: what its component
    // says), and where a hand reaches (none: nowhere), by a weight.
    void look_at(entt::entity e, std::optional<glm::dvec3> point, entt::entity target = entt::null);
    void reach(entt::entity e, bool right, std::optional<glm::dvec3> point, float weight = 1.0f);
    // Whether "speed" and "vertical_speed" are set from how characters move (else they are left
    // to whoever sets them: the editor's preview).
    void set_automatic(bool automatic) { automatic_ = automatic; }
    // A skinned mesh cut down to the triangles whose vertices all hang mostly from kept joints
    // (`keep`, by the mesh's joint indices): a body's arms, say. Its vertices are renumbered.
    static void cut_skinned(const render::MeshData& mesh, std::span<const glm::u16vec4> joints,
                            std::span<const glm::vec4> weights, const std::vector<bool>& keep,
                            render::MeshData& out_mesh, std::vector<glm::u16vec4>& out_joints,
                            std::vector<glm::vec4>& out_weights);
    // A scene was left: the characters' states go (entity handles are used again); the models
    // stay, for a scene coming back.
    void clear_states() { states_.clear(); }

    // For tests and tools: a character's pose (its skeleton's joints in model space), and the
    // clip it plays; none if it has none.
    const Pose* pose(entt::entity e) const;
    std::string clip_of(entt::entity e) const;

    // Its graph's parameters and triggers (scripts'): throw AnimationError if it has no graph,
    // or the graph no such parameter. Set before its graph starts, they wait for it.
    void set_parameter(entt::entity e, const std::string& name, float value);
    void trigger(entt::entity e, const std::string& name);
    float parameter(entt::entity e, const std::string& name) const;
    // Its clip, or graph, from the start (a graph's parameters back to their starting values).
    void restart(entt::entity e);
    // Where one of its humanoid bones is in the world (none: it hasn't that bone, or no pose).
    std::optional<glm::dvec3> bone_position(const Scene& scene, entt::entity e,
                                            HumanBone bone) const;
    // The state a layer of its graph is in ("" without one).
    std::string state_of(entt::entity e, size_t layer = 0) const;

    // From the last update: events the graphs passed, and the root motion of characters
    // whose graph has it (metres in each one's own frame, before its scale and rotation).
    struct Event {
        entt::entity entity;
        std::string name;
    };
    const std::vector<Event>& events() const { return events_; }
    const std::vector<std::pair<entt::entity, glm::vec3>>& motions() const { return motions_; }

private:
    struct Model {
        Skeleton skeleton;
        RawSkeleton raw;          // (for retargeting)
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
        std::map<std::string, std::string> rig;
        std::string graph;
        std::unique_ptr<AnimPlayer> player;
        std::map<std::string, float> waiting;  // set before the graph started
        std::set<std::string> triggered;
        glm::dvec3 last{0.0};  // where it was (for its speed)
        bool placed = false;
        float speed = 0.0f, vertical_speed = 0.0f;
        // Inverse kinematics: its humanoid bones (in its skeleton's order; -1: none), feet's
        // offsets from the ground and the hips' drop (smoothed), what it looks at and reaches.
        std::array<int, kHumanBones> bones{};
        bool bones_known = false;
        float foot_offset[2] = {0.0f, 0.0f}, drop = 0.0f, look_weight = 0.0f;
        std::optional<glm::dvec3> look_point;
        entt::entity look_entity = entt::null;
        struct Reach {
            std::optional<glm::dvec3> point;
            float weight = 1.0f, blend = 0.0f;
        } reach[2];
        // First-person arms (the player's).
        struct FirstPerson {
            std::string key;  // (its settings: made again when they change)
            std::shared_ptr<Model> m;
            bool own = false;  // its own rig (else cut from the body, in the body's pose)
            std::unique_ptr<Pose> pose;
            std::unique_ptr<AnimPlayer> player;
            std::vector<glm::mat4> palette;
            render::MeshData mesh;
            std::vector<glm::u16vec4> joints;
            std::vector<glm::vec4> weights;
            render::MeshId gpu = 0;
            bool uploaded = false;
            int eye = 0;  // in its skeleton
            float time = 0.0f;
        };
        std::unique_ptr<FirstPerson> fp;
        const Clip* playing = nullptr;
        std::shared_ptr<Model> m;
        std::unique_ptr<Pose> pose;
        float time = 0.0f;
        std::vector<glm::mat4> palette;  // the skin's joint matrices (draws point at them)
    };
    const Clip* find_clip(const State& s);
    const Clip* find_clip(const State& s, const std::string& name) const;
    void start_graph(const CharacterRef& c, State& s);
    void find_bones(const CharacterRef& c, State& s) const;
    void update_first_person(const CharacterRef& c, State& s, float dt);
    const std::vector<Clip>* clips_for(const std::string& clips, const std::string& model,
                                       const std::map<std::string, std::string>& rig) const;
    void apply_ik(const Scene& scene, entt::entity e, const CharacterRef& c, State& s, float dt);
    std::optional<glm::dvec3> look_target(const Scene& scene, const CharacterRef& c,
                                          const State& s) const;
    State& graph_state(entt::entity e);

    std::map<std::string, std::shared_ptr<Model>> models_;  // by content id
    // Clips from another file, built for a model's skeleton: by (clips, model, its rig).
    using LibraryKey = std::tuple<std::string, std::string, std::map<std::string, std::string>>;
    std::map<LibraryKey, std::vector<Clip>> libraries_;
    std::map<std::string, std::string> failed_;  // content id -> why
    std::unordered_map<entt::entity, State> states_;
    std::map<std::string, std::shared_ptr<const AnimGraph>> graphs_;  // by JSON text
    std::vector<Event> events_;
    bool automatic_ = true;
    Ground ground_;
    entt::entity player_ = entt::null;
    std::vector<std::pair<entt::entity, glm::vec3>> motions_;
};

}  // namespace midgard
