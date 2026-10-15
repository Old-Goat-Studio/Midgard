// SPDX-License-Identifier: Apache-2.0
#include "midgard/game.h"

#include <glm/gtc/matrix_transform.hpp>

#include "midgard/gltf.h"

namespace midgard {

Game::Game(const std::filesystem::path& package_dir)
    : package_(open_package(package_dir)),
      scene_(Scene::load(package_dir / package_.manifest.database, package_.manifest.entry_scene)) {
    const auto pak_file = package_dir / "game.pak";
    if (std::filesystem::is_regular_file(pak_file)) pak_ = std::make_unique<Pak>(pak_file);
    third_person_ = std::make_unique<ThirdPersonCamera>(scene_);
}

InputMap Game::input_map() const {
    return InputMap::load(package_.root / package_.manifest.database);
}

void Game::update(const Actions& actions, float dt) {
    third_person_->update(scene_, actions, dt);
}

render::MeshData Game::load_mesh(const std::string& asset) const {
    if (asset == kBuiltinCube) return render::make_cube();
    for (const ContentEntry& c : package_.manifest.content) {
        if (c.id != asset) continue;
        if (!pak_) throw AssetError("mesh '" + asset + "' needs game.pak, which is missing");
        const std::vector<uint8_t> bytes = pak_->read(c.path);
        try {
            return load_glb(bytes);
        } catch (const AssetError& e) {
            throw AssetError("mesh '" + asset + "' (" + c.path + "): " + e.what());
        }
    }
    throw AssetError("mesh '" + asset + "' is not in the package's content list");
}

void Game::upload_meshes(render::Renderer& renderer) {
    for (entt::entity e : scene_.registry.view<const MeshRef>()) {
        const std::string& asset = scene_.registry.get<MeshRef>(e).asset;
        if (!meshes_.contains(asset))
            meshes_.emplace(asset, renderer.upload_mesh(load_mesh(asset)));
    }
}

std::vector<render::DrawItem> Game::draws() const {
    std::vector<render::DrawItem> out;
    for (entt::entity e : scene_.registry.view<const MeshRef>()) {
        auto it = meshes_.find(scene_.registry.get<MeshRef>(e).asset);
        if (it == meshes_.end()) continue;  // not uploaded yet
        const Material* material = scene_.registry.try_get<Material>(e);
        out.push_back({it->second, scene_.world_matrix(e),
                       material ? material->color : glm::vec4(0.8f, 0.8f, 0.8f, 1.0f)});
    }
    return out;
}

render::Camera Game::camera(float aspect) const {
    if (auto cam = scene_.camera(aspect)) return *cam;
    render::Camera cam;
    cam.view = glm::lookAt(glm::vec3(4.0f, 3.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0, 1, 0));
    cam.projection = render::perspective(glm::radians(55.0f), aspect, 0.05f, 500.0f);
    return cam;
}

}  // namespace midgard
