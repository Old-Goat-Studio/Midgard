// SPDX-License-Identifier: Apache-2.0
#include "midgard/game.h"

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

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

std::vector<std::string> Game::upload_new_meshes(render::Renderer& renderer) {
    std::vector<std::string> errors;
    for (entt::entity e : scene_.registry.view<const MeshRef>()) {
        const std::string asset = scene_.registry.get<MeshRef>(e).asset;
        if (meshes_.contains(asset) || failed_meshes_.contains(asset)) continue;
        try {
            meshes_.emplace(asset, renderer.upload_mesh(load_mesh(asset)));
        } catch (const std::exception& ex) {
            failed_meshes_.insert(asset);
            errors.emplace_back(ex.what());
        }
    }
    return errors;
}

void Game::put_entity(const nlohmann::json& entity) {
    const auto text = [&](const char* key) {
        const auto it = entity.find(key);
        return it != entity.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    const std::string id = text("id");
    if (id.empty()) throw SceneError("entity.put needs an entity id");
    const auto components = entity.find("components");
    refresh_third_person(
        scene_.put_entity(id, text("name"), text("parent"),
                          components != entity.end() ? *components : nlohmann::json::object()));
}

// After an edit the player or camera may be different entities. The orbit is kept unless the
// camera itself was edited.
void Game::refresh_third_person(entt::entity edited) {
    const ThirdPersonCamera old = *third_person_;
    third_person_ = std::make_unique<ThirdPersonCamera>(scene_);
    const bool camera_edited = edited != entt::null && scene_.registry.valid(edited) &&
                               scene_.registry.all_of<CameraLens>(edited);
    if (old.active() && !camera_edited)
        third_person_->set_view(scene_, old.yaw(), old.elevation(), old.distance());
}

void Game::set_component(const std::string& entity_id, const std::string& component,
                         const nlohmann::json& value) {
    const entt::entity e = scene_.find(entity_id);
    if (e == entt::null) throw SceneError("entity '" + entity_id + "' is not in the scene");
    scene_.set_component(e, component, value);
    refresh_third_person(e);
}

void Game::remove_entity(const std::string& entity_id) {
    scene_.remove_entity(entity_id);
    refresh_third_person(entt::null);
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
