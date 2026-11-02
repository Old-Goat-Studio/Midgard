// SPDX-License-Identifier: Apache-2.0
#include "midgard/game.h"

#include "midgard/scene_nav.h"

#include <algorithm>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <sqlite3.h>

#include "midgard/collider_outlines.h"
#include "midgard/gltf.h"
#include "midgard/world_surface.h"

namespace midgard {

Game::Game(const std::filesystem::path& package_dir)
    : package_(open_package(package_dir)),
      scene_(Scene::load(package_dir / package_.manifest.database, package_.manifest.entry_scene)) {
    const auto pak_file = package_dir / "game.pak";
    if (std::filesystem::is_regular_file(pak_file)) pak_ = std::make_unique<Pak>(pak_file);
    layers_ = PhysicsLayers::load(package_dir / package_.manifest.database);
    rebuild_physics();
    rebuild_terrain();
    camera_ = std::make_unique<CameraRig>(scene_);
    camera_->follow(scene_, 0.0f, camera_sweep());
    note_exits_inside();

    // The clock, on the standard calendar (validation refused unusable calendars at the cook).
    const std::filesystem::path database = package_dir / package_.manifest.database;
    std::vector<Calendar> calendars;
    try {
        calendars = load_calendars(database);
    } catch (const CalendarError&) {
    }
    const auto standard = std::find_if(calendars.begin(), calendars.end(),
                                       [](const Calendar& c) { return c.standard; });
    if (standard != calendars.end() && standard->problems().empty()) {
        time_.follow(*standard);
    } else {  // a package from before calendars: plain days and months, no seasons
        Calendar plain;
        plain.id = "builtin";
        plain.months.assign(12, {"", 30});
        plain.weekdays.assign(7, "");
        time_.follow(plain);
    }
    time_.set_time(standard_calendar().start_time());
    time_.set_scale(standard_calendar().time_scale());
    update_sun();

    sqlite3* db = nullptr;
    const std::u8string u8 = database.u8string();
    if (sqlite3_open_v2(std::string(u8.begin(), u8.end()).c_str(), &db, SQLITE_OPEN_READONLY,
                        nullptr) == SQLITE_OK) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT key, english FROM string", -1, &st, nullptr) ==
            SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                const auto* k = sqlite3_column_text(st, 0);
                const auto* e = sqlite3_column_text(st, 1);
                if (k && e)
                    strings_[reinterpret_cast<const char*>(k)] = reinterpret_cast<const char*>(e);
            }
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
}

std::string Game::text(const std::string& key) const {
    const auto it = strings_.find(key);
    return it != strings_.end() ? it->second : key;
}

void Game::advance_time(std::int64_t game_seconds, SkipPolicy policy) {
    const std::vector<TimeEvent>& passed = time_.advance(game_seconds, policy);
    skipped_.insert(skipped_.end(), passed.begin(), passed.end());
    update_sun();
}

void Game::set_time(std::int64_t time) {
    time_.set_time(time);
    update_sun();
}

void Game::update_sun() {
    const Calendar& c = local_calendar();
    const Date d = date_at(c, time_.now());
    // With the part of a second not yet on the clock, so the sun moves smoothly.
    const double seconds = double(d.seconds_into_day) + (time_.precise() - double(time_.now()));
    const double daylight = daylight_on(c, d.year, d.day_of_year);
    scene_.solar_phase = solar_phase(seconds, double(c.seconds_per_day()), daylight, c.solar_noon);
    // The rest of the clock for the world's light model (midgard/sunlight.h): the year starts at
    // the spring equinox; the tilt is the body's (or the middle of the calendar's range).
    scene_.sun_clock.daylight = daylight;
    scene_.sun_clock.noon = c.solar_noon;
    scene_.sun_clock.tilt = c.tilt != 0.0 ? c.tilt : 0.5 * (c.tilt_min + c.tilt_max);
    scene_.sun_clock.year = double(d.day_of_year) / double(std::max(c.year_days(d.year), 1));
    scene_.sun_clock.season = summer_on(c, d.year, d.day_of_year);  // for the season tints
    scene_.sun_clock.autumn = autumn_on(c, d.year, d.day_of_year);  // and the terrain's
}

