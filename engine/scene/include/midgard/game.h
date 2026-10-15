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
#include "midgard/third_person.h"

namespace midgard {

// Mesh asset ids that are built into the engine rather than stored in game.pak.
inline constexpr const char* kBuiltinCube = "builtin:cube";

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

    // One draw per entity with a mesh, in world space.
    std::vector<render::DrawItem> draws() const;

    // The scene's camera, or a default view of the origin if it has none.
    render::Camera camera(float aspect) const;

    // The package's input bindings (built-in defaults for packages older than schema 3).
    InputMap input_map() const;

    // One frame of gameplay: the player moves and the third-person camera follows.
    void update(const Actions& actions, float dt);
    const ThirdPersonCamera& third_person() const { return *third_person_; }

private:
    render::MeshData load_mesh(const std::string& asset) const;
    void refresh_third_person(entt::entity edited);

    PackageInfo package_;
    std::unique_ptr<Pak> pak_;
    Scene scene_;
    std::unique_ptr<ThirdPersonCamera> third_person_;
    std::unordered_map<std::string, render::MeshId> meshes_;
    std::unordered_set<std::string> failed_meshes_;
};

}  // namespace midgard
