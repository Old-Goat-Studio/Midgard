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
        MeshRef ref{c.at("asset").get<std::string>(), {}};
        if (const auto lods = c.find("lods"); lods != c.end() && lods->is_array()) {
            for (const json& lod : *lods)
                ref.lods.push_back({lod.at("asset").get<std::string>(),
                                    std::max(lod.value("distance", 0.0f), 0.0f)});
            std::stable_sort(
                ref.lods.begin(), ref.lods.end(),
                [](const MeshLod& a, const MeshLod& b) { return a.distance < b.distance; });
        }
        reg.emplace_or_replace<MeshRef>(e, std::move(ref));
    } else if (name == "material") {
        Material m;
        m.color = color4(c, "color");
        m.metallic = std::clamp(c.value("metallic", 0.0f), 0.0f, 1.0f);
        m.roughness = std::clamp(c.value("roughness", 0.8f), 0.0f, 1.0f);
        m.emissive = vec3(c, "emissive", glm::vec3(0.0f));
        m.emissive_strength = std::max(c.value("emissive_strength", 1.0f), 0.0f);
        m.normal_scale = c.value("normal_scale", 1.0f);
        m.occlusion_strength = std::clamp(c.value("occlusion_strength", 1.0f), 0.0f, 1.0f);
        if (const auto t = c.find("textures"); t != c.end() && t->is_object()) {
            const char* slots[5] = {"base_color", "metallic_roughness", "normal", "occlusion",
                                    "emissive"};
            for (int i = 0; i < 5; ++i) m.textures[size_t(i)] = t->value(slots[i], "");
        }
        reg.emplace_or_replace<Material>(e, m);
    } else if (name == "camera") {
        CameraLens lens{c.value("fov", 55.0f), c.value("near", 0.05f), c.value("far", 500.0f)};
        CameraRigSettings& rig = lens.rig;
        rig.third.fov = lens.fov;
        rig.blend = std::clamp(c.value("blend", rig.blend), 0.0f, 10.0f);
        if (const auto v = view_from_name(c.value("view", std::string("third")))) rig.start = *v;
        if (const auto views = c.find("views"); views != c.end() && views->is_object()) {
            for (auto [key, settings] :
                 {std::pair{"first", &rig.first}, std::pair{"third", &rig.third},
                  std::pair{"top", &rig.top}}) {
                const auto it = views->find(key);
                if (it == views->end() || !it->is_object()) continue;
                const json& v = *it;
                ViewSettings& s = *settings;
                s.fov = std::clamp(v.value("fov", s.fov), 10.0f, 150.0f);
                if (v.contains("distance") && settings == &rig.third) rig.third_distance_set = true;
                s.distance = std::max(v.value("distance", s.distance), 0.0f);
                s.min_distance = std::max(v.value("min_distance", s.min_distance), 0.0f);
                s.max_distance = std::max(v.value("max_distance", s.max_distance), s.min_distance);
                s.height = v.value("height", s.height);
                s.shoulder = v.value("shoulder", s.shoulder);
                s.min_pitch = std::clamp(v.value("min_pitch", s.min_pitch), -89.0f, 89.0f);
                s.max_pitch = std::clamp(v.value("max_pitch", s.max_pitch), s.min_pitch, 89.0f);
                s.radius = std::clamp(v.value("radius", s.radius), 0.0f, 5.0f);
                s.smoothing = std::clamp(v.value("smoothing", s.smoothing), 0.0f, 5.0f);
            }
        }
        lens.fov = rig.third.fov;  // (views.third.fov, if given, wins over "fov")
        reg.emplace_or_replace<CameraLens>(e, lens);
    } else if (name == "player") {
        Player p;
        p.speed = std::max(c.value("speed", p.speed), 0.0f);
        p.run = std::clamp(c.value("run_multiplier", p.run), 1.0f, 5.0f);
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
    } else if (name == "character") {
        CharacterRef ch;
        ch.model = c.value("model", std::string());
        ch.clips = c.value("clips", std::string());
        ch.clip = c.value("clip", std::string());
        ch.speed = std::clamp(c.value("speed", 1.0f), 0.0f, 10.0f);
        ch.loop = c.value("loop", true);
        if (const auto rig = c.find("rig"); rig != c.end() && rig->is_object())
            for (const auto& [bone, joint] : rig->items())
                if (joint.is_string()) ch.rig[bone] = joint.get<std::string>();
        reg.emplace_or_replace<CharacterRef>(e, ch);
    } else if (name == "arrival") {
        reg.emplace_or_replace<Arrival>(e, Arrival{c.value("name", std::string())});
    } else if (name == "scene_exit") {
        SceneExit x;
        x.scene = c.value("scene", std::string());
        x.arrival = c.value("arrival", std::string());
        const std::string how = c.value("activation", std::string("enter"));
        if (how == "use")
            x.activation = ExitActivation::Use;
        else if (how == "script")
            x.activation = ExitActivation::Script;
        else if (how != "enter")
            throw SceneError("a scene exit's activation is enter, use or script, not \"" + how +
                             "\"");
        x.size = glm::max(vec3(c, "size", x.size), glm::vec3(0.01f));
        x.fade = std::clamp(c.value("fade", x.fade), 0.0f, 10.0f);
        reg.emplace_or_replace<SceneExit>(e, x);
    } else if (name == "sky") {
        reg.emplace_or_replace<Sky>(e, vec3(c, "color", SceneSettings{}.sky),
                                    vec3(c, "ambient", SceneSettings{}.ambient),
                                    c.value("environment", std::string()));
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
    } else if (name == "group") {
        reg.emplace_or_replace<Group>(e);
    } else if (name == "object") {
        reg.emplace_or_replace<ObjectRef>(e, c.at("id").get<std::string>());
    } else if (name == "physics_material") {
        reg.emplace_or_replace<PhysicsMaterial>(
            e, std::max(c.value("friction", 0.5f), 0.0f),
            std::clamp(c.value("bounciness", 0.0f), 0.0f, 1.0f));
    } else if (name == "light") {
        const std::string type = c.value("type", "directional");
        reg.remove<DirectionalLight, LocalLightSource>(e);
        if (type == "directional") {
            reg.emplace<DirectionalLight>(e, vec3(c, "color", glm::vec3(1.0f)),
                                          c.value("intensity", 1.0f),
                                          c.value("follows_clock", false));
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
    else if (name == "arrival")
        reg.remove<Arrival>(e);
    else if (name == "character")
        reg.remove<CharacterRef>(e);
    else if (name == "scene_exit")
        reg.remove<SceneExit>(e);
    else if (name == "sky")
        reg.remove<Sky>(e);
    else if (name == "rigidbody")
        reg.remove<RigidBody>(e);
    else if (name == "collider")
        reg.remove<ColliderShape>(e);
    else if (name == "physics_material")
        reg.remove<PhysicsMaterial>(e);
    else if (name == "group")
        reg.remove<Group>(e);
    else if (name == "object")
        reg.remove<ObjectRef>(e);
}

void add_components(entt::registry& reg, entt::entity e, const json& c) {
    if (!c.is_object()) throw SceneError("components must be a JSON object");
    reg.remove<Transform, MeshRef, Material, CameraLens, DirectionalLight, LocalLightSource, Player,
               ScriptRef, Trigger, Arrival, SceneExit, CharacterRef, Sky, RigidBody, ColliderShape,
               PhysicsMaterial, Group, ObjectRef>(e);
    reg.emplace<Transform>(e);
    for (const auto& [name, value] : c.items()) add_component(reg, e, name, value);
}

// A world type's version's recipe (migration 12).
WorldRecipe world_recipe(sqlite3* db, const std::string& world_type, int version) {
    if (!has_column(db, "world_type_version", "recipe"))
        throw SceneError("terrain names a world type, but the database has none");
    Stmt s = prepare(db,
                     "SELECT recipe FROM world_type_version WHERE world_type = ? AND "
                     "version = ?");
    sqlite3_bind_text(s.get(), 1, world_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s.get(), 2, version);
    if (sqlite3_step(s.get()) != SQLITE_ROW)
        throw SceneError("world type '" + world_type + "' has no version " +
                         std::to_string(version));
    try {
        return recipe_from_json(json::parse(column_text(s.get(), 0)));
    } catch (const SceneError&) {
        throw;
    } catch (const std::exception& e) {
        throw SceneError("world type '" + world_type + "' version " + std::to_string(version) +
                         ": " + e.what());
    }
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
        if (settings.contains("terrain")) {
            const json& terrain = settings.at("terrain");
            if (is_world_ref(terrain)) {  // a world type's version, and a seed (migration 12)
                out.world = world_ref_from_json(terrain);
                out.recipe = world_recipe(db, out.world->world_type, out.world->version);
                out.terrain = world_terrain(*out.recipe, *out.world);
            } else {  // the settings themselves (packages from before)
                out.terrain = terrain_from_json(terrain);
            }
        }
        if (const auto mode = sun_mode_from_name(settings.value("sun_mode", "uniform")))
            out.sun_mode = *mode;
        else
            throw SceneError("sun_mode is \"uniform\" or \"latitude\"");
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

const char* exit_activation_name(ExitActivation a) {
    switch (a) {
        case ExitActivation::Enter:
            return "enter";
        case ExitActivation::Use:
            return "use";
        case ExitActivation::Script:
            return "script";
    }
    return "?";
}

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
    // Environments (migration 10).
    if (has_column(db.get(), "environment", "curves")) {
        Stmt v = prepare(db.get(), "SELECT id, name, curves, settings FROM environment");
        while (sqlite3_step(v.get()) == SQLITE_ROW) {
            const std::string id = column_text(v.get(), 0);
            try {
                scene.environments.emplace(
                    id, environment_from_json(id, column_text(v.get(), 1), column_text(v.get(), 2),
                                              column_text(v.get(), 3)));
            } catch (const std::exception& ex) {
                throw SceneError(ex.what());
            }
        }
    }
    // Terrain stamps (migration 11): the scene's sculpting and painting, in order.
    if (has_column(db.get(), "terrain_stamp", "stamp")) {
        Stmt st = prepare(db.get(), "SELECT stamp FROM terrain_stamp WHERE scene = ? ORDER BY id");
        sqlite3_bind_text(st.get(), 1, scene_id.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st.get()) == SQLITE_ROW) {
            try {
                scene.terrain_stamps.push_back(
                    stamp_from_json(json::parse(column_text(st.get(), 0))));
            } catch (const std::exception& ex) {
                throw SceneError(std::string("terrain stamp: ") + ex.what());
            }
        }
    }
    // Object templates (migration 6), then every instance filled in.
    if (has_column(db.get(), "object", "parts")) {
        Stmt o = prepare(db.get(), "SELECT id, parts FROM object ORDER BY id");
        while (sqlite3_step(o.get()) == SQLITE_ROW)
            scene.objects.emplace(column_text(o.get(), 0), column_text(o.get(), 1));
    }
    std::vector<entt::entity> instances;
    for (entt::entity e : scene.registry.view<const ObjectRef>()) instances.push_back(e);
    std::sort(instances.begin(), instances.end());
    for (entt::entity e : instances) scene.expand(e);
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
    expand(e);
    apply_sky(e);
    return e;
}

