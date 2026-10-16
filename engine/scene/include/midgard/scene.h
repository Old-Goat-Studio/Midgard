// SPDX-License-Identifier: Apache-2.0
// A scene built with EnTT from a package database's scene and entity tables. The component
// shapes are defined in schema/components.md.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include "midgard/body.h"
#include "midgard/physics.h"
#include "midgard/render/renderer.h"

namespace midgard {

class SceneError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ---- components -------------------------------------------------------------------------
struct EntityInfo {
    std::string id;
    std::string name;
    entt::entity parent = entt::null;
};

struct Transform {
    glm::dvec3 position{0.0};  // metres, double precision (worlds are tens of kilometres across)
    glm::vec3 rotation{0.0f};  // Euler degrees, R = Ry * Rx * Rz
    glm::vec3 scale{1.0f};
};

struct MeshRef {
    std::string asset;  // content id in the manifest
};

// glTF's metal/roughness factors (textures arrive with the object pipeline).
struct Material {
    glm::vec4 color{1.0f};  // base colour, sRGB, and alpha
    float metallic = 0.0f;
    float roughness = 0.8f;
    glm::vec3 emissive{0.0f};  // sRGB
    float emissive_strength = 1.0f;
};

struct CameraLens {
    float fov = 55.0f;  // vertical, degrees
    float near_plane = 0.05f;
    float far_plane = 500.0f;
};

struct DirectionalLight {
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
};

// A point or spot light (a spot shines along the entity's local -Z).
struct LocalLightSource {
    bool spot = false;
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
    float range = 10.0f;
    float inner_angle = 20.0f, outer_angle = 30.0f;  // degrees
};

// The entity the player controls: a character in the physics world (an upright capsule centred
// on the entity) that walks on the ground. The first camera follows it in third person.
struct Player {
    float speed = 4.0f;        // metres per second
    float radius = 0.3f;       // the capsule
    float height = 1.0f;       // end to end, along "up"
    float max_slope = 50.0f;   // degrees: steeper ground is a wall
    float step_height = 0.3f;  // steps up to this high are walked up
    float jump = 4.5f;         // metres per second upwards (about 1 m high); 0: no jumping
};

// A Python module in the package's scripts/ folder that drives this entity ("door" is
// scripts/door.py, "enemies.wolf" is scripts/enemies/wolf.py).
struct ScriptRef {
    std::string module;
};

// A box around the entity (size in its local units, so the entity's scale applies) that reports
// when the player enters or leaves it: a sensor in the physics world, so nothing bumps into
// it. It moves with the entity, and is separate from any collider the entity has.
struct Trigger {
    glm::vec3 size{1.0f};
};

// How the entity moves in the physics world. An entity with a collider and no rigid body is
// static scenery.
struct RigidBody {
    physics::Motion motion = physics::Motion::Dynamic;
    float mass = 0.0f;  // kilograms; 0: from the collider's volume at the density of water
    int layer = 0;      // the project's collision layer (0 to 15)
};

// The entity's shape for physics, in its local units (its scale applies). Convex and mesh
// colliders take the entity's own mesh unless `mesh` names another.
struct ColliderShape {
    physics::Collider::Kind kind = physics::Collider::Kind::Box;
    glm::vec3 size{1.0f};
    float radius = 0.5f;
    float height = 2.0f;
    glm::vec3 offset{0.0f};
    std::string mesh;  // a content id, for convex and mesh colliders
    // Heightfield: samples x samples heights, `spacing` metres apart.
    std::uint32_t samples = 0;
    std::vector<float> heights;
    float spacing = 1.0f;
};

// A group: an entity whose children are the parts of one thing. It has its own transform (its
// centre, which the editor keeps evenly scaled), so moving it moves them all. With a dynamic or
// kinematic rigid body, its parts' colliders make one compound body that moves as a whole.
struct Group {};

struct PhysicsMaterial {
    float friction = 0.5f;
    float bounciness = 0.0f;
};

struct SceneSettings {
    glm::vec3 sky{0.08f, 0.11f, 0.18f};
    glm::vec3 ambient{0.18f, 0.2f, 0.26f};
    // The world's shape: where "up" is and how gravity pulls (midgard/body.h). None: flat
    // ground, +Y up, gravity 9.81 m/s^2 down.
    std::optional<Body> body;
};

// The scene's sky: an entity with this component sets the scene's sky and ambient colours
// (the scene table's settings are only the fallback, for scenes without one).
struct Sky {
    glm::vec3 color = SceneSettings{}.sky;
    glm::vec3 ambient = SceneSettings{}.ambient;
};

// One row of the entity table.
struct EntityRow {
    std::string id;
    std::string name;
    std::string parent;           // empty: no parent
    std::string components_json;  // the components column
};

// ---- the scene --------------------------------------------------------------------------
class Scene {
public:
    // Reads the scene and its entities from a package database (read-only). Throws SceneError.
    static Scene load(const std::filesystem::path& database, const std::string& scene_id);

    // Hot reload: one scene's settings, or one entity row (nothing if it was deleted), read
    // again from the package database. Throw SceneError.
    static SceneSettings read_settings(const std::filesystem::path& database,
                                       const std::string& scene_id);
    static std::optional<EntityRow> read_entity(const std::filesystem::path& database,
                                                const std::string& entity_id);

    entt::registry registry;
    SceneSettings settings;
    std::string id;

    // The entity with this id, or entt::null.
    entt::entity find(const std::string& entity_id) const;

    // Live editing. put_entity adds an entity or replaces all of its components, keeping its
    // handle; `components` has the shape of the entity table's components column. A component
    // value of null in set_component removes it. Throw SceneError on malformed components.
    entt::entity put_entity(const std::string& entity_id, const std::string& name,
                            const std::string& parent_id, const nlohmann::json& components);
    void set_component(entt::entity e, const std::string& component, const nlohmann::json& value);
    bool remove_entity(const std::string& entity_id);

    glm::dmat4 local_matrix(entt::entity e) const;
    glm::dmat4 world_matrix(entt::entity e) const;  // includes parents
    glm::dvec3 world_position(entt::entity e) const;
    // The entity's model matrix for drawing, relative to a camera's origin.
    glm::mat4 draw_matrix(entt::entity e, const glm::dvec3& origin) const;
    // How to draw the entity with this mesh: its matrix relative to `origin` and its material
    // (a plain light grey without one). The game and the editor's viewport share it.
    render::DrawItem draw_item(entt::entity e, render::MeshId mesh, const glm::dvec3& origin) const;

    // The first camera (by entity id order), for the given aspect ratio: its origin is its world
    // position and its view only turns (camera-relative rendering, see render::Camera).
    std::optional<render::Camera> camera(float aspect) const;
    // The lighting: the first directional light, the ambient colours, and the point and spot
    // lights nearest `origin` (the camera's), relative to it. "Up" is the world shape's there.
    render::Light light(const glm::dvec3& origin = glm::dvec3(0.0)) const;

    // The first Sky entity's colours become the scene's (after its settings were read again).
    void use_sky();

private:
    // A Sky component on e becomes the scene's sky and ambient colours.
    void apply_sky(entt::entity e);
};

glm::mat4 rotation_matrix(const glm::vec3& euler_degrees);

}  // namespace midgard