void Game::use_world(const std::filesystem::path& folder) {
    world_ = WorldSave::open(folder, package_.manifest.game_name);
    if (const auto saved = world_->clock()) {
        time_.set_time(saved->game_time);
        time_.set_scale(saved->scale);
        update_sun();
    }
    // The player is put back where they were (the character follows on the next step); the
    // camera keeps its view, now round the player's new place.
    const auto place = world_->place();
    if (place && !place->scene.empty() && place->scene != scene_.id) {
        try {  // the scene they were in (where the scene puts them; their place follows)
            load_scene(place->scene, "");
        } catch (const std::exception&) {  // (a scene since deleted: the starting one)
        }
    }
    const entt::entity player = camera_->player();
    if (place && (place->scene.empty() || place->scene == scene_.id) && player != entt::null) {
        Transform& t = scene_.registry.get<Transform>(player);
        t.position = place->position;
        t.rotation = place->rotation;
        const CameraRig::State view = camera_->state();
        camera_ = std::make_unique<CameraRig>(scene_);
        camera_->restore(scene_, view);
        camera_->follow(scene_, 0.0f, camera_sweep());
        note_exits_inside();
    }
}

void Game::save_world() {
    if (!world_) return;
    world_->save_clock({time_.now(), time_.scale()});
    const entt::entity player = camera_->player();
    if (player != entt::null && scene_.registry.valid(player)) {
        const Transform& t = scene_.registry.get<Transform>(player);
        world_->save_place({t.position, t.rotation, scene_.id});
    }
}

void Game::rebuild_physics() {
    nav_stale_ = true;  // (the scenery may have changed too)
    ground_.reset();    // its bodies were in the old world
    physics_ = std::make_unique<ScenePhysics>(
        scene_, layers_, [this](const std::string& asset) { return load_mesh(asset); });
}

InputMap Game::input_map() const {
    return InputMap::load(package_.root / package_.manifest.database);
}

CameraSweep Game::camera_sweep() const {
    // (Not stopped by the player's own capsule.)
    const physics::BodyId player = camera_ ? physics_->body(camera_->player()) : physics::kNoBody;
    return [this, player](const glm::dvec3& from, const glm::vec3& direction, double distance,
                          float radius) {
        return physics_->world().sweep_sphere(from, direction, distance, radius,
                                              physics::Touching::Solid, player);
    };
}

void Game::update(const Actions& actions, float dt) {
    update_scene_change(dt);
    time_events_ = std::move(skipped_);
    skipped_.clear();
    const std::vector<TimeEvent>& passed = time_.update(dt);
    time_events_.insert(time_events_.end(), passed.begin(), passed.end());
    update_sun();
    scene_.water_time += dt;
    const PlayerIntent intent = camera_->steer(scene_, actions, dt);
    if (change_)  // (on the way to another scene: the player stands still)
        physics_->set_player_intent(glm::vec3(0.0f), false);
    else
        physics_->set_player_intent(intent.walk, intent.jump);
    update_ground();               // solid ground under the player before it moves
    physics_->update(scene_, dt);  // the player walks
    camera_->follow(scene_, dt, camera_sweep());
    update_triggers();
    update_exits(actions);
    if (!profile_file_.empty() && camera_->active() && camera_->view() != saved_view_) {
        try {
            save_profile();
        } catch (const ProfileError&) {
            saved_view_ = camera_->view();  // not retried every frame; the save at the end tries
        }
    }
}