void Scene::expand(entt::entity e) {
    // The old parts go (they are made from the template, never edited on their own).
    std::vector<entt::entity> old;
    for (entt::entity p : registry.view<const ObjectPart>())
        if (registry.get<ObjectPart>(p).instance == e) old.push_back(p);
    for (entt::entity p : old) registry.destroy(p);
    const auto* ref = registry.try_get<ObjectRef>(e);
    if (!ref) return;
    const auto it = objects.find(ref->id);
    if (it == objects.end()) return;  // an unknown object: nothing to show (cooking warns)
    const std::string& instance_id = registry.get<EntityInfo>(e).id;
    json parts;
    try {
        parts = json::parse(it->second);
    } catch (const std::exception& ex) {
        throw SceneError("object '" + ref->id + "': " + ex.what());
    }
    if (!parts.is_array()) throw SceneError("object '" + ref->id + "': parts must be a list");
    for (size_t i = 0; i < parts.size(); ++i) {
        const json& part = parts[i];
        const entt::entity p = registry.create();
        registry.emplace<EntityInfo>(p, instance_id + "/" + std::to_string(i),
                                     part.value("name", "part"), e);
        registry.emplace<ObjectPart>(p, e);
        try {
            add_components(registry, p, part.value("components", json::object()));
        } catch (const std::exception& ex) {
            throw SceneError("object '" + ref->id + "', part " + std::to_string(i) + ": " +
                             ex.what());
        }
    }
}

