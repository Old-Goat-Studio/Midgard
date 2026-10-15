// SPDX-License-Identifier: Apache-2.0
// A loaded game: package + game.pak + entry scene, with meshes uploaded to the renderer.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "midgard/package.h"
#include "midgard/pak.h"
#include "midgard/render/renderer.h"
#include "midgard/scene.h"

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

    // One draw per entity with a mesh, in world space.
    std::vector<render::DrawItem> draws() const;

    // The scene's camera, or a default view of the origin if it has none.
    render::Camera camera(float aspect) const;

private:
    render::MeshData load_mesh(const std::string& asset) const;

    PackageInfo package_;
    std::unique_ptr<Pak> pak_;
    Scene scene_;
    std::unordered_map<std::string, render::MeshId> meshes_;
};

}  // namespace midgard