void Game::use_profile(const std::filesystem::path& file) {
    profile_file_ = file;
    saved_view_ = camera_->view();
    profile_ = PlayerProfile::load(file);  // (if damaged, the profile stays empty)
    if (!camera_->active()) return;
    CameraRig::State state = camera_->state();
    if (profile_.third_distance) state.third_distance = *profile_.third_distance;
    if (profile_.top_distance) state.top_distance = *profile_.top_distance;
    camera_->restore(scene_, state);  // (the distances clamped to the camera's zoom range)
    if (profile_.view) camera_->set_view(scene_, *profile_.view, false);
    camera_->follow(scene_, 0.0f, camera_sweep());
    saved_view_ = camera_->view();
}

void Game::save_profile() {
    if (profile_file_.empty() || !camera_->active()) return;
    const CameraRig::State state = camera_->state();
    profile_.view = state.view;
    profile_.third_distance = state.third_distance;
    profile_.top_distance = state.top_distance;
    saved_view_ = state.view;
    profile_.save(profile_file_);
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
    world_light_.upload(renderer);
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
    nav_stale_ = true;
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
    physics_->rebuild(scene_, e);
    refresh_camera(e);
}

// After an edit the player or camera may be different entities, or the camera's settings
// changed. The player's view is kept; when the camera itself was edited, only which view it is
// (the rest starts again from the new settings, so the edit shows).
void Game::refresh_camera(entt::entity edited) {
    const CameraRig::State old = camera_->state();
    const bool was_active = camera_->active();
    camera_ = std::make_unique<CameraRig>(scene_);
    const bool camera_edited = edited != entt::null && scene_.registry.valid(edited) &&
                               scene_.registry.all_of<CameraLens>(edited);
    if (was_active && !camera_edited) {
        camera_->restore(scene_, old);
    } else if (was_active) {
        camera_->set_view(scene_, old.view, false);
    }
    camera_->follow(scene_, 0.0f, camera_sweep());
}

void Game::set_object(const std::string& object_id, const std::string& parts_json) {
    for (entt::entity e : scene_.set_object(object_id, parts_json)) physics_->rebuild(scene_, e);
}

void Game::set_component(const std::string& entity_id, const std::string& component,
                         const nlohmann::json& value) {
    nav_stale_ = true;
    const entt::entity e = scene_.find(entity_id);
    if (e == entt::null) throw SceneError("entity '" + entity_id + "' is not in the scene");
    scene_.set_component(e, component, value);
    physics_->rebuild(scene_, e);
    refresh_camera(e);
}

void Game::remove_entity(const std::string& entity_id) {
    nav_stale_ = true;
    physics_->remove(scene_.find(entity_id));
    scene_.remove_entity(entity_id);
    physics_->prune(scene_);  // an instance's parts went with it
    refresh_camera(entt::null);
}

void Game::reload_scene_settings() {
    scene_.settings = Scene::read_settings(package_.root / package_.manifest.database, scene_.id);
    scene_.use_sky();   // a Sky entity outranks the settings
    rebuild_physics();  // the world's shape may have changed
    rebuild_terrain();
}

void Game::rebuild_terrain() {
    if (terrain_ && terrain_renderer_) terrain_->release(*terrain_renderer_);
    terrain_.reset();
    place_again();  // (none without terrain)
    if (!scene_.settings.body || !scene_.settings.terrain) return;
    if (!jobs_) jobs_ = std::make_unique<JobSystem>();
    terrain_ = std::make_unique<TerrainStreamer>(*scene_.settings.body, *scene_.settings.terrain,
                                                 jobs_.get());
    terrain_->set_stamps(scene_.terrain_stamps);
    // Solid ground. A player who starts below it is lifted onto it.
    if (ground_) ground_->clear(physics_->world());
    ground_ = std::make_unique<TerrainCollision>(terrain_->terrain());
    const entt::entity player = physics_->player();
    if (player != entt::null && scene_.registry.valid(player) &&
        terrain_->terrain().above_ground(scene_.world_position(player)) < 0.0)
        put_on_ground(player);
    place_again();
    update_ground(1000);  // everything within reach of the start, now
}