std::vector<entt::entity> Scene::set_object(const std::string& object_id,
                                            const std::string& parts_json) {
    objects[object_id] = parts_json;
    std::vector<entt::entity> instances;
    for (entt::entity e : registry.view<const ObjectRef>())
        if (registry.get<ObjectRef>(e).id == object_id) instances.push_back(e);
    std::sort(instances.begin(), instances.end());
    for (entt::entity e : instances) expand(e);
    return instances;
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
    if (component == "object") expand(e);
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
    registry.remove<ObjectRef>(e);
    expand(e);  // an instance's parts go with it
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
        d.normal_scale = m->normal_scale;
        d.occlusion_strength = m->occlusion_strength;
    }
    // The world's surface casts no shadow: inside a cylinder or a hollow world its far side
    // would shade everything (inside worlds get their own light in milestone 5.8).
    if (const MeshRef* r = registry.try_get<MeshRef>(e); r && r->asset == kBuiltinSurface)
        d.casts_shadow = false;
    return d;
}

const char* view_name(CameraView view) {
    switch (view) {
        case CameraView::First:
            return "first";
        case CameraView::Top:
            return "top";
        default:
            return "third";
    }
}

std::optional<CameraView> view_from_name(const std::string& name) {
    if (name == "first") return CameraView::First;
    if (name == "third") return CameraView::Third;
    if (name == "top") return CameraView::Top;
    return std::nullopt;
}

