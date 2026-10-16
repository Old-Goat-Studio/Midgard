// SPDX-License-Identifier: Apache-2.0
#include "midgard/scene.h"

#include <algorithm>

#include <memory>
#include <unordered_map>

#include <sqlite3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include "midgard/world_surface.h"

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

glm::dvec3 dvec3(const json& j, const char* key, glm::dvec3 fallback) {
    if (!j.contains(key)) return fallback;
    const json& v = j.at(key);
    if (!v.is_array() || v.size() < 3)
        throw SceneError(std::string("'") + key + "' needs 3 numbers");
    return {v[0].get<double>(), v[1].get<double>(), v[2].get<double>()};
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
        reg.emplace_or_replace<Transform>(e, dvec3(c, "position", glm::dvec3(0.0)),
                                          vec3(c, "rotation", glm::vec3(0.0f)),
                                          vec3(c, "scale", glm::vec3(1.0f)));
    } else if (name == "mesh") {
        reg.emplace_or_replace<MeshRef>(e, c.at("asset").get<std::string>());
    } else if (name == "material") {
        Material m;
        m.color = color4(c, "color");
        m.metallic = std::clamp(c.value("metallic", 0.0f), 0.0f, 1.0f);
        m.roughness = std::clamp(c.value("roughness", 0.8f), 0.0f, 1.0f);
        m.emissive = vec3(c, "emissive", glm::vec3(0.0f));
        m.emissive_strength = std::max(c.value("emissive_strength", 1.0f), 0.0f);
        reg.emplace_or_replace<Material>(e, m);
    } else if (name == "camera") {
        reg.emplace_or_replace<CameraLens>(e, c.value("fov", 55.0f), c.value("near", 0.05f),
                                           c.value("far", 500.0f));
    } else if (name == "player") {
        Player p;
        p.speed = std::max(c.value("speed", p.speed), 0.0f);
        p.radius = std::max(c.value("radius", p.radius), 0.01f);
        p.height = std::max(c.value("height", p.height), 2.0f * p.radius);
        p.max_slope = std::clamp(c.value("max_slope", p.max_slope), 0.0f, 89.0f);
        p.step_height = std::max(c.value("step_height", p.step_height), 0.0f);
        p.jump = std::max(c.value("jump", p.jump), 0.0f);
        reg.emplace_or_replace<Player>(e, p);
    } else if (name == "script") {
        reg.emplace_or_replace<ScriptRef>(e, c.at("module").get<std::string>());
    } else if (name == "trigger") {
        reg.emplace_or_replace<Trigger>(e, vec3(c, "size", glm::vec3(1.0f)));
    } else if (name == "sky") {
        reg.emplace_or_replace<Sky>(e, vec3(c, "color", SceneSettings{}.sky),
                                    vec3(c, "ambient", SceneSettings{}.ambient));
    } else if (name == "rigidbody") {
        RigidBody b;
        const std::string motion = c.value("motion", "dynamic");
        if (motion == "static")
            b.motion = physics::Motion::Static;
        else if (motion == "kinematic")
            b.motion = physics::Motion::Kinematic;
        else if (motion != "dynamic")
            throw SceneError("rigidbody: motion '" + motion +
                             "' is not static, dynamic or kinematic");
        b.mass = std::max(c.value("mass", 0.0f), 0.0f);
        b.layer = c.value("layer", 0);
        if (b.layer < 0 || b.layer >= physics::kLayers)
            throw SceneError("rigidbody: layer " + std::to_string(b.layer) + " is not 0 to 15");
        reg.emplace_or_replace<RigidBody>(e, b);
    } else if (name == "collider") {
        ColliderShape s;
        const std::string shape = c.value("shape", "box");
        using Kind = physics::Collider::Kind;
        if (shape == "box")
            s.kind = Kind::Box;
        else if (shape == "sphere")
            s.kind = Kind::Sphere;
        else if (shape == "capsule")
            s.kind = Kind::Capsule;
        else if (shape == "convex")
            s.kind = Kind::Convex;
        else if (shape == "mesh")
            s.kind = Kind::Mesh;
        else if (shape == "heightfield")
            s.kind = Kind::Heightfield;
        else
            throw SceneError("collider: shape '" + shape +
                             "' is not box, sphere, capsule, convex, "
                             "mesh or heightfield");
        s.size = vec3(c, "size", glm::vec3(1.0f));
        s.radius = c.value("radius", 0.5f);
        s.height = c.value("height", 2.0f);
        s.offset = vec3(c, "offset", glm::vec3(0.0f));
        s.mesh = c.value("mesh", "");
        s.samples = c.value("samples", 0u);
        s.heights = c.value("heights", std::vector<float>{});
        s.spacing = c.value("spacing", 1.0f);
        reg.emplace_or_replace<ColliderShape>(e, std::move(s));
    } else if (name == "physics_material") {
        reg.emplace_or_replace<PhysicsMaterial>(
            e, std::max(c.value("friction", 0.5f), 0.0f),
            std::clamp(c.value("bounciness", 0.0f), 0.0f, 1.0f));
    } else if (name == "light") {
        const std::string type = c.value("type", "directional");
        reg.remove<DirectionalLight, LocalLightSource>(e);
        if (type == "directional") {
            reg.emplace<DirectionalLight>(e, vec3(c, "color", glm::vec3(1.0f)),
                                          c.value("intensity", 1.0f));
        } else if (type == "point" || type == "spot") {
            LocalLightSource l;
            l.spot = type == "spot";
            l.color = vec3(c, "color", glm::vec3(1.0f));
            l.intensity = std::max(c.value("intensity", 1.0f), 0.0f);
            l.range = std::max(c.value("range", 10.0f), 0.01f);
            l.inner_angle = std::clamp(c.value("inner_angle", 20.0f), 0.0f, 89.0f);
            l.outer_angle = std::clamp(c.value("outer_angle", 30.0f), l.inner_angle, 90.0f);
            reg.emplace<LocalLightSource>(e, l);
        }
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
        reg.remove<DirectionalLight, LocalLightSource>(e);
    else if (name == "script")
        reg.remove<ScriptRef>(e);
    else if (name == "trigger")
        reg.remove<Trigger>(e);
    else if (name == "sky")
        reg.remove<Sky>(e);
    else if (name == "rigidbody")
        reg.remove<RigidBody>(e);
    else if (name == "collider")
        reg.remove<ColliderShape>(e);
    else if (name == "physics_material")
        reg.remove<PhysicsMaterial>(e);
}