void Game::place_again() {
    if (placed_) {
        placed_->clear(scene_, [this](entt::entity e) { physics_->remove(e); });
        physics_->prune(scene_);
        placed_.reset();
    }
    if (terrain_ && !terrain_->terrain().settings().placements.empty())
        placed_ = std::make_unique<PlacedObjects>(terrain_->terrain());
}

void Game::put_on_ground(entt::entity e) {
    // Standing on the terrain: the entity's origin (the player's capsule's middle) half its
    // height above the ground, whatever its parents (folders and groups) are.
    const Terrain& t = terrain_->terrain();
    const glm::dvec3 p = scene_.world_position(e);
    const double half =
        scene_.registry.all_of<Player>(e) ? 0.5 * scene_.registry.get<Player>(e).height : 0.0;
    const glm::dvec3 target = t.ground(p) + scene_.settings.body->up(p) * (half + 0.05);
    const entt::entity parent = scene_.registry.get<EntityInfo>(e).parent;
    glm::dvec3 local = target;
    if (parent != entt::null && scene_.registry.valid(parent))
        local = glm::dvec3(glm::inverse(scene_.world_matrix(parent)) * glm::dvec4(target, 1.0));
    scene_.registry.get<Transform>(e).position = local;
}

void Game::update_ground(int max_tiles) {
    if (!ground_) return;
    const entt::entity player = physics_->player();
    const bool has_player = player != entt::null && scene_.registry.valid(player);
    glm::dvec3 at = glm::dvec3(0.0);
    if (physics_->character())
        at = physics_->character()->position();
    else if (has_player)
        at = scene_.world_position(player);
    // A player found well under the ground (fallen through, or put there) is put back on it.
    if (has_player && terrain_->terrain().above_ground(at) < -1.0) {
        put_on_ground(player);
        at = scene_.world_position(player);
    }
    // Where the player is about to be, too (a teleport by a script or the editor).
    std::vector<glm::dvec3> around{at};
    if (has_player) around.push_back(scene_.world_position(player));
    ground_->update(physics_->world(), around);
    // The world type's objects round the player: solid as they come, gone well behind.
    if (placed_) {
        bool removed = false;
        placed_->update(
            scene_, at, max_tiles,
            [this](entt::entity e) {
                physics_->rebuild(scene_, e);
                placed_changed_ = true;
            },
            [this, &removed](entt::entity e) {
                physics_->remove(e);
                removed = true;
            });
        if (removed) physics_->prune(scene_);  // their parts went with them
    }
}

void Game::set_terrain_stamps(std::vector<TerrainStamp> stamps) {
    scene_.terrain_stamps = stamps;
    if (!terrain_) return;
    terrain_->set_stamps(std::move(stamps));
    // The solid ground follows: made again round the player.
    if (ground_) ground_->clear(physics_->world());
    ground_ = std::make_unique<TerrainCollision>(terrain_->terrain());
    place_again();  // they stand on the ground as it now is
    update_ground(1000);
}

void Game::stream_terrain(render::Renderer& renderer, const glm::dvec3& camera, bool wait) {
    terrain_renderer_ = &renderer;
    stream(&renderer, camera, wait);
}

void Game::stream_terrain(const glm::dvec3& camera, bool wait) {
    stream(nullptr, camera, wait);
}