std::optional<LocalSun> Scene::local_sun(const glm::dvec3& origin) const {
    if (!solar_phase) return std::nullopt;
    const entt::entity e = first_with<DirectionalLight>(registry);
    if (e == entt::null) return std::nullopt;
    if (!registry.get<DirectionalLight>(e).follows_clock && !active_environment())
        return std::nullopt;
    // As placed, the light shines along its -Z: the sun is the other way, at its noon.
    const glm::dvec3 placed =
        -glm::normalize(glm::dvec3(world_matrix(e) * glm::dvec4(0, 0, -1, 0)));
    SunClock clock = sun_clock;
    clock.time = time_of_phase(*solar_phase, clock.daylight, clock.noon);
    clock.mode = settings.sun_mode;
    return midgard::local_sun(settings.body, placed, clock, origin);
}

float Scene::horizon_dip(const glm::dvec3& origin) const {
    // Above an outside sphere the ground falls away: its edge is acos(R / (R + h)) below level.
    if (!settings.body || settings.body->shape.kind() != Shape::Kind::SphereOutside) return 0.0f;
    const double r = settings.body->shape.radius();
    const double h = std::max(settings.body->surface(origin).height, 0.0);
    return float(std::acos(std::clamp(r / (r + h), -1.0, 1.0)));
}

const EnvironmentDef* Scene::active_environment() const {
    const entt::entity e = first_with<Sky>(registry);
    if (e == entt::null) return nullptr;
    const std::string& link = registry.get<Sky>(e).environment;
    if (link.empty()) return nullptr;
    const auto it = environments.find(link);
    return it == environments.end() ? nullptr : &it->second;
}

