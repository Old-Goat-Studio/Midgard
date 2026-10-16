// SPDX-License-Identifier: Apache-2.0
#include "midgard/scene_physics.h"

#include <algorithm>
#include <cmath>

#include <sqlite3.h>

namespace midgard {

namespace {

bool table_exists(sqlite3* db, const char* table) {
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?", -1, &s,
                       nullptr);
    sqlite3_bind_text(s, 1, table, -1, SQLITE_STATIC);
    const bool found = sqlite3_step(s) == SQLITE_ROW;
    sqlite3_finalize(s);
    return found;
}

// Each column of a 3x3 matrix divided by its length: the rotation without the scale.
glm::quat rotation_of(const glm::dmat4& m, const glm::dvec3& scale) {
    glm::mat3 r;
    for (int i = 0; i < 3; ++i)
        r[i] = glm::vec3(glm::dvec3(m[i]) / (scale[i] > 0.0 ? scale[i] : 1.0));
    return glm::normalize(glm::quat_cast(r));
}

// A rotation that turns +Y to `up` (the shortest way).
glm::quat upright(const glm::vec3& up) {
    return glm::quat(glm::vec3(0.0f, 1.0f, 0.0f), glm::normalize(up));
}

}  // namespace

glm::vec3 euler_degrees(const glm::quat& rotation) {
    const glm::mat3 m = glm::mat3_cast(glm::normalize(rotation));
    // R = Ry * Rx * Rz: its third row's middle is -sin(pitch) (glm keeps columns: m[col][row]).
    const float sx = std::clamp(-m[2][1], -1.0f, 1.0f);
    const float pitch = std::asin(sx);
    float yaw, roll;
    if (std::abs(sx) < 0.99999f) {
        yaw = std::atan2(m[2][0], m[2][2]);
        roll = std::atan2(m[0][1], m[1][1]);
    } else {  // looking straight up or down: yaw and roll turn about the same axis
        yaw = std::atan2(-m[0][2], m[0][0]);
        roll = 0.0f;
    }
    return glm::degrees(glm::vec3(pitch, yaw, roll));
}

PhysicsLayers PhysicsLayers::load(const std::filesystem::path& database) {
    PhysicsLayers out;
    out.names[0] = "default";
    sqlite3* db = nullptr;
    const std::u8string u8 = database.u8string();
    if (sqlite3_open_v2(std::string(u8.begin(), u8.end()).c_str(), &db, SQLITE_OPEN_READONLY,
                        nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        throw SceneError("cannot open " + database.string());
    }
    if (table_exists(db, "physics_layer")) {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db, "SELECT id, name FROM physics_layer", -1, &s, nullptr);
        while (sqlite3_step(s) == SQLITE_ROW) {
            const int id = sqlite3_column_int(s, 0);
            const auto* name = sqlite3_column_text(s, 1);
            if (id >= 0 && id < physics::kLayers && name)
                out.names[size_t(id)] = reinterpret_cast<const char*>(name);
        }
        sqlite3_finalize(s);
        sqlite3_prepare_v2(db, "SELECT a, b FROM physics_ignore ORDER BY a, b", -1, &s, nullptr);
        while (sqlite3_step(s) == SQLITE_ROW)
            out.ignored.emplace_back(sqlite3_column_int(s, 0), sqlite3_column_int(s, 1));
        sqlite3_finalize(s);
    }
    sqlite3_close(db);
    return out;
}

ScenePhysics::ScenePhysics(const Scene& scene, const PhysicsLayers& layers, MeshLoader load_mesh)
    : world_(std::make_unique<physics::World>(scene.settings.body)),
      load_mesh_(std::move(load_mesh)) {
    for (const auto& [a, b] : layers.ignored)
        if (a >= 0 && b >= 0 && a < physics::kLayers && b < physics::kLayers)
            world_->set_collides(a, b, false);
    // In entity order, so a scene builds the same world every time.
    std::vector<entt::entity> entities;
    for (entt::entity e : scene.registry.view<const EntityInfo>()) entities.push_back(e);
    std::sort(entities.begin(), entities.end());
    for (entt::entity e : entities) add(scene, e);
    make_character(scene);
}

