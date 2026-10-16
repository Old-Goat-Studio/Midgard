// SPDX-License-Identifier: Apache-2.0
// A scene's physics: a body in a physics::World for every entity with a collider (static
// scenery, or moved as its rigid body says), and a sensor for every trigger (moving with its
// entity), kept in step with the entities. Before each step, entities the game or a script moved
// carry their bodies with them; after it, dynamic bodies move their entities.
#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "midgard/physics.h"
#include "midgard/render/mesh_data.h"
#include "midgard/scene.h"

namespace midgard {

// The project's collision layers (the physics_layer and physics_ignore tables, migration 5):
// their names, and the pairs that pass through each other.
struct PhysicsLayers {
    std::array<std::string, physics::kLayers> names;
    std::vector<std::pair<int, int>> ignored;

    // Packages older than migration 5 have one layer, "default", that collides with itself.
    static PhysicsLayers load(const std::filesystem::path& database);  // throws SceneError
};

class ScenePhysics {
public:
    using MeshLoader = std::function<render::MeshData(const std::string& asset)>;

    // Builds a body for every entity that needs one. Throws SceneError naming the entity whose
    // collider can't be built (a missing mesh, a malformed shape).
    ScenePhysics(const Scene& scene, const PhysicsLayers& layers, MeshLoader load_mesh);

    // After an edit: the entity's body is built again (or removed, if it no longer needs one).
    // Throws SceneError like the constructor.
    void rebuild(const Scene& scene, entt::entity e);
    void remove(entt::entity e);

    // What the player wants this frame (from the third-person camera); a jump waits for the
    // next physics step.
    void set_player_intent(const glm::vec3& walk, bool jump);

    // One frame: bodies follow entities that moved, the world steps (the player's character
    // walking), dynamic bodies and the character move their entities. Returns the number of
    // physics steps taken.
    int update(Scene& scene, double dt);

    // The triggers the player overlaps now, in entity order.
    std::vector<entt::entity> triggers_touching(const Scene& scene, entt::entity player) const;

    physics::World& world() { return *world_; }
    const physics::World& world() const { return *world_; }
    physics::BodyId body(entt::entity e) const;  // the solid body, else the trigger's
    // The player's character (the scene's first player), or nullptr.
    physics::Character* character() { return character_.get(); }
    entt::entity player() const { return player_; }

private:
    struct Placed {
        glm::dvec3 position;
        glm::quat rotation;
        glm::vec3 scale;
    };
    struct Tracked {
        physics::BodyId id;      // the solid body (its collider), or kNoBody
        physics::BodyId sensor;  // its trigger, or kNoBody
        physics::Motion motion;
        Placed placed;  // where the entity was when the body last matched it
    };
    void add(const Scene& scene, entt::entity e);
    void make_character(const Scene& scene);
    void write_back(Scene& scene, entt::entity e, const glm::dvec3& position,
                    const glm::quat* rotation);
    Placed placement(const Scene& scene, entt::entity e) const;
    physics::Collider collider_for(const Scene& scene, entt::entity e) const;

    std::unique_ptr<physics::World> world_;
    MeshLoader load_mesh_;
    std::unordered_map<entt::entity, Tracked> bodies_;
    // After the world: a character's capsule is a body in it, removed first.
    std::unique_ptr<physics::Character> character_;
    entt::entity player_ = entt::null;
    Placed player_placed_{};
    glm::vec3 walk_{0.0f};
    bool jump_ = false;
};

// Euler degrees (pitch, yaw, roll) for a rotation, as Transform keeps them (R = Ry * Rx * Rz).
glm::vec3 euler_degrees(const glm::quat& rotation);

}  // namespace midgard
