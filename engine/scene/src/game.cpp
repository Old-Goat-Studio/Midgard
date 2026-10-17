// SPDX-License-Identifier: Apache-2.0
#include "midgard/game.h"

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include "midgard/collider_outlines.h"
#include "midgard/gltf.h"
#include "midgard/world_surface.h"

namespace midgard {

Game::Game(const std::filesystem::path& package_dir)
    : package_(open_package(package_dir)),
      scene_(Scene::load(package_dir / package_.manifest.database, package_.manifest.entry_scene)) {
    const auto pak_file = package_dir / "game.pak";
    if (std::filesystem::is_regular_file(pak_file)) pak_ = std::make_unique<Pak>(pak_file);
    third_person_ = std::make_unique<ThirdPersonCamera>(scene_);
    layers_ = PhysicsLayers::load(package_dir / package_.manifest.database);
    rebuild_physics();
}

void Game::rebuild_physics() {
    physics_ = std::make_unique<ScenePhysics>(
        scene_, layers_, [this](const std::string& asset) { return load_mesh(asset); });
}

InputMap Game::input_map() const {
    return InputMap::load(package_.root / package_.manifest.database);
}

void Game::update(const Actions& actions, float dt) {
    const PlayerIntent intent = third_person_->steer(scene_, actions, dt);
    physics_->set_player_intent(intent.walk, intent.jump);
    physics_->update(scene_, dt);  // the player walks
    third_person_->follow(scene_);
    update_triggers();
}

void Game::update_triggers() {
    trigger_events_.clear();
    entt::entity player = entt::null;
    for (entt::entity e : scene_.registry.view<const Player>()) {
        player = e;
        break;
    }
    // The player's capsule against every trigger's sensor in the physics world.
    std::unordered_set<entt::entity> inside;
    for (entt::entity e : physics_->triggers_touching(scene_, player)) inside.insert(e);
    for (entt::entity e : inside)
        if (!inside_.contains(e)) trigger_events_.push_back({e, player, true});
    for (entt::entity e : inside_)
        if (!inside.contains(e) && scene_.registry.valid(e))
            trigger_events_.push_back({e, player, false});
    inside_ = std::move(inside);
}

render::MeshData Game::load_mesh(const std::string& asset) const {
    if (asset == kBuiltinCube) return render::make_cube();
    if (asset == kBuiltinSurface) return surface_mesh(scene_.settings.body);
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
    for (entt::entity e : scene_.registry.view<const MeshRef>())
        for (const std::string& asset : scene_.registry.get<MeshRef>(e).assets())
            if (!meshes_.contains(asset))
                meshes_.emplace(asset, renderer.upload_mesh(load_mesh(asset)));
    // A missing texture is drawn without (the cook warned).
    materials_.upload(scene_, renderer, [this](const std::string& t) { return read_texture(t); });
}

std::vector<uint8_t> Game::read_texture(const std::string& asset) const {
    for (const ContentEntry& c : package_.manifest.content) {
        if (c.id != asset) continue;
        if (!pak_) throw AssetError("it needs game.pak, which is missing");
        return pak_->read(c.path);
    }
    throw AssetError("it is not in the package");
}

std::vector<std::string> Game::upload_new_meshes(render::Renderer& renderer) {
    std::vector<std::string> errors;
    for (entt::entity e : scene_.registry.view<const MeshRef>()) {
        for (const std::string& asset : scene_.registry.get<MeshRef>(e).assets()) {
            if (meshes_.contains(asset) || failed_meshes_.contains(asset)) continue;
            try {
                meshes_.emplace(asset, renderer.upload_mesh(load_mesh(asset)));
            } catch (const std::exception& ex) {
                failed_meshes_.insert(asset);
                errors.emplace_back(ex.what());
            }
        }
    }
    for (std::string& error : materials_.upload(
             scene_, renderer, [this](const std::string& t) { return read_texture(t); }))
        errors.push_back(std::move(error));
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
    const entt::entity e =
        scene_.put_entity(id, text("name"), text("parent"),
                          components != entity.end() ? *components : nlohmann::json::object());
    refresh_third_person(e);
    physics_->rebuild(scene_, e);
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

void Game::set_object(const std::string& object_id, const std::string& parts_json) {
    for (entt::entity e : scene_.set_object(object_id, parts_json)) physics_->rebuild(scene_, e);
}

void Game::set_component(const std::string& entity_id, const std::string& component,
                         const nlohmann::json& value) {
    const entt::entity e = scene_.find(entity_id);
    if (e == entt::null) throw SceneError("entity '" + entity_id + "' is not in the scene");
    scene_.set_component(e, component, value);
    refresh_third_person(e);
    physics_->rebuild(scene_, e);
}

void Game::remove_entity(const std::string& entity_id) {
    physics_->remove(scene_.find(entity_id));
    scene_.remove_entity(entity_id);
    physics_->prune(scene_);  // an instance's parts went with it
    refresh_third_person(entt::null);
}

void Game::reload_scene_settings() {
    scene_.settings = Scene::read_settings(package_.root / package_.manifest.database, scene_.id);
    scene_.use_sky();   // a Sky entity outranks the settings
    rebuild_physics();  // the world's shape may have changed
}

void Game::reload_entity(const std::string& entity_id) {
    const auto row = Scene::read_entity(package_.root / package_.manifest.database, entity_id);
    if (!row) {
        remove_entity(entity_id);
        return;
    }
    nlohmann::json components;
    try {
        components = nlohmann::json::parse(row->components_json);
    } catch (const std::exception& e) {
        throw SceneError("entity '" + entity_id + "': " + e.what());
    }
    put_entity({{"id", row->id},
                {"name", row->name},
                {"parent", row->parent},
                {"components", std::move(components)}});
}

bool Game::reload_asset(const std::string& asset, render::Renderer& renderer) {
    package_.manifest = load_manifest(package_.root / "manifest.json");
    const auto pak_file = package_.root / "game.pak";
    pak_ = std::filesystem::is_regular_file(pak_file) ? std::make_unique<Pak>(pak_file) : nullptr;
    failed_meshes_.erase(asset);
    outline_meshes_.erase(asset);
    // A texture: the materials using it are made again from the new package.
    materials_.forget(asset);
    bool texture_used = false;
    for (entt::entity e : scene_.registry.view<const Material>())
        for (const std::string& t : scene_.registry.get<Material>(e).textures)
            texture_used = texture_used || t == asset;
    if (texture_used)
        materials_.upload(scene_, renderer,
                          [this](const std::string& t) { return read_texture(t); });
    bool used = false;
    for (entt::entity e : scene_.registry.view<const MeshRef>())
        for (const std::string& a : scene_.registry.get<MeshRef>(e).assets())
            used = used || a == asset;
    if (!used) return texture_used;
    render::MeshData mesh = load_mesh(asset);
    if (auto it = meshes_.find(asset); it != meshes_.end())
        renderer.replace_mesh(it->second, mesh);
    else
        meshes_.emplace(asset, renderer.upload_mesh(mesh));
    // Colliders made from this mesh change with it.
    for (entt::entity e : scene_.registry.view<const ColliderShape, const MeshRef>())
        if (scene_.registry.get<MeshRef>(e).asset == asset) physics_->rebuild(scene_, e);
    return true;
}

std::vector<render::DrawItem> Game::draws(const glm::dvec3& origin) const {
    std::vector<render::DrawItem> out;
    for (entt::entity e : scene_.registry.view<const MeshRef>()) {
        const MeshRef& ref = scene_.registry.get<MeshRef>(e);
        const double distance =
            ref.lods.empty() ? 0.0 : glm::distance(scene_.world_position(e), origin);
        auto it = meshes_.find(ref.at(distance));
        if (it == meshes_.end()) continue;  // not uploaded yet
        render::DrawItem d = scene_.draw_item(e, it->second, origin);
        if (const Material* m = scene_.registry.try_get<Material>(e)) materials_.apply(*m, d);
        out.push_back(d);
    }
    return out;
}

std::vector<render::DebugLine> Game::debug_lines(const glm::dvec3& origin) const {
    return collider_outlines(scene_, origin, [this](const std::string& asset) {
        auto it = outline_meshes_.find(asset);
        if (it == outline_meshes_.end()) {
            std::unique_ptr<render::MeshData> data;
            try {
                data = std::make_unique<render::MeshData>(load_mesh(asset));
            } catch (const std::exception&) {  // reported when the scene loaded
            }
            it = outline_meshes_.emplace(asset, std::move(data)).first;
        }
        return it->second.get();
    });
}

render::Camera Game::camera(float aspect) const {
    if (auto cam = scene_.camera(aspect)) return *cam;
    render::Camera cam;
    cam.view = glm::lookAt(glm::vec3(4.0f, 3.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0, 1, 0));
    cam.projection = render::perspective(glm::radians(55.0f), aspect, 0.05f, 500.0f);
    return cam;
}

}  // namespace midgard
