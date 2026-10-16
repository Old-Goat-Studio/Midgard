// SPDX-License-Identifier: Apache-2.0
#include "midgard/scene.h"

#include <memory>
#include <unordered_map>

#include <sqlite3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

namespace midgard {
namespace {

using nlohmann::json;

struct SqliteCloser {
    void operator()(sqlite3* db) const { sqlite3_close(db); }
};
struct StmtCloser {
    void operator()(sqlite3_stmt* s) const { sqlite3_finalize(s); }
};
using Db = std::unique_ptr<sqlite3, SqliteCloser>;
using Stmt = std::unique_ptr<sqlite3_stmt, StmtCloser>;

Db open_read_only(const std::filesystem::path& file) {
    sqlite3* raw = nullptr;
    const std::u8string u8 = file.u8string();
    const int rc = sqlite3_open_v2(std::string(u8.begin(), u8.end()).c_str(), &raw,
                                   SQLITE_OPEN_READONLY, nullptr);
    Db db(raw);
    if (rc != SQLITE_OK)
        throw SceneError("cannot open " + file.string() + ": " +
                         (raw ? sqlite3_errmsg(raw) : "out of memory"));
    return db;
}

Stmt prepare(sqlite3* db, const char* sql) {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
        throw SceneError(std::string("database query failed: ") + sqlite3_errmsg(db));
    return Stmt(s);
}

std::string column_text(sqlite3_stmt* s, int col) {
    const auto* text = sqlite3_column_text(s, col);
    return text ? reinterpret_cast<const char*>(text) : "";
}

bool has_column(sqlite3* db, const char* table, const char* column) {
    Stmt s = prepare(db, (std::string("PRAGMA table_info(") + table + ")").c_str());
    while (sqlite3_step(s.get()) == SQLITE_ROW)
        if (column_text(s.get(), 1) == column) return true;
    return false;
}

glm::vec3 vec3(const json& j, const char* key, glm::vec3 fallback) {
    if (!j.contains(key)) return fallback;
    const json& v = j.at(key);
    if (!v.is_array() || v.size() < 3)
        throw SceneError(std::string("'") + key + "' needs 3 numbers");
    return {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
}

glm::vec4 color4(const json& j, const char* key) {
    const glm::vec3 rgb = vec3(j, key, glm::vec3(1.0f));
    const json& v = j.contains(key) ? j.at(key) : json::array();
    return {rgb, v.size() >= 4 ? v[3].get<float>() : 1.0f};
}

// One component from its JSON (schema/components.md). Components this engine doesn't know yet
// stay in the database for later versions and are ignored here.
void add_component(entt::registry& reg, entt::entity e, const std::string& name, const json& c) {
    if (name == "transform") {
        reg.emplace_or_replace<Transform>(e, vec3(c, "position", glm::vec3(0.0f)),
                                          vec3(c, "rotation", glm::vec3(0.0f)),
                                          vec3(c, "scale", glm::vec3(1.0f)));
    } else if (name == "mesh") {
        reg.emplace_or_replace<MeshRef>(e, c.at("asset").get<std::string>());
    } else if (name == "material") {
        reg.emplace_or_replace<Material>(e, color4(c, "color"));
    } else if (name == "camera") {
        reg.emplace_or_replace<CameraLens>(e, c.value("fov", 55.0f), c.value("near", 0.05f),
                                           c.value("far", 500.0f));
    } else if (name == "player") {
        reg.emplace_or_replace<Player>(e, c.value("speed", 4.0f));
    } else if (name == "script") {
        reg.emplace_or_replace<ScriptRef>(e, c.at("module").get<std::string>());
    } else if (name == "trigger") {
        reg.emplace_or_replace<Trigger>(e, vec3(c, "size", glm::vec3(1.0f)));
    } else if (name == "sky") {
        reg.emplace_or_replace<Sky>(e, vec3(c, "color", SceneSettings{}.sky),
                                    vec3(c, "ambient", SceneSettings{}.ambient));
    } else if (name == "light") {
        if (c.value("type", "directional") == "directional")
            reg.emplace_or_replace<DirectionalLight>(e, vec3(c, "color", glm::vec3(1.0f)),
                                                     c.value("intensity", 1.0f));
        else
            reg.remove<DirectionalLight>(e);
    }
}

void remove_component(entt::registry& reg, entt::entity e, const std::string& name) {
    if (name == "transform")
        reg.emplace_or_replace<Transform>(e);  // every entity has a transform
    else if (name == "mesh")
        reg.remove<MeshRef>(e);
    else if (name == "material")
        reg.remove<Material>(e);
    else if (name == "camera")
        reg.remove<CameraLens>(e);
    else if (name == "player")
        reg.remove<Player>(e);
    else if (name == "light")
        reg.remove<DirectionalLight>(e);
    else if (name == "script")
        reg.remove<ScriptRef>(e);
    else if (name == "trigger")
        reg.remove<Trigger>(e);
    else if (name == "sky")
        reg.remove<Sky>(e);
}

void add_components(entt::registry& reg, entt::entity e, const json& c) {
    if (!c.is_object()) throw SceneError("components must be a JSON object");
    reg.remove<Transform, MeshRef, Material, CameraLens, DirectionalLight, Player, ScriptRef,
               Trigger, Sky>(e);
    reg.emplace<Transform>(e);
    for (const auto& [name, value] : c.items()) add_component(reg, e, name, value);
}

SceneSettings settings_from(sqlite3* db, const std::string& scene_id) {
    const bool settings_column = has_column(db, "scene", "settings");  // migration 2
    Stmt s = prepare(db, settings_column ? "SELECT settings FROM scene WHERE id = ?"
                                         : "SELECT '{}' FROM scene WHERE id = ?");
    sqlite3_bind_text(s.get(), 1, scene_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s.get()) != SQLITE_ROW)
        throw SceneError("scene '" + scene_id + "' is not in the database");
    SceneSettings out;
    try {
        const json settings = json::parse(column_text(s.get(), 0));
        out.sky = vec3(settings, "sky", out.sky);
        out.ambient = vec3(settings, "ambient", out.ambient);
    } catch (const std::exception& e) {
        throw SceneError("scene '" + scene_id + "' settings: " + e.what());
    }
    return out;
}

template <typename Component>
entt::entity first_with(const entt::registry& reg) {
    // Entities are created in entity-id order, so the lowest handle is the first by id.
    entt::entity found = entt::null;
    for (entt::entity e : reg.view<const Component>())
        if (found == entt::null || entt::to_integral(e) < entt::to_integral(found)) found = e;
    return found;
}

}  // namespace

glm::mat4 rotation_matrix(const glm::vec3& d) {
    const glm::mat4 ry = glm::rotate(glm::mat4(1.0f), glm::radians(d.y), glm::vec3(0, 1, 0));
    const glm::mat4 rx = glm::rotate(glm::mat4(1.0f), glm::radians(d.x), glm::vec3(1, 0, 0));
    const glm::mat4 rz = glm::rotate(glm::mat4(1.0f), glm::radians(d.z), glm::vec3(0, 0, 1));
    return ry * rx * rz;
}

Scene Scene::load(const std::filesystem::path& database, const std::string& scene_id) {
    Db db = open_read_only(database);
    Scene scene;
    scene.id = scene_id;
    scene.settings = settings_from(db.get(), scene_id);

    Stmt s = prepare(db.get(),
                     "SELECT id, name, parent, components FROM entity WHERE scene = ? ORDER BY id");
    sqlite3_bind_text(s.get(), 1, scene_id.c_str(), -1, SQLITE_TRANSIENT);
    std::unordered_map<std::string, entt::entity> by_id;
    std::vector<std::pair<entt::entity, std::string>> parents;
    while (sqlite3_step(s.get()) == SQLITE_ROW) {
        const entt::entity e = scene.registry.create();
        const std::string id = column_text(s.get(), 0);
        scene.registry.emplace<EntityInfo>(e, id, column_text(s.get(), 1), entt::null);
        if (sqlite3_column_type(s.get(), 2) != SQLITE_NULL)
            parents.emplace_back(e, column_text(s.get(), 2));
        try {
            add_components(scene.registry, e, json::parse(column_text(s.get(), 3)));
        } catch (const std::exception& ex) {  // bad JSON or a component of the wrong shape
            throw SceneError("entity '" + id + "': " + ex.what());
        }
        by_id.emplace(id, e);
    }
    for (const auto& [e, parent_id] : parents) {
        auto it = by_id.find(parent_id);
        if (it == by_id.end()) throw SceneError("entity parent '" + parent_id + "' not found");
        scene.registry.get<EntityInfo>(e).parent = it->second;
    }
    scene.use_sky();
    return scene;
}

SceneSettings Scene::read_settings(const std::filesystem::path& database,
                                   const std::string& scene_id) {
    Db db = open_read_only(database);
    return settings_from(db.get(), scene_id);
}

std::optional<EntityRow> Scene::read_entity(const std::filesystem::path& database,
                                            const std::string& entity_id) {
    Db db = open_read_only(database);
    Stmt s = prepare(db.get(), "SELECT id, name, parent, components FROM entity WHERE id = ?");
    sqlite3_bind_text(s.get(), 1, entity_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s.get()) != SQLITE_ROW) return std::nullopt;
    return EntityRow{column_text(s.get(), 0), column_text(s.get(), 1), column_text(s.get(), 2),
                     column_text(s.get(), 3)};
}