void Game::stream(render::Renderer* renderer, const glm::dvec3& camera, bool wait) {
    if (stream_all_) {  // a scene just loaded: its meshes, and all of its ground in view
        if (renderer)
            for (const std::string& problem : upload_new_meshes(*renderer)) (void)problem;
        wait = true;
        stream_all_ = false;
    }
    if (terrain_) terrain_->update(camera, renderer, wait);
    if (placed_changed_ && renderer) {  // the placed objects' meshes, as they come
        upload_new_meshes(*renderer);
        placed_changed_ = false;
    }
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
    const entt::entity hidden = camera_->hides_player() ? camera_->player() : entt::null;
    for (entt::entity e : scene_.registry.view<const MeshRef>()) {
        if (e == hidden) continue;  // first person: the player's own model isn't drawn
        const MeshRef& ref = scene_.registry.get<MeshRef>(e);
        const double distance =
            ref.lods.empty() ? 0.0 : glm::distance(scene_.world_position(e), origin);
        auto it = meshes_.find(ref.at(distance));
        if (it == meshes_.end()) continue;  // not uploaded yet
        render::DrawItem d = scene_.draw_item(e, it->second, origin);
        if (const Material* m = scene_.registry.try_get<Material>(e)) materials_.apply(*m, d);
        out.push_back(d);
    }
    if (terrain_) {
        // The season where the camera is (a tilted planet's hemispheres differ).
        const std::optional<LocalSun> sun = scene_.local_sun(origin);
        terrain_->set_season(sun ? sun->season : scene_.sun_clock.season,
                             sun ? sun->autumn : scene_.sun_clock.autumn);
        std::vector<render::DrawItem> ground = terrain_->draws(origin);
        out.insert(out.end(), ground.begin(), ground.end());
    }
    if (const auto light = world_light_.draw(scene_, origin)) out.push_back(*light);
    return out;
}

render::TerrainLook Game::terrain_look() const {
    return terrain_ ? terrain_->look() : render::TerrainLook{};
}

const render::MeshData* Game::outline_mesh(const std::string& asset) const {
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
}

std::vector<render::DebugLine> Game::debug_lines(const glm::dvec3& origin) const {
    std::vector<render::DebugLine> lines = collider_outlines(
        scene_, origin, [this](const std::string& asset) { return outline_mesh(asset); });
    if (show_navigation_)
        if (const NavMesh* nav = navmesh()) {
            const glm::vec4 colour(0.2f, 0.95f, 0.75f, 1.0f);
            const glm::vec3 lift(0.0f, 0.05f, 0.0f);  // (just over the ground)
            for (const auto& [a, b] : nav->edges())
                lines.push_back({glm::vec3(glm::dvec3(a) - origin) + lift,
                                 glm::vec3(glm::dvec3(b) - origin) + lift, colour});
        }
    return lines;
}

const NavMesh* Game::navmesh() const {
    if (nav_stale_) {
        nav_stale_ = false;
        nav_.reset();
        if (flat_world(scene_)) {
            const NavGeometry g =
                nav_geometry(scene_, glm::dvec3(0.0),
                             [this](const std::string& asset) { return outline_mesh(asset); });
            nav_ = build_navmesh(NavSettings{}, g, jobs_.get());
        }
    }
    return nav_.get();
}

std::optional<NavMesh::Path> Game::find_path(const glm::dvec3& from, const glm::dvec3& to) const {
    const NavMesh* nav = navmesh();
    if (!nav) return std::nullopt;
    return nav->find_path(glm::vec3(from), glm::vec3(to));
}

render::Camera Game::camera(float aspect) const {
    if (auto cam = scene_.camera(aspect)) return *cam;
    render::Camera cam;
    cam.view = glm::lookAt(glm::vec3(4.0f, 3.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0, 1, 0));
    const float far_plane = std::max(terrain_ ? kTerrainViewDistance : 500.0f,
                                     float(inside_view_distance(scene_.settings.body)));
    cam.projection = render::perspective(glm::radians(55.0f), aspect, 0.05f, far_plane);
    return cam;
}

// ---- moving between scenes (milestone 5.11a) ---------------------------------------------------

