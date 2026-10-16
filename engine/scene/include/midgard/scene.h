// SPDX-License-Identifier: Apache-2.0
// A scene built with EnTT from a package database's scene and entity tables. The component
// shapes are defined in schema/components.md.
#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

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
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f};  // Euler degrees, R = Ry * Rx * Rz
    glm::vec3 scale{1.0f};
};

struct MeshRef {
    std::string asset;  // content id in the manifest
};

struct Material {
    glm::vec4 color{1.0f};
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

// The entity the player controls. The first camera follows it in third person.
struct Player {
    float speed = 4.0f;  // metres per second
};

// A Python module in the package's scripts/ folder that drives this entity ("door" is
// scripts/door.py, "enemies.wolf" is scripts/enemies/wolf.py).
struct ScriptRef {
    std::string module;
};

// A box around the entity (size in its local units, so the entity's scale applies) that reports
// when the player enters or leaves it.
struct Trigger {
    glm::vec3 size{1.0f};
};

struct SceneSettings {
    glm::vec3 sky{0.08f, 0.11f, 0.18f};
    glm::vec3 ambient{0.18f, 0.2f, 0.26f};
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

    glm::mat4 local_matrix(entt::entity e) const;
    glm::mat4 world_matrix(entt::entity e) const;  // includes parents

    // The first camera (by entity id order), as view + projection for the given aspect ratio.
    std::optional<render::Camera> camera(float aspect) const;
    // The first directional light, combined with the scene's ambient colour.
    render::Light light() const;

    // The first Sky entity's colours become the scene's (after its settings were read again).
    void use_sky();

private:
    // A Sky component on e becomes the scene's sky and ambient colours.
    void apply_sky(entt::entity e);
};

glm::mat4 rotation_matrix(const glm::vec3& euler_degrees);

}  // namespace midgard