// The scene's first player walks as a character; its own collider and rigid body, if it has
// any, are left out (the character is its body).
void ScenePhysics::make_character(const Scene& scene) {
    character_.reset();
    player_ = entt::null;
    for (entt::entity e : scene.registry.view<const Player>())
        if (player_ == entt::null || entt::to_integral(e) < entt::to_integral(player_)) player_ = e;
    if (player_ == entt::null) return;
    const Player& p = scene.registry.get<Player>(player_);
    physics::CharacterDesc d;
    d.radius = p.radius;
    d.height = p.height;
    d.max_slope = p.max_slope;
    d.step_height = p.step_height;
    d.user = entt::to_integral(player_);
    player_placed_ = placement(scene, player_);
    d.position = player_placed_.position;
    try {
        character_ = std::make_unique<physics::Character>(*world_, d);
    } catch (const std::exception& ex) {
        throw SceneError("entity '" + scene.registry.get<EntityInfo>(player_).id +
                         "': player: " + ex.what());
    }
}

void ScenePhysics::set_player_intent(const glm::vec3& walk, bool jump) {
    walk_ = walk;
    jump_ = jump_ || jump;
}

// An entity's new world position (and rotation) from physics, in its parent's space.
void ScenePhysics::write_back(Scene& scene, entt::entity e, const glm::dvec3& position,
                              const glm::quat* rotation) {
    Transform& tf = scene.registry.get<Transform>(e);
    const entt::entity parent = scene.registry.get<EntityInfo>(e).parent;
    if (parent == entt::null) {
        tf.position = position;
        if (rotation) tf.rotation = euler_degrees(*rotation);
        return;
    }
    tf.position = glm::dvec3(glm::inverse(scene.world_matrix(parent)) * glm::dvec4(position, 1.0));
    if (rotation)
        tf.rotation = euler_degrees(glm::inverse(placement(scene, parent).rotation) * *rotation);
}

ScenePhysics::Placed ScenePhysics::placement(const Scene& scene, entt::entity e) const {
    const glm::dmat4 m = scene.world_matrix(e);
    const glm::dvec3 scale(glm::length(glm::dvec3(m[0])), glm::length(glm::dvec3(m[1])),
                           glm::length(glm::dvec3(m[2])));
    return {glm::dvec3(m[3]), rotation_of(m, scale), glm::vec3(scale)};
}

physics::Collider ScenePhysics::collider_for(const Scene& scene, entt::entity e) const {
    physics::Collider c;
    const ColliderShape& s = scene.registry.get<ColliderShape>(e);
    c.kind = s.kind;
    c.size = s.size;
    c.radius = s.radius;
    c.height = s.height;
    c.offset = s.offset;
    c.samples = s.samples;
    c.heights = s.heights;
    c.spacing = s.spacing;
    using Kind = physics::Collider::Kind;
    if (s.kind == Kind::Convex || s.kind == Kind::Mesh) {
        std::string asset = s.mesh;
        if (asset.empty())
            if (const auto* mesh = scene.registry.try_get<MeshRef>(e)) asset = mesh->asset;
        if (asset.empty()) throw SceneError("a convex or mesh collider needs a mesh");
        const render::MeshData data = load_mesh_(asset);
        for (const render::Vertex& v : data.vertices)
            c.points.emplace_back(v.position[0], v.position[1], v.position[2]);
        if (s.kind == Kind::Mesh) c.triangles = data.indices;
    }
    return c;
}

void ScenePhysics::add(const Scene& scene, entt::entity e) {
    if (scene.registry.all_of<Player>(e)) return;  // the player is a character instead
    const auto* shape = scene.registry.try_get<ColliderShape>(e);
    const auto* trigger = scene.registry.try_get<Trigger>(e);
    if (!shape && !trigger) return;
    const std::string& id = scene.registry.get<EntityInfo>(e).id;
    try {
        const Placed p = placement(scene, e);
        Tracked t{physics::kNoBody, physics::kNoBody, physics::Motion::Static, p};
        physics::BodyDesc d;
        d.position = p.position;
        d.rotation = p.rotation;
        d.scale = p.scale;
        d.user = entt::to_integral(e);
        if (const auto* rb = scene.registry.try_get<RigidBody>(e)) {
            d.motion = rb->motion;
            d.mass = rb->mass;
            d.layer = rb->layer;
        }
        t.motion = d.motion;
        if (shape) {  // the solid body: what collides
            d.collider = collider_for(scene, e);
            if (const auto* m = scene.registry.try_get<PhysicsMaterial>(e))
                d.surface = {m->friction, m->bounciness};
            t.id = world_->add(d);
        }
        if (trigger) {
            // The trigger: a box sensor of its own, moving with the entity (kinematic when the
            // entity moves, so it's carried along without colliding).
            physics::BodyDesc s = d;
            s.collider = {};
            s.collider.size = trigger->size;
            s.sensor = true;
            s.motion = shape && t.motion != physics::Motion::Static ? physics::Motion::Kinematic
                                                                    : physics::Motion::Static;
            s.surface = {};
            t.sensor = world_->add(s);
        }
        bodies_[e] = t;
    } catch (const SceneError& ex) {
        throw SceneError("entity '" + id + "': " + ex.what());
    } catch (const std::exception& ex) {  // PhysicsError, AssetError
        throw SceneError("entity '" + id + "': " + ex.what());
    }
}