void Game::change_scene(const std::string& scene, const std::string& arrival, float fade) {
    if (change_ && !change_->swapped) return;  // already on the way out
    // (Asked while fading in, say by a script's on_room_enter: it fades out again from as dark
    // as it is now.)
    SceneChange next{scene, arrival, std::clamp(fade, 0.0f, 10.0f)};
    next.t = change_ ? fade_ * next.fade : 0.0f;
    change_ = next;
    scene_change_error_.clear();
}

void Game::update_scene_change(float dt) {
    scene_changed_ = false;
    if (!change_) return;
    SceneChange& c = *change_;
    c.t += dt;
    const float part = c.fade > 0.0f ? std::min(c.t / c.fade, 1.0f) : 1.0f;
    if (!c.swapped) {
        fade_ = part;
        if (part < 1.0f) return;
        try {
            // (Checked first: the scene's scripts hear they are leaving only if they are.)
            if (const std::string problem = arrival_problem(c.scene, c.arrival); !problem.empty())
                throw SceneError(problem);
            const std::string left = scene_.id;
            if (leaving_) leaving_(c.scene);
            load_scene(c.scene, c.arrival);
            previous_scene_ = left;
            scene_changed_ = true;
        } catch (const std::exception& e) {  // (the cook checks exits; scripts may still ask)
            scene_change_error_ = e.what();
        }
        c.swapped = true;
        c.t = 0.0f;
        return;
    }
    fade_ = 1.0f - part;
    if (part >= 1.0f) {
        change_.reset();
        fade_ = 0.0f;
    }
}

void Game::load_scene(const std::string& id, const std::string& arrival) {
    // Everything that can fail first, so a failed change leaves the scene as it was.
    Scene next = Scene::load(package_.root / package_.manifest.database, id);
    std::optional<glm::dmat4> at;
    if (!arrival.empty()) {
        const entt::entity a = next.find(arrival);
        if (a == entt::null || !next.registry.all_of<Arrival>(a))
            throw SceneError("scene '" + id + "' has no arrival '" + arrival + "'");
        at = next.world_matrix(a);
    }
    const bool had_camera = camera_ && camera_->active();
    const CameraRig::State view = had_camera ? camera_->state() : CameraRig::State{};

    // Leave the scene: its ground, placed objects and physics go.
    if (terrain_ && terrain_renderer_) terrain_->release(*terrain_renderer_);
    terrain_.reset();
    placed_.reset();
    ground_.reset();
    inside_.clear();
    exits_inside_.clear();
    trigger_events_.clear();
    outline_meshes_.clear();
    scene_ = std::move(next);

    // The player at the arrival, facing the way it does (level: "up" comes from the world).
    entt::entity player = entt::null;
    for (entt::entity e : scene_.registry.view<const Player>()) player = e;
    if (at && player != entt::null) {
        glm::dmat4 parent(1.0);
        if (const auto* info = scene_.registry.try_get<EntityInfo>(player);
            info && info->parent != entt::null && scene_.registry.valid(info->parent))
            parent = scene_.world_matrix(info->parent);
        Transform& t = scene_.registry.get<Transform>(player);
        t.position = glm::dvec3(glm::inverse(parent) * (*at)[3]);
        const glm::dvec3 ahead = -glm::dvec3((*at)[2]);
        const glm::vec3 turn = look_rotation(glm::vec3(0.0f), glm::vec3(ahead));
        t.rotation = {0.0f, turn.y, 0.0f};
    }
    rebuild_physics();
    rebuild_terrain();  // (lifts the player onto the ground there, and makes it solid)
    camera_ = std::make_unique<CameraRig>(scene_);
    if (had_camera && camera_->active()) {  // the same view, behind the player as it faces
        CameraRig::State s = view;
        s.yaw = camera_->facing_yaw(scene_);
        camera_->restore(scene_, s);
    }
    camera_->follow(scene_, 0.0f, camera_sweep());
    update_sun();
    note_exits_inside();
    stream_all_ = true;
}

