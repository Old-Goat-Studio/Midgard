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

void add_components(entt::registry& reg, entt::entity e, const json& c) {
    if (c.contains("transform")) {
        const json& t = c.at("transform");
        reg.emplace<Transform>(e, vec3(t, "position", glm::vec3(0.0f)),
                               vec3(t, "rotation", glm::vec3(0.0f)),
                               vec3(t, "scale", glm::vec3(1.0f)));
    } else {
        reg.emplace<Transform>(e);
    }
    if (c.contains("mesh")) reg.emplace<MeshRef>(e, c.at("mesh").at("asset").get<std::string>());
    if (c.contains("material")) reg.emplace<Material>(e, color4(c.at("material"), "color"));
    if (c.contains("camera")) {
        const json& cam = c.at("camera");
        reg.emplace<CameraLens>(e, cam.value("fov", 55.0f), cam.value("near", 0.05f),
                                cam.value("far", 500.0f));
    }
    if (c.contains("light") && c.at("light").value("type", "directional") == "directional") {
        const json& l = c.at("light");
        reg.emplace<DirectionalLight>(e, vec3(l, "color", glm::vec3(1.0f)),
                                      l.value("intensity", 1.0f));
    }
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

    {
        const bool settings_column = has_column(db.get(), "scene", "settings");  // migration 2
        Stmt s = prepare(db.get(), settings_column ? "SELECT settings FROM scene WHERE id = ?"
                                                   : "SELECT '{}' FROM scene WHERE id = ?");
        sqlite3_bind_text(s.get(), 1, scene_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s.get()) != SQLITE_ROW)
            throw SceneError("scene '" + scene_id + "' is not in the database");
        try {
            const json settings = json::parse(column_text(s.get(), 0));
            scene.settings.sky = vec3(settings, "sky", scene.settings.sky);
            scene.settings.ambient = vec3(settings, "ambient", scene.settings.ambient);
        } catch (const std::exception& e) {
            throw SceneError("scene '" + scene_id + "' settings: " + e.what());
        }
    }

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
    return scene;
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