void ScenePhysics::rebuild(const Scene& scene, entt::entity e) {
    remove(e);
    if (scene.registry.valid(e)) add(scene, e);
    if (e == player_ || (scene.registry.valid(e) && scene.registry.all_of<Player>(e)))
        make_character(scene);
}

void ScenePhysics::remove(entt::entity e) {
    if (e == player_) {
        character_.reset();
        player_ = entt::null;
    }
    const auto it = bodies_.find(e);
    if (it == bodies_.end()) return;
    for (physics::BodyId id : {it->second.id, it->second.sensor})
        if (id != physics::kNoBody) world_->remove(id);
    bodies_.erase(it);
}

physics::BodyId ScenePhysics::body(entt::entity e) const {
    const auto it = bodies_.find(e);
    if (it == bodies_.end()) return physics::kNoBody;
    return it->second.id != physics::kNoBody ? it->second.id : it->second.sensor;
}

int ScenePhysics::update(Scene& scene, double dt) {
    // Bodies follow their entities wherever the game or a script moved them.
    std::vector<entt::entity> gone, rescaled;
    for (auto& [e, t] : bodies_) {
        if (!scene.registry.valid(e) || !scene.registry.all_of<Transform>(e)) {
            gone.push_back(e);
            continue;
        }
        const Placed now = placement(scene, e);
        if (now.position == t.placed.position && now.rotation == t.placed.rotation &&
            now.scale == t.placed.scale)
            continue;
        if (now.scale != t.placed.scale) {
            rescaled.push_back(e);
            continue;
        }
        for (physics::BodyId id : {t.id, t.sensor})
            if (id != physics::kNoBody) world_->move_to(id, now.position, now.rotation);
        t.placed = now;
    }
    for (entt::entity e : gone) remove(e);
    for (entt::entity e : rescaled) rebuild(scene, e);
    // The player moved by something other than walking (a script, the editor): it jumps there.
    if (character_ && player_ != entt::null && scene.registry.valid(player_)) {
        const Placed now = placement(scene, player_);
        if (now.position != player_placed_.position) character_->set_position(now.position);
        player_placed_ = now;
    }

    const float jump_speed = player_ != entt::null && scene.registry.valid(player_) &&
                                     scene.registry.all_of<Player>(player_)
                                 ? scene.registry.get<Player>(player_).jump
                                 : 0.0f;
    const int steps = world_->step(dt, [&] {
        if (!character_) return;
        character_->step(walk_, jump_ ? jump_speed : 0.0f);
        jump_ = false;
    });
    if (steps == 0) return 0;
    if (character_ && scene.registry.valid(player_)) {
        write_back(scene, player_, character_->position(), nullptr);
        player_placed_ = placement(scene, player_);
    }

    // Dynamic bodies move their entities (in their parents' space, if they have parents).
    for (auto& [e, t] : bodies_) {
        if (t.motion != physics::Motion::Dynamic || t.id == physics::kNoBody) continue;
        const physics::BodyState s = world_->state(t.id);
        write_back(scene, e, s.position, &s.rotation);
        t.placed = placement(scene, e);
        // Its trigger goes with it (by the end of the next step).
        if (t.sensor != physics::kNoBody)
            world_->move_to(t.sensor, t.placed.position, t.placed.rotation);
    }
    return steps;
}

std::vector<entt::entity> ScenePhysics::triggers_touching(const Scene& scene,
                                                          entt::entity player) const {
    std::vector<entt::entity> out;
    if (player == entt::null || !scene.registry.valid(player)) return out;
    physics::Collider capsule;
    capsule.kind = physics::Collider::Kind::Capsule;
    Player p;
    if (const auto* own = scene.registry.try_get<Player>(player)) p = *own;
    capsule.radius = p.radius;
    capsule.height = p.height;
    const glm::dvec3 at = scene.world_position(player);
    const glm::quat turn =
        upright(glm::vec3(scene.settings.body ? scene.settings.body->up(at) : glm::dvec3(0, 1, 0)));
    for (std::uint64_t user :
         world_->overlaps(capsule, at, turn, glm::vec3(1.0f), physics::Touching::Sensors)) {
        const auto e = static_cast<entt::entity>(user);
        if (e != player && scene.registry.valid(e) && scene.registry.all_of<Trigger>(e))
            out.push_back(e);
    }
    return out;
}

}  // namespace midgard