render::Environment Scene::environment(const glm::dvec3& origin) const {
    render::Environment out;
    out.water.time = float(std::fmod(water_time, 3600.0));  // (an hour, then the waves repeat)
    const EnvironmentDef* env = active_environment();
    if (!env || !solar_phase) {
        // A flat sky: the sky's colour everywhere, with the scene's sun as a small white disc in
        // it (the same colour as the frame's clear colour, so only the disc shows).
        if (first_with<DirectionalLight>(registry) == entt::null) return out;
        out.sky = true;
        out.horizon_dip = horizon_dip(origin);
        render::SkyColors& s = out.sky_colors;
        s.zenith = s.horizon = s.horizon_away = s.ground = settings.sky;
        s.brightness = 1.0f;
        s.glow_brightness = 0.0f;
        s.sun = glm::vec3(1.0f);
        s.sun_brightness = 20.0f;
        s.sun_size = EnvironmentDef{}.sun_size;
        return out;
    }
    const std::optional<LocalSun> sun = local_sun(origin);
    const double phase = sun ? sun->phase : *solar_phase;
    const float height = sun ? std::clamp(float(sun->height), -1.0f, 1.0f) : 1.0f;
    const double season = sun ? sun->season : sun_clock.season;
    const EnvironmentLook look = evaluate(*env, phase, std::asin(height), season);
    out.sky = true;
    out.sky_colors = look.sky;
    out.horizon_dip = horizon_dip(origin);
    out.water = look.water;
    out.water.time = float(std::fmod(water_time, 3600.0));
    out.fog.density = look.fog_density;
    out.fog.height_falloff = env->fog_height_falloff;
    out.fog.sun_scatter = env->fog_sun_scatter;
    if (look.fog_color) {
        out.fog.color = *look.fog_color;
        out.fog.color_from_sky = false;
    }
    // Fog lies on the ground: the camera's height above it, along "up".
    out.fog.camera_height =
        settings.body ? float(settings.body->surface(origin).height) : float(origin.y);
    return out;
}

std::optional<render::Camera> Scene::camera(float aspect) const {
    const entt::entity e = first_with<CameraLens>(registry);
    if (e == entt::null) return std::nullopt;
    const CameraLens& lens = registry.get<CameraLens>(e);
    render::Camera cam;
    cam.origin = world_position(e);
    // Cameras look along their local -Z; relative to their own origin they only turn.
    cam.view = glm::inverse(render::relative_to(world_matrix(e), cam.origin));
    // With terrain, the camera sees as far as it is drawn.
    const float far_plane = settings.body && settings.terrain
                                ? std::max(lens.far_plane, kTerrainViewDistance)
                                : lens.far_plane;
    cam.projection =
        render::perspective(glm::radians(lens.fov), aspect, lens.near_plane, far_plane);
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
        const EnvironmentDef* env = active_environment();
        if (const std::optional<LocalSun> sun = local_sun(origin)) {
            // Where the world's light model puts the sun, seen from here (midgard/sunlight.h).
            light.direction = -glm::vec3(sun->to_sun);
            // Twilight here: full light a little above the horizon, none a little below it.
            const float height = float(sun->height);
            const float t = std::clamp((height + 0.1f) / 0.2f, 0.0f, 1.0f);
            const float day = t * t * (3.0f - 2.0f * t);
            light.ambient *= 0.25f + 0.75f * day;
            light.ambient_ground *= 0.25f + 0.75f * day;
            if (env) {  // the environment's sunlight and ambient light at this time of day
                const EnvironmentLook look = evaluate(
                    *env, sun->phase, std::asin(std::clamp(height, -1.0f, 1.0f)), sun->season);
                light.color = l.color * look.sunlight;
                light.intensity = l.intensity * look.sunlight_intensity;
                if (look.ambient) light.ambient = *look.ambient;
                if (look.ambient_ground) light.ambient_ground = *look.ambient_ground;
                light.ambient *= look.ambient_tint;
                light.ambient_ground *= look.ambient_tint;
            } else {
                light.intensity *= day;
            }
        }
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