void Game::note_exits_inside() {
    // Exits the player starts or arrives inside wait until they have stepped out.
    exits_inside_.clear();
    for (entt::entity player : scene_.registry.view<const Player>())
        for (entt::entity e : scene_.registry.view<const SceneExit>())
            if (inside_exit(e, scene_.world_position(player))) exits_inside_.insert(e);
}

bool Game::inside_exit(entt::entity exit, const glm::dvec3& p) const {
    const SceneExit& x = scene_.registry.get<SceneExit>(exit);
    const glm::dvec3 local =
        glm::dvec3(glm::inverse(scene_.world_matrix(exit)) * glm::dvec4(p, 1.0));
    const glm::dvec3 half = glm::dvec3(x.size) * 0.5;
    return std::abs(local.x) <= half.x && std::abs(local.y) <= half.y &&
           std::abs(local.z) <= half.z;
}

bool Game::take_exit(entt::entity exit) {
    if (!scene_.registry.valid(exit) || !scene_.registry.all_of<SceneExit>(exit)) return false;
    const SceneExit& x = scene_.registry.get<SceneExit>(exit);
    if (x.scene.empty()) return false;
    change_scene(x.scene, x.arrival, x.fade);
    return true;
}

bool Game::has_scene(const std::string& scene) const {
    bool found = false;
    sqlite3* db = nullptr;
    const std::u8string u8 = (package_.root / package_.manifest.database).u8string();
    if (sqlite3_open_v2(std::string(u8.begin(), u8.end()).c_str(), &db, SQLITE_OPEN_READONLY,
                        nullptr) == SQLITE_OK) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT 1 FROM scene WHERE id = ?", -1, &st, nullptr) ==
            SQLITE_OK) {
            sqlite3_bind_text(st, 1, scene.c_str(), -1, SQLITE_TRANSIENT);
            found = sqlite3_step(st) == SQLITE_ROW;
            sqlite3_finalize(st);
        }
    }
    sqlite3_close(db);
    return found;
}

std::string Game::arrival_problem(const std::string& scene, const std::string& arrival) const {
    if (!has_scene(scene)) return "there is no scene '" + scene + "' in the game";
    if (arrival.empty()) return "";
    bool found = false;
    sqlite3* db = nullptr;
    const std::u8string u8 = (package_.root / package_.manifest.database).u8string();
    if (sqlite3_open_v2(std::string(u8.begin(), u8.end()).c_str(), &db, SQLITE_OPEN_READONLY,
                        nullptr) == SQLITE_OK) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT components FROM entity WHERE id = ? AND scene = ?", -1,
                               &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, arrival.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, scene.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_ROW) {
                const auto* text = sqlite3_column_text(st, 0);
                const nlohmann::json c = nlohmann::json::parse(
                    text ? reinterpret_cast<const char*>(text) : "", nullptr, false);
                found = c.is_object() && c.contains("arrival");
            }
            sqlite3_finalize(st);
        }
    }
    sqlite3_close(db);
    return found ? "" : "scene '" + scene + "' has no arrival '" + arrival + "'";
}

void Game::update_exits(const Actions& actions) {
    if (change_) return;
    entt::entity player = entt::null;
    for (entt::entity e : scene_.registry.view<const Player>()) player = e;
    if (player == entt::null) return;
    const glm::dvec3 p = scene_.world_position(player);
    for (entt::entity e : scene_.registry.view<const SceneExit>()) {
        const bool inside = inside_exit(e, p);
        const bool was = exits_inside_.contains(e);
        const SceneExit& x = scene_.registry.get<SceneExit>(e);
        if (inside && !was) {
            exits_inside_.insert(e);
            if (x.activation == ExitActivation::Enter && take_exit(e)) return;
        } else if (!inside && was) {
            exits_inside_.erase(e);
        }
        // Use: pressed inside (arriving inside one counts: the player is there to use it).
        if (inside && x.activation == ExitActivation::Use && actions.pressed("use") && take_exit(e))
            return;
    }
}

}  // namespace midgard