entt::entity Scene::find(const std::string& entity_id) const {
    for (entt::entity e : registry.view<const EntityInfo>())
        if (registry.get<EntityInfo>(e).id == entity_id) return e;
    return entt::null;
}

entt::entity Scene::put_entity(const std::string& entity_id, const std::string& name,
                               const std::string& parent_id, const json& components) {
    entt::entity e = find(entity_id);
    if (e == entt::null) {
        e = registry.create();
        registry.emplace<EntityInfo>(e, entity_id, name, entt::null);
    }
    EntityInfo& info = registry.get<EntityInfo>(e);
    info.name = name;
    info.parent = parent_id.empty() ? entt::null : find(parent_id);
    try {
        add_components(registry, e, components);
    } catch (const SceneError&) {
        throw;
    } catch (const std::exception& ex) {  // a component of the wrong shape
        throw SceneError("entity '" + entity_id + "': " + ex.what());
    }
    apply_sky(e);
    return e;
}

void Scene::set_component(entt::entity e, const std::string& component, const json& value) {
    try {
        if (value.is_null())
            remove_component(registry, e, component);
        else
            add_component(registry, e, component, value);
    } catch (const std::exception& ex) {
        throw SceneError("component '" + component + "': " + ex.what());
    }
    apply_sky(e);
}