void add_components(entt::registry& reg, entt::entity e, const json& c) {
    if (!c.is_object()) throw SceneError("components must be a JSON object");
    reg.remove<Transform, MeshRef, Material, CameraLens, DirectionalLight, LocalLightSource, Player,
               ScriptRef, Trigger, Sky, RigidBody, ColliderShape, PhysicsMaterial>(e);
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
        if (settings.contains("body")) out.body = body_from_json(settings.at("body"));
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

glm::dmat4 Scene::local_matrix(entt::entity e) const {
    const Transform& t = registry.get<Transform>(e);
    return glm::translate(glm::dmat4(1.0), t.position) * glm::dmat4(rotation_matrix(t.rotation)) *
           glm::scale(glm::dmat4(1.0), glm::dvec3(t.scale));
}

glm::dmat4 Scene::world_matrix(entt::entity e) const {
    glm::dmat4 m = local_matrix(e);
    int depth = 0;
    for (entt::entity p = registry.get<EntityInfo>(e).parent; p != entt::null;
         p = registry.get<EntityInfo>(p).parent) {
        if (++depth > 256) throw SceneError("entity parents form a loop");
        m = local_matrix(p) * m;
    }
    return m;
}

glm::dvec3 Scene::world_position(entt::entity e) const {
    return glm::dvec3(world_matrix(e)[3]);
}

glm::mat4 Scene::draw_matrix(entt::entity e, const glm::dvec3& origin) const {
    return render::relative_to(world_matrix(e), origin);
}

render::DrawItem Scene::draw_item(entt::entity e, render::MeshId mesh,
                                  const glm::dvec3& origin) const {
    render::DrawItem d;
    d.mesh = mesh;
    d.model = draw_matrix(e, origin);
    d.color = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
    if (const Material* m = registry.try_get<Material>(e)) {
        d.color = m->color;
        d.metallic = m->metallic;
        d.roughness = m->roughness;
        d.emissive = m->emissive;
        d.emissive_strength = m->emissive_strength;
    }
    // The world's surface casts no shadow: inside a cylinder or a hollow world its far side
    // would shade everything (inside worlds get their own light in milestone 5.8).
    if (const MeshRef* r = registry.try_get<MeshRef>(e); r && r->asset == kBuiltinSurface)
        d.casts_shadow = false;
    return d;
}

std::optional<render::Camera> Scene::camera(float aspect) const {
    const entt::entity e = first_with<CameraLens>(registry);
    if (e == entt::null) return std::nullopt;
    const CameraLens& lens = registry.get<CameraLens>(e);
    render::Camera cam;
    cam.origin = world_position(e);
    // Cameras look along their local -Z; relative to their own origin they only turn.
    cam.view = glm::inverse(render::relative_to(world_matrix(e), cam.origin));
    cam.projection =
        render::perspective(glm::radians(lens.fov), aspect, lens.near_plane, lens.far_plane);
    return cam;
}

render::Light Scene::light(const glm::dvec3& origin) const {
    render::Light light;
    light.ambient = settings.ambient;
    // Until environments are authored (Phase 5.8), the ground reflects some of the sky.
    light.ambient_ground = settings.ambient * 0.6f;
    if (settings.body) light.up = glm::vec3(settings.body->up(origin));
    const entt::entity e = first_with<DirectionalLight>(registry);
    if (e != entt::null) {
        const DirectionalLight& l = registry.get<DirectionalLight>(e);
        light.direction = glm::vec3(world_matrix(e) * glm::dvec4(0, 0, -1, 0));
        light.color = l.color;
        light.intensity = l.intensity;
    }
    // The local lights nearest the camera, positioned relative to it.
    struct Near {
        double distance;
        render::LocalLight light;
    };
    std::vector<Near> near;
    for (entt::entity le : registry.view<const LocalLightSource>()) {
        const LocalLightSource& s = registry.get<LocalLightSource>(le);
        const glm::dmat4 m = world_matrix(le);
        const glm::dvec3 relative = glm::dvec3(m[3]) - origin;
        render::LocalLight l;
        l.position = glm::vec3(relative);
        l.color = s.color;
        l.intensity = s.intensity;
        l.range = s.range;
        l.spot = s.spot;
        l.direction = glm::vec3(m * glm::dvec4(0, 0, -1, 0));
        l.inner_angle = s.inner_angle;
        l.outer_angle = s.outer_angle;
        near.push_back({glm::length(relative), l});
    }
    std::stable_sort(near.begin(), near.end(),
                     [](const Near& a, const Near& b) { return a.distance < b.distance; });
    for (size_t i = 0; i < near.size() && i < size_t(render::kMaxLocalLights); ++i)
        light.local.push_back(near[i].light);
    return light;
}

}  // namespace midgard
