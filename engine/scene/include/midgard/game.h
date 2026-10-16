// SPDX-License-Identifier: Apache-2.0
// A loaded game: package + game.pak + entry scene, with meshes uploaded to the renderer.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "midgard/input.h"
#include "midgard/package.h"
#include "midgard/pak.h"
#include "midgard/render/renderer.h"
#include "midgard/scene.h"
#include "midgard/scene_physics.h"
#include "midgard/third_person.h"

namespace midgard {

// Mesh asset ids that are built into the engine rather than stored in game.pak.
inline constexpr const char* kBuiltinCube = "builtin:cube";

// The player entered or left a trigger this frame.
struct TriggerEvent {
    entt::entity trigger = entt::null;
    entt::entity other = entt::null;  // the player
    bool entered = true;              // false: left
};

class Game {
public:
    // Opens <dir>/manifest.json, its database and (if present) game.pak, and loads the entry
    // scene. Throws PackageError, ManifestError, SceneError or AssetError.
    explicit Game(const std::filesystem::path& package_dir);

    const PackageInfo& package() const { return package_; }
    Scene& scene() { return scene_; }
    const Scene& scene() const { return scene_; }

    // Reads every mesh the scene uses (from game.pak or the built-ins) and uploads it once.
    void upload_meshes(render::Renderer& renderer);

    // Live editing: uploads meshes the scene uses that aren't on the GPU yet. Meshes that fail
    // to load are skipped (and not retried); the returned messages say why.
    std::vector<std::string> upload_new_meshes(render::Renderer& renderer);

    // Live editing (the editor's live link). Each throws SceneError on malformed data.
    // entity: {"id", "name", "parent", "components"} as in the entity table.
    void put_entity(const nlohmann::json& entity);
    void set_component(const std::string& entity_id, const std::string& component,
                       const nlohmann::json& value);
    void remove_entity(const std::string& entity_id);

    // Hot reload (the editor rewrote part of the package on disk, then said which part).
    void reload_scene_settings();
    void reload_entity(const std::string& entity_id);
    // Re-reads manifest.json and game.pak, then the mesh with this content id. Returns false if
    // no mesh of the scene uses it (nothing to do). Throws on a broken package.
    bool reload_asset(const std::string& asset, render::Renderer& renderer);

    // One draw per entity with a mesh, in world space.
    // What to draw, relative to the camera's origin (camera-relative rendering).
    std::vector<render::DrawItem> draws(const glm::dvec3& origin) const;

    // Outlines of the scene's colliders, triggers and player for debug drawing, relative to the
    // camera's origin (midgard/collider_outlines.h).
    std::vector<render::DebugLine> debug_lines(const glm::dvec3& origin) const;

    // The scene's camera, or a default view of the origin if it has none.
    render::Camera camera(float aspect) const;

    // The package's input bindings (built-in defaults for packages older than schema 3).
    InputMap input_map() const;

    // One frame of gameplay: the player moves, the third-person camera follows, physics steps
    // (bodies follow moved entities, dynamic bodies move theirs), and triggers note who entered
    // or left them (trigger_events, until the next update).
    void update(const Actions& actions, float dt);
    const std::vector<TriggerEvent>& trigger_events() const { return trigger_events_; }
    const ThirdPersonCamera& third_person() const { return *third_person_; }
    ScenePhysics& physics() { return *physics_; }
    const PhysicsLayers& physics_layers() const { return layers_; }

private:
    render::MeshData load_mesh(const std::string& asset) const;
    void refresh_third_person(entt::entity edited);
    void rebuild_physics();
    void update_triggers();

    PackageInfo package_;
    std::unique_ptr<Pak> pak_;
    Scene scene_;
    std::unique_ptr<ThirdPersonCamera> third_person_;
    PhysicsLayers layers_;
    std::unique_ptr<ScenePhysics> physics_;
    std::unordered_map<std::string, render::MeshId> meshes_;
    std::unordered_set<std::string> failed_meshes_;
    // Meshes read for collider outlines (once each; nullptr: couldn't be read).
    mutable std::unordered_map<std::string, std::unique_ptr<render::MeshData>> outline_meshes_;
    std::unordered_set<entt::entity> inside_;  // triggers the player is in
    std::vector<TriggerEvent> trigger_events_;
};

}  // namespace midgard