void Scene::use_sky() {
    apply_sky(first_with<Sky>(registry));
}

void Scene::apply_sky(entt::entity e) {
    if (e == entt::null || !registry.all_of<Sky>(e)) return;
    const Sky& sky = registry.get<Sky>(e);
    settings.sky = sky.color;
    settings.ambient = sky.ambient;
}

bool Scene::remove_entity(const std::string& entity_id) {
    const entt::entity e = find(entity_id);
    if (e == entt::null) return false;
    for (entt::entity child : registry.view<EntityInfo>())
        if (registry.get<EntityInfo>(child).parent == e)
            registry.get<EntityInfo>(child).parent = entt::null;
    registry.destroy(e);
    return true;
}

glm::mat4 Scene::local_matrix(entt::entity e) const {
    const Transform& t = registry.get<Transform>(e);
    return glm::translate(glm::mat4(1.0f), t.position) * rotation_matrix(t.rotation) *
           glm::scale(glm::mat4(1.0f), t.scale);
}

glm::mat4 Scene::world_matrix(entt::entity e) const {
    glm::mat4 m = local_matrix(e);
    int depth = 0;
    for (entt::entity p = registry.get<EntityInfo>(e).parent; p != entt::null;
         p = registry.get<EntityInfo>(p).parent) {
        if (++depth > 256) throw SceneError("entity parents form a loop");
        m = local_matrix(p) * m;
    }
    return m;
}

std::optional<render::Camera> Scene::camera(float aspect) const {
    const entt::entity e = first_with<CameraLens>(registry);
    if (e == entt::null) return std::nullopt;
    const CameraLens& lens = registry.get<CameraLens>(e);
    render::Camera cam;
    cam.view = glm::inverse(world_matrix(e));  // cameras look along their local -Z
    cam.projection =
        render::perspective(glm::radians(lens.fov), aspect, lens.near_plane, lens.far_plane);
    return cam;
}

render::Light Scene::light() const {
    render::Light light;
    light.ambient = settings.ambient;
    const entt::entity e = first_with<DirectionalLight>(registry);
    if (e != entt::null) {
        const DirectionalLight& l = registry.get<DirectionalLight>(e);
        light.direction = glm::vec3(world_matrix(e) * glm::vec4(0, 0, -1, 0));
        light.color = l.color * l.intensity;
    }
    return light;
}

}  // namespace midgard
