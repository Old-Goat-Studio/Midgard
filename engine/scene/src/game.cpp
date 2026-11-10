// SPDX-License-Identifier: Apache-2.0
#include "midgard/game.h"

#include "midgard/scene_nav.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <sqlite3.h>

#include "midgard/collider_outlines.h"
#include "midgard/det.h"
#include "midgard/footsteps.h"
#include "midgard/gltf.h"
#include "midgard/survival.h"
#include "midgard/terrain_collision.h"
#include "midgard/world_surface.h"

namespace midgard {

Game::Game(const std::filesystem::path& package_dir)
    : package_(open_package(package_dir)),
      scene_(Scene::load(package_dir / package_.manifest.database, package_.manifest.entry_scene)) {
    const auto pak_file = package_dir / "game.pak";
    if (std::filesystem::is_regular_file(pak_file)) pak_ = std::make_unique<Pak>(pak_file);
    layers_ = PhysicsLayers::load(package_dir / package_.manifest.database);
    note_loaded();
    rebuild_physics();
    rebuild_terrain();
    camera_ = std::make_unique<CameraRig>(scene_);
    camera_->follow(scene_, 0.0f, camera_sweep());
    note_exits_inside();
    // Characters' feet stand on what physics has (the ground, and anything solid).
    characters_.set_ground([this](entt::entity self, const glm::dvec3& from,
                                  const glm::vec3& direction,
                                  double distance) -> std::optional<Characters::GroundHit> {
        if (!physics_) return std::nullopt;
        const auto hit = physics_->world().raycast(from, direction, distance,
                                                   physics::Touching::Solid, physics_->body(self));
        if (!hit) return std::nullopt;
        return Characters::GroundHit{hit->point, hit->normal};
    });

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
    stats_.define(scene_.stats);
    rebuild_weather();  // (on the clock's calendar)
    update_weather();
    weather_pending_.clear();         // (the weather the game starts with isn't a change)
    look_ = look_of(weather_type());  // (as it is, from the start)

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
    // The game's own state (milestone 5.16): the scenes as the player left them, the player
    // carried between them, a script's weather and how wet the player is.
    for (const auto& [name, value] : world_->values("rooms/")) rooms_[name.substr(6)] = value;
    if (const auto player = world_->value("player")) {
        const nlohmann::json p = nlohmann::json::parse(*player, nullptr, false);
        if (p.is_object()) {
            carried_id_ = p.value("id", "");
            carried_name_ = p.value("name", "");
            carried_player_ = p.value("components", "");
        }
    }
    if (const auto w = world_->value("weather")) {
        const nlohmann::json j = nlohmann::json::parse(*w, nullptr, false);
        if (j.is_object() && j.contains("forced")) {
            Forced f{j.value("forced", ""), std::nullopt};
            if (j.contains("until") && j.at("until").is_number_integer())
                f.until = j.at("until").get<std::int64_t>();
            if (scene_.weather_types.contains(f.weather)) forced_ = std::move(f);
        }
        if (j.is_object()) exposure_.wetness = j.value("player_wetness", 0.0f);
    }
    if (const auto s = world_->value("stats"))  // (the player's stats as they left them)
        stats_.restore(nlohmann::json::parse(*s, nullptr, false));
    // The scene it starts in, as the player left it (before it is left for theirs, below:
    // what it remembers is then this), with their player.
    if (const auto room = rooms_.find(scene_.id);
        room != rooms_.end() || !carried_player_.empty()) {
        if (room != rooms_.end()) apply_room(room->second);
        carry_player();
        rebuild_physics();
        rebuild_terrain();
        camera_ = std::make_unique<CameraRig>(scene_);
        camera_->follow(scene_, 0.0f, camera_sweep());
    }
    update_weather();
    weather_pending_.clear();
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
    std::map<std::string, std::string> state;
    rooms_[scene_.id] = capture_room();
    for (const auto& [scene, room] : rooms_) state["rooms/" + scene] = room;
    if (!carried_player_.empty())
        state["player"] = nlohmann::json{
            {"id", carried_id_},
            {"name", carried_name_},
            {"components",
             carried_player_}}.dump();
    nlohmann::json weather = {{"player_wetness", exposure_.wetness}};
    if (forced_) {
        weather["forced"] = forced_->weather;
        if (forced_->until) weather["until"] = *forced_->until;
    }
    state["weather"] = weather.dump();
    state["stats"] = stats_.save().dump();
    world_->save_values(state);
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
    scene_.light_time += dt;
    const PlayerIntent intent = camera_->steer(scene_, actions, dt);
    if (!change_) fly(actions, dt);  // (a flying player: the ship in space, a jetpack)
    if (change_)                     // (on the way to another scene: the player stands still)
        physics_->set_player_intent(glm::vec3(0.0f), false);
    else if (!route_.empty())  // following a path
        physics_->set_player_intent(route_walk(camera_->player()), false);
    else
        physics_->set_player_intent(intent.walk, intent.jump);
    update_ground();               // solid ground under the player before it moves
    physics_->update(scene_, dt);  // the player walks
    camera_->follow(scene_, dt, camera_sweep());
    update_triggers();
    update_exits(actions);
    load_characters();  // (new ones from live edits; read once each)
    characters_.set_player(physics_->player());
    characters_.update(scene_, dt);
    apply_root_motion();
    stream_navigation();
    cross_atmosphere();
    update_weather();
    weather_changes_ = std::exchange(weather_pending_, {});  // (scripts' since the last, too)
    stats_.update(dt);
    stat_changes_ = stats_.take_changes();  // (scripts' since the last update, too)
    hud_.update(dt);
    finished_timers_ = hud_.take_finished();
    ui_events_ = hud_.take_events();
    update_minimap();
    update_weather_look(dt);
    update_falling(dt);
    update_ground_cover(dt);
    update_exposure(dt);
    update_sounds(dt);
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
    if (asset == kBuiltinSphere) return render::make_sphere();
    if (asset == kBuiltinCylinder) return render::make_cylinder();
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

// ---- sound (milestone 5.14) --------------------------------------------------------------------

void Game::set_audio(audio::Mixer* mixer) {
    stop_sources();
    weather_voices_.clear();
    weather_sounding_.clear();  // (the new player plays them)
    ambience_.reset();
    music_.reset();
    sounds_.reset();
    if (!mixer) return;
    sounds_ = std::make_unique<audio::SoundPlayer>(
        *mixer, [this](const std::string& id) { return read_texture(id); });
    for (const auto& [id, e] : scene_.sound_events) sounds_->add(e);
    music_ = std::make_unique<audio::MusicDirector>(*sounds_);
    for (const auto& [id, p] : scene_.playlists) music_->add(p);
    ambience_ = std::make_unique<audio::AmbienceDirector>(*sounds_);
    for (const auto& [id, a] : scene_.ambience) ambience_->add(a);
    for (const auto& [bus, volume] : scene_.bus_volumes)
        if (const auto b = audio::bus_from_name(bus)) mixer->set_volume(*b, volume);
    sounds_->set_blocked([this](audio::VoiceId voice, const glm::dvec3& from,
                                const glm::dvec3& to) { return sound_blocked(voice, from, to); });
    listen();
}

// The listener is the camera: where it is, which way it faces.
void Game::listen() {
    if (!sounds_) return;
    const render::Camera cam = camera(1.0f);
    const glm::mat3 turn = glm::inverse(glm::mat3(cam.view));
    audio::SoundPlayer::Listener listener;
    listener.position = cam.origin;
    listener.forward = glm::normalize(turn * glm::vec3(0, 0, -1));
    listener.up = glm::normalize(turn * glm::vec3(0, 1, 0));
    sounds_->set_listener(listener);
}

// Something solid between the listener and a placed sound (not the thing making it, nor its
// parts): a ray through the physics world.
bool Game::sound_blocked(audio::VoiceId voice, const glm::dvec3& from, const glm::dvec3& to) const {
    const glm::dvec3 d = to - from;
    const double distance = glm::length(d);
    if (distance < 0.5 || !physics_) return false;
    const auto hit = physics_->world().raycast(from, glm::vec3(d / distance), distance - 0.25);
    if (!hit) return false;
    const auto owner = attached_.find(voice);
    if (owner == attached_.end()) return true;
    // The source's own body, or one of its parts' or parents': not a wall.
    const auto hit_entity = static_cast<entt::entity>(hit->user);
    for (entt::entity e = hit_entity; e != entt::null && scene_.registry.valid(e);) {
        if (e == owner->second) return false;
        const EntityInfo* info = scene_.registry.try_get<EntityInfo>(e);
        e = info ? info->parent : entt::null;
    }
    for (entt::entity e = owner->second; e != entt::null && scene_.registry.valid(e);) {
        if (e == hit_entity) return false;
        const EntityInfo* info = scene_.registry.try_get<EntityInfo>(e);
        e = info ? info->parent : entt::null;
    }
    return true;
}

// The echo the listener hears: the reverb zone they're most inside (fading in over its edge).
void Game::update_reverb() {
    const glm::dvec3 at = camera(1.0f).origin;
    float best = 0.0f;
    audio::ReverbSettings chosen;
    chosen.wet = 0.0f;
    for (const entt::entity e : scene_.registry.view<const ReverbZone>()) {
        const ReverbZone& z = scene_.registry.get<ReverbZone>(e);
        const glm::dmat4 world = scene_.world_matrix(e);
        const glm::dvec3 local = glm::dvec3(glm::inverse(world) * glm::dvec4(at, 1.0));
        glm::dvec3 outside(0.0);
        for (int i = 0; i < 3; ++i)
            outside[i] = std::max(0.0, std::abs(local[i]) - double(z.size[i]) / 2.0) *
                         glm::length(glm::dvec3(world[i]));
        const double away = glm::length(outside);
        const float weight = away <= 0.0      ? 1.0f
                             : z.edge <= 0.0f ? 0.0f
                                              : float(std::max(0.0, 1.0 - away / z.edge));
        if (weight > best) {
            best = weight;
            chosen = audio::reverb_preset(z.preset).value_or(audio::ReverbSettings{});
            if (z.wet) chosen.wet = *z.wet;
            chosen.wet *= weight;
        }
    }
    reverb_ = chosen;
    if (sounds_) sounds_->mixer().set_reverb(chosen);
}

void Game::set_audio_data(const nlohmann::json& a) {
    // Everything read first: a mistake changes nothing.
    std::map<std::string, audio::SoundEvent> events;
    std::vector<audio::Playlist> playlists;
    std::vector<audio::AmbienceSet> sets;
    std::map<std::string, float> buses;
    try {
        for (const nlohmann::json& e : a.value("events", nlohmann::json::array())) {
            audio::SoundEvent ev = audio::sound_event_from_json(
                e.at("id"), e.value("name", ""), e.value("event", nlohmann::json::object()).dump());
            events[ev.id] = std::move(ev);
        }
        for (const nlohmann::json& p : a.value("playlists", nlohmann::json::array()))
            playlists.push_back(
                audio::playlist_from_json(p.at("id"), p.value("name", ""),
                                          p.value("playlist", nlohmann::json::object()).dump()));
        for (const nlohmann::json& s : a.value("ambience", nlohmann::json::array()))
            sets.push_back(
                audio::ambience_from_json(s.at("id"), s.value("name", ""),
                                          s.value("ambience", nlohmann::json::object()).dump()));
        const nlohmann::json volumes = a.value("buses", nlohmann::json::object());
        for (const auto& [bus, volume] : volumes.items())  // (a named value: items() refers to it)
            if (audio::bus_from_name(bus)) buses[bus] = volume.get<float>();
    } catch (const std::exception& ex) {
        throw SceneError(std::string("audio: ") + ex.what());
    }
    scene_.sound_events = events;
    scene_.playlists.clear();
    for (const auto& p : playlists) scene_.playlists[p.id] = p;
    scene_.ambience.clear();
    for (const auto& s : sets) scene_.ambience[s.id] = s;
    scene_.bus_volumes = buses;
    if (!sounds_) return;
    // Events: changed ones replaced (their sounds read again: new ones may be in the package
    // now), removed ones stopped.
    std::vector<std::string> gone;
    for (const auto& [id, e] : sounds_->events())
        if (!events.contains(id)) gone.push_back(id);
    for (const std::string& id : gone) sounds_->remove(id);
    for (const auto& [id, e] : events) {
        const audio::SoundEvent* had = sounds_->find(id);
        if (had && *had == e) continue;
        for (const auto& v : e.sounds) sounds_->forget(v.sound);
        sounds_->add(e);
    }
    if (music_) music_->replace(std::move(playlists));
    if (ambience_) ambience_->replace(std::move(sets));
    for (const char* bus : {"master", "music", "effects", "voice", "ambience", "interface"}) {
        const auto it = buses.find(bus);
        sounds_->mixer().set_volume(*audio::bus_from_name(bus),
                                    it == buses.end() ? 1.0f : it->second);
    }
}

audio::MusicContext Game::music_context() const {
    audio::MusicContext c;
    c.scene = scene_.id;
    if (scene_.settings.world) c.world_type = scene_.settings.world->world_type;
    c.time = audio::time_of_day(scene_.solar_phase.value_or(0.25));  // (no clock: day)
    if (terrain_ && scene_.settings.body && scene_.settings.terrain) {
        const glm::dvec3 at = camera(1.0f).origin;
        const SurfacePoint p = scene_.settings.body->surface(at);
        const Terrain& t = terrain_->terrain();
        const int biome = t.biome_index(p.face, p.u, p.v, t.height(p.face, p.u, p.v));
        if (biome >= 0 && size_t(biome) < scene_.settings.terrain->biomes.size())
            c.biome = scene_.settings.terrain->biomes[size_t(biome)].name;
    }
    return c;
}

// ---- weather (milestone 5.15) ------------------------------------------------------------------

void Game::rebuild_weather() {
    weather_ = scene_weather(
        scene_.settings, scene_.weather_types, local_calendar(),
        [this]() -> const Terrain* { return terrain_ ? &terrain_->terrain() : nullptr; });
}

double Game::heading() const {
    const render::Camera cam = camera(1.0f);
    const glm::mat4& v = cam.view;  // (world to camera: its rotation's rows are the camera's axes)
    const glm::dvec3 forward(-v[0][2], -v[1][2], -v[2][2]);
    return heading_of(bearing_at(scene_.settings.body, cam.origin), forward);
}

void Game::update_minimap(bool now) {
    if (!minimap_wanted_ || !terrain_) return;
    // One made in the background, ready: shown (if it is of this scene).
    if (minimap_job_ &&
        minimap_job_->pixels.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        std::vector<std::uint8_t> pixels = minimap_job_->pixels.get();
        if (minimap_job_->scene == scene_.id) {
            minimap_.centre = minimap_job_->centre;
            minimap_.radius = kMinimapGroundRadius;
            minimap_.size = kMinimapSize;
            minimap_.pixels = std::move(pixels);
            ++minimap_.version;
            minimap_scene_ = scene_.id;
        }
        minimap_job_.reset();
    }
    const glm::dvec3 here = player_position();
    const Bearing b = bearing_at(scene_.settings.body, minimap_.centre);
    const glm::dvec2 moved = east_north(b, minimap_.centre, here);
    const bool stale = minimap_.pixels.empty() || minimap_scene_ != scene_.id ||
                       glm::length(moved) > kMinimapGroundRadius / 4.0;
    if (!stale || (minimap_job_ && !now)) return;
    if (now) {  // (the first: a screenshot, or the HUD's first frame)
        minimap_job_.reset();
        minimap_.centre = here;
        minimap_.radius = kMinimapGroundRadius;
        minimap_.size = kMinimapSize;
        minimap_.pixels = local_map(terrain_->terrain(), here, kMinimapGroundRadius, kMinimapSize);
        ++minimap_.version;
        minimap_scene_ = scene_.id;
        return;
    }
    // (the terrain is read from background jobs already: its heights and colours are const)
    const Terrain* terrain = &terrain_->terrain();
    minimap_job_ =
        MinimapJob{here, scene_.id, std::async(std::launch::async, [terrain, here] {
                       return local_map(*terrain, here, kMinimapGroundRadius, kMinimapSize);
                   })};
}

std::string Game::stat_text(const stats::StatDef& stat) const {
    if (stat.kind == stats::Kind::position)
        return place_text(scene_.settings.body, player_position());
    if (stat.kind == stats::Kind::clock)
        return date_text(local_calendar(), time_.now(), stat.show,
                         [this](const std::string& key) { return text(key); });
    return {};
}

glm::dvec2 Game::minimap_offset() const {
    return east_north(bearing_at(scene_.settings.body, minimap_.centre), minimap_.centre,
                      player_position());
}

std::vector<Game::MapMark> Game::map_marks(double radius) const {
    std::vector<MapMark> out;
    const glm::dvec3 here = player_position();
    const Bearing b = bearing_at(scene_.settings.body, here);
    const auto place = [&](entt::entity e, std::string kind, std::string label) {
        const glm::dvec3 at(scene_.world_matrix(e)[3]);
        const glm::dvec2 en = east_north(b, here, at);
        if (glm::length(en) > radius) return;
        out.push_back({std::move(kind), std::move(label), en.x, en.y});
    };
    for (const entt::entity e : scene_.registry.view<const SceneExit>()) place(e, "exit", "");
    if (!hud_.marks.empty()) {
        for (const entt::entity e : scene_.registry.view<const EntityInfo>()) {
            const auto it = hud_.marks.find(scene_.registry.get<EntityInfo>(e).id);
            if (it != hud_.marks.end()) place(e, "mark", it->second);
        }
    }
    return out;
}

const ScreenDef* Game::screen(const std::string& id) const {
    for (const ScreenDef& s : scene_.screens)
        if (s.id == id) return &s;
    return nullptr;
}

void Game::set_screen_data(const nlohmann::json& rows) {
    std::vector<ScreenDef> screens;
    try {
        for (const nlohmann::json& r : rows)
            screens.push_back(screen_from_json(r.at("id"), r.value("name", ""),
                                               r.value("screen", nlohmann::json::object()).dump()));
    } catch (const std::exception& e) {
        throw SceneError(std::string("screens: ") + e.what());
    }
    scene_.screens = std::move(screens);
    for (const std::string& id : std::vector(hud_.screens()))
        if (!screen(id)) hud_.hide_screen(id);
}

bool Game::ui_click(const std::string& screen_id, const std::string& element) {
    const ScreenDef* s = screen(screen_id);
    const ScreenElement* e = s ? s->find(element) : nullptr;
    hud_.post({screen_id, element, "click", 0.0});
    if (!e) return false;
    if (e->action == "close") hud_.hide_screen(screen_id);
    // (Opening another puts it on top: closing that one comes back to this, as Settings goes
    // back to the pause menu.)
    if (e->action == "open" && screen(e->target)) hud_.show_screen(e->target);
    return e->action == "quit";
}

void Game::ui_change(const std::string& screen_id, const std::string& element, double value) {
    hud_.post({screen_id, element, "change", value});
    // An option chosen: the others in its group aren't.
    const ScreenDef* s = screen(screen_id);
    const ScreenElement* e = s ? s->find(element) : nullptr;
    if (e && e->kind == "option" && value != 0.0)
        for (const ScreenElement& other : s->elements)
            if (other.kind == "option" && other.group == e->group && other.id != e->id)
                hud_.values[screen_id + "/" + other.id] = 0.0;
}

void Game::set_stat_data(const nlohmann::json& rows) {
    std::vector<stats::StatDef> defs;
    try {
        for (const nlohmann::json& r : rows)
            defs.push_back(stats::stat_from_json(r.at("id"), r.value("name", ""),
                                                 r.value("stat", nlohmann::json::object()).dump()));
    } catch (const std::exception& e) {
        throw SceneError(std::string("stats: ") + e.what());
    }
    scene_.stats = defs;
    stats_.define(std::move(defs));
}

void Game::set_weather_data(const nlohmann::json& data) {
    weather::WeatherTypes types;
    std::string settings = scene_.settings.weather;
    try {
        for (const nlohmann::json& t : data.value("types", nlohmann::json::array())) {
            weather::WeatherType w = weather::weather_type_from_json(
                t.at("id"), t.value("name", ""),
                t.value("weather", nlohmann::json::object()).dump());
            types.emplace(w.id, std::move(w));
        }
        if (data.contains("settings")) {
            settings = data.at("settings").is_null() ? std::string() : data.at("settings").dump();
            if (!settings.empty()) weather::weather_settings_from_json(settings);
        }
    } catch (const std::exception& e) {
        throw SceneError(std::string("weather: ") + e.what());
    }
    if (data.contains("types")) scene_.weather_types = std::move(types);
    scene_.settings.weather = std::move(settings);
    rebuild_weather();
    update_weather();
    weather_sounding_ = "\x01";  // (its sounds start again: they may have changed)
}

glm::dvec3 Game::player_position() const {
    for (const entt::entity e : scene_.registry.view<const Player>())
        return scene_.world_position(e);
    return camera(1.0f).origin;
}

std::optional<weather::Cell> Game::weather_cell() const {
    if (!weather_) return std::nullopt;
    const glm::dvec3 at = player_position();
    return weather_->cell_at(weather_point(scene_.settings, at));
}

const weather::WeatherType* Game::weather_type() const {
    const auto it = scene_.weather_types.find(weather_now_.weather);
    return it == scene_.weather_types.end() ? nullptr : &it->second;
}

void Game::update_weather() {
    const std::int64_t now = time_.now();
    if (forced_ && forced_->until && now >= *forced_->until) forced_.reset();
    weather::Now next;
    if (in_space()) {
        // (Space has no weather; a script's comes back on landing.)
    } else if (forced_) {
        next = {forced_->weather, -1, true};
    } else if (const auto cell = weather_cell()) {
        next = weather_->at(*cell, now);
    }
    if (next != weather_now_) {
        weather_pending_.push_back({weather_now_, next});
        weather_now_ = std::move(next);
    }
}

std::vector<weather::Spell> Game::weather_forecast(double hours) {
    const std::int64_t from = time_.now();
    const std::int64_t until =
        from + std::max<std::int64_t>(
                   std::llround(hours * double(local_calendar().seconds_per_hour())), 1);
    std::vector<weather::Spell> natural;
    if (const auto cell = weather_cell())
        natural = weather_->forecast(*cell, from, until);
    else
        natural = {{"", from, until, -1, false}};
    if (!forced_) return natural;
    // The forced weather until it ends, then what there would have been.
    const std::int64_t end = std::min(until, forced_->until.value_or(until));
    std::vector<weather::Spell> out{{forced_->weather, from, end, -1, true}};
    for (weather::Spell s : natural) {
        if (s.end <= end) continue;
        s.start = std::max(s.start, end);
        out.push_back(std::move(s));
    }
    return out;
}

bool Game::force_weather(const std::string& weather, double hours) {
    if (!scene_.weather_types.contains(weather)) return false;
    Forced f{weather, std::nullopt};
    if (hours > 0.0)
        f.until =
            time_.now() + std::max<std::int64_t>(
                              std::llround(hours * double(local_calendar().seconds_per_hour())), 1);
    forced_ = std::move(f);
    update_weather();  // (now: scripts and the sky see it this frame)
    return true;
}

void Game::release_weather() {
    forced_.reset();
    update_weather();
}

// The weather's look eases towards the weather's own (most of the way in about eight seconds),
// its clouds drift with the wind (as the clock runs), and lightning flashes now and then at its
// rate a minute: a strike or two, the thunder a few seconds after.
void Game::update_weather_look(float dt) {
    const weather::WeatherType* type = weather_type();
    const WeatherLook target = look_of(type);
    look_ = blend(look_, target, 1.0f - std::exp(-std::max(dt, 0.0f) / 4.0f));
    const float wind = type ? type->wind : 0.0f;
    look_.drift.x += wind * dt * float(std::max(time_.scale(), 1.0));
    look_.drift.y += 0.3f * wind * dt * float(std::max(time_.scale(), 1.0));

    flash_clock_ += dt;
    det::Rng rng(lightning_rng_);
    const auto roll = [&] {
        const double r = rng.uniform();
        lightning_rng_ = rng.next();
        return r;
    };
    const float rate = type ? type->lightning : 0.0f;  // a minute
    if (rate <= 0.0f) {
        next_flash_ = -1.0;
    } else {
        if (next_flash_ < 0.0) next_flash_ = flash_clock_ - std::log(1.0 - roll()) * 60.0 / rate;
        if (flash_clock_ >= next_flash_) {
            ++flashes_;
            strikes_ = {flash_clock_};
            if (roll() < 0.6) strikes_.push_back(flash_clock_ + 0.08 + 0.1 * roll());
            if (type && !type->thunder.empty())
                thunder_.emplace_back(flash_clock_ + 1.0 + 3.0 * roll(), type->thunder);
            next_flash_ = flash_clock_ - std::log(1.0 - roll()) * 60.0 / rate;
        }
    }
    float flash = 0.0f;
    for (const double at : strikes_)
        if (flash_clock_ >= at)
            flash = std::max(flash, float(std::exp(-(flash_clock_ - at) / 0.07)));
    look_.flash = flash < 0.01f ? 0.0f : flash;
    for (auto it = thunder_.begin(); it != thunder_.end();) {
        if (flash_clock_ < it->first) {
            ++it;
            continue;
        }
        if (sounds_) sounds_->play(it->second);
        it = thunder_.erase(it);
    }
}

// What falls eases in and out over a few seconds as the weather changes (one kind fading away
// before the next comes), at its fall speed, carried by the wind (east, gusting).
void Game::update_falling(float dt) {
    const weather::WeatherType* type = weather_type();
    using render::Falls;
    Falls kind = Falls::None;
    float amount = 0.0f;
    if (type && type->falls != weather::Falls::none) {
        kind = Falls(int(type->falls));  // (the same order: none, rain, snow, ash, sand)
        amount = type->amount;
    }
    const float ease = 1.0f - std::exp(-std::max(dt, 0.0f) / 3.0f);
    if (falling_.kind != kind) {
        falling_.amount -= falling_.amount * ease + 0.01f * dt;  // (the last of it goes too)
        if (falling_.amount <= 0.01f) {
            falling_.kind = kind;
            falling_.amount = 0.0f;
        }
    } else {
        falling_.amount += (amount - falling_.amount) * ease;
    }
    falling_.time += dt;
    if (falling_.kind == Falls::None) return;
    const glm::dvec3 at = camera(1.0f).origin;
    glm::dvec3 up(0.0, 1.0, 0.0), east(1.0, 0.0, 0.0);
    if (scene_.settings.body) {
        const SurfaceFrame f = scene_.settings.body->frame(at);
        up = f.up;
        east = f.east;
    }
    const double speeds[] = {0.0, 9.0, 1.2, 1.0, 0.4};  // metres a second, falling
    const double t = falling_.time;
    const double gusting =
        type ? 1.0 + type->gusts * 0.6 * std::sin(t * 0.9) * std::sin(t * 0.37 + 1.0) : 1.0;
    const double wind = type ? type->wind * gusting : 0.0;
    const glm::dvec3 velocity = east * wind - up * speeds[size_t(falling_.kind)];
    falling_.velocity = glm::vec3(velocity);
    // (Wrapped at 480 m: a whole number of every kind's box, so nothing jumps.)
    falling_.moved = glm::mod(falling_.moved + velocity * double(dt), glm::dvec3(480.0));
}

double Game::temperature_here() const {
    const weather::WeatherType* type = weather_type();
    const double change = type ? type->temperature : 0.0;
    if (!terrain_ || !scene_.settings.body || !scene_.settings.terrain ||
        !scene_.settings.terrain->climate)
        return 15.0 + change;
    const glm::dvec3 at = player_position();
    const SurfacePoint p = scene_.settings.body->surface(at);
    const Terrain& t = terrain_->terrain();
    return t.temperature_at(p.face, p.u, p.v, p.height) + double(terrain_look().season_offset) +
           change;
}

void Game::update_ground_cover(float dt) {
    const double hours = double(std::max(dt, 0.0f)) * std::max(time_.scale(), 0.0) /
                         double(local_calendar().seconds_per_hour());
    if (hours <= 0.0) return;
    const weather::WeatherType* type = weather_type();
    const bool raining = falling_.kind == render::Falls::Rain && falling_.amount > 0.01f;
    const bool snowing = falling_.kind == render::Falls::Snow && falling_.amount > 0.01f;
    const double temperature = temperature_here();
    const bool climate = scene_.settings.terrain && scene_.settings.terrain->climate;
    // Snow: settling, or melting (fast in rain or the warmth, slowly otherwise; never in a
    // climate's frost).
    double snow = ground_cover_.snow;
    if (snowing) {
        snow += hours * 0.4 * double(falling_.amount);
    } else if (!(climate && temperature < 0.0)) {
        const double rate = raining || temperature > 4.0 ? 0.6 : 0.15;
        snow -= hours * rate;
    }
    snow = std::clamp(snow, 0.0, 1.0);
    const double melted = std::max(double(ground_cover_.snow) - snow, 0.0);
    // Wetness: up to the weather's in about a quarter of an hour while it rains (or a fog's damp
    // settles), drying over about two hours after; melting snow wets it.
    double wet = ground_cover_.wetness;
    const double target =
        type ? double(type->wetness) * (raining || type->falls == weather::Falls::none ? 1.0 : 0.0)
             : 0.0;
    if (target > wet)
        wet += (target - wet) * (1.0 - std::exp(-hours / 0.25));
    else
        wet *= std::exp(-hours / (temperature > 20.0 ? 1.0 : 2.0));
    wet = std::clamp(wet + melted * 2.0, 0.0, 1.0);
    ground_cover_.snow = float(snow);
    ground_cover_.wetness = float(wet);
}

void Game::set_player_wetness(float wetness) {
    exposure_.wetness = std::clamp(wetness, 0.0f, 1.0f);
}

// Shelter (rays upward and sideways from the player's head, four times a second), the wind
// and heat reaching them, how wet they are, and how warm it feels.
void Game::update_exposure(float dt) {
    entt::entity player = entt::null;
    for (const entt::entity e : scene_.registry.view<const Player>()) player = e;
    const glm::dvec3 at = player_position();
    glm::dvec3 up(0.0, 1.0, 0.0), east(1.0, 0.0, 0.0), north(0.0, 0.0, -1.0);
    if (scene_.settings.body) {
        const SurfaceFrame f = scene_.settings.body->frame(at);
        up = f.up;
        east = f.east;
        north = f.north;
    }
    shelter_due_ -= dt;
    if (shelter_due_ <= 0.0 && physics_) {
        shelter_due_ = 0.25;
        const glm::dvec3 head = at + up * 0.6;
        const physics::BodyId self =
            player != entt::null ? physics_->body(player) : physics::kNoBody;
        const auto blocked = [&](const glm::dvec3& d, double reach) {
            return physics_->world()
                .raycast(head, glm::vec3(glm::normalize(d)), reach, physics::Touching::Solid, self)
                .has_value();
        };
        int roof = 0, walls = 0;
        const double lean = 0.36;  // (about 20 degrees off straight up)
        for (const glm::dvec3& d :
             {up, up + east * lean, up - east * lean, up + north * lean, up - north * lean})
            roof += blocked(d, 40.0);
        for (int i = 0; i < 8; ++i) {
            const double a = double(i) * 0.7853981633974483;
            walls += blocked(east * std::cos(a) + north * std::sin(a), 6.0);
        }
        exposure_.roof = float(roof) / 5.0f;
        exposure_.walls = float(walls) / 8.0f;
        exposure_.sheltered = exposure_.roof >= 0.6f;
    }
    // Heat sources near the player.
    double heat = 0.0;
    for (const entt::entity e : scene_.registry.view<const HeatSource>()) {
        const HeatSource& h = scene_.registry.get<HeatSource>(e);
        heat += survival::warmth(h.heat, h.radius, glm::length(scene_.world_position(e) - at));
    }
    exposure_.heat = heat;
    const weather::WeatherType* type = weather_type();
    exposure_.wind = (type ? type->wind : 0.0) * (1.0 - 0.8 * exposure_.walls);
    // Wet in the open while it rains (snow less so), drying under cover, faster by a fire.
    const double hours = double(std::max(dt, 0.0f)) * std::max(time_.scale(), 0.0) /
                         double(local_calendar().seconds_per_hour());
    const bool falling_now =
        falling_.kind == render::Falls::Rain || falling_.kind == render::Falls::Snow;
    double wet = exposure_.wetness;
    const double target =
        falling_now && !exposure_.sheltered
            ? double(falling_.amount) * (falling_.kind == render::Falls::Rain ? 1.0 : 0.4)
            : 0.0;
    if (target > wet)
        wet += (target - wet) * (1.0 - std::exp(-hours / 0.15));
    else
        wet *= std::exp(-hours / (heat > 5.0 ? 0.25 : 1.0));
    exposure_.wetness = float(std::clamp(wet, 0.0, 1.0));
    exposure_.air = temperature_here();
    exposure_.feels_like =
        survival::feels_like(exposure_.air, exposure_.wind, exposure_.wetness, exposure_.heat);
}

render::Environment Game::environment(const glm::dvec3& origin) const {
    render::Environment env = scene_.environment(origin);
    render::Light light;
    apply(look_, env, light);
    env.falling = falling_;
    // (Indoors, a roof and walls all round: none falls where the player can see.)
    env.falling.amount *= 1.0f - exposure_.roof * exposure_.walls;
    // (Under the water, none: the camera is below its surface.)
    if (physics_ && physics_->world().water_depth(origin) > 0.0) env.falling.amount = 0.0f;
    env.ground = ground_cover_;
    if (in_space()) {  // (the stars, from the world's seed: milestone 5.16)
        env.stars.brightness = 1.0f;
        env.stars.density = 0.35f;
        env.stars.seed = scene_.settings.body ? std::uint32_t(scene_.settings.body->seed) : 1u;
    }
    return env;
}

render::Light Game::light(const glm::dvec3& origin) const {
    render::Light light = scene_.light(origin);
    render::Environment env;
    apply(look_, env, light);
    return light;
}

// The weather's sounds: those of the weather now, looping, fading in as the last weather's fade
// out (each at its sound's volume).
void Game::update_weather_sounds() {
    if (!sounds_ || weather_sounding_ == weather_now_.weather) return;
    constexpr float kFade = 4.0f;
    for (const audio::VoiceId v : weather_voices_) sounds_->fade_out(v, kFade);
    weather_voices_.clear();
    weather_sounding_ = weather_now_.weather;
    if (const weather::WeatherType* type = weather_type()) {
        for (const weather::WeatherType::Sound& s : type->sounds) {
            audio::SoundPlayer::Play play;
            play.volume = s.volume;
            play.fade_in = kFade;
            if (const audio::VoiceId v = sounds_->play(s.event, play)) weather_voices_.push_back(v);
        }
    }
}

audio::VoiceId Game::play_sound(const std::string& event, float volume, entt::entity from,
                                std::optional<glm::dvec3> at) {
    if (!sounds_) return 0;
    audio::SoundPlayer::Play play;
    play.volume = volume;
    if (from != entt::null && scene_.registry.valid(from)) at = scene_.world_position(from);
    play.at = at;
    const audio::VoiceId voice = sounds_->play(event, play);
    if (voice && from != entt::null) attached_[voice] = from;
    return voice;
}

void Game::stop_sources() {
    if (sounds_)
        for (const auto& [e, s] : sources_) sounds_->stop(s.voice);
    sources_.clear();
    attached_.clear();  // (a script's sound from one of its entities plays on where it was)
    walks_.clear();
}

std::string Game::ground_under(entt::entity e) const {
    const glm::dvec3 at = scene_.world_position(e);
    const Body* body = scene_.settings.body ? &*scene_.settings.body : nullptr;
    const glm::dvec3 up = body ? body->up(at) : glm::dvec3(0, 1, 0);
    double reach = 1.5;  // (a character stands on its entity; the player in its middle)
    if (const Player* p = scene_.registry.try_get<Player>(e)) reach += 0.5 * p->height;
    const glm::dvec3 from = at + up * 0.25;
    const auto hit = physics_
                         ? physics_->world().raycast(from, glm::vec3(-up), reach + 0.25,
                                                     physics::Touching::Solid, physics_->body(e))
                         : std::nullopt;
    if (hit && hit->user != kTerrainUser) {
        // An object: its surface (or its group's, or its parents').
        for (entt::entity o = static_cast<entt::entity>(hit->user);
             o != entt::null && scene_.registry.valid(o);) {
            if (const PhysicsMaterial* m = scene_.registry.try_get<PhysicsMaterial>(o);
                m && !m->surface.empty())
                return m->surface;
            const EntityInfo* info = scene_.registry.try_get<EntityInfo>(o);
            o = info ? info->parent : entt::null;
        }
        return "default";
    }
    if (terrain_ && body) return terrain_ground(terrain_->terrain(), *body, hit ? hit->point : at);
    return "default";
}

void Game::step(entt::entity e, const Footsteps& f) {
    Step s;
    s.entity = scene_.registry.get<EntityInfo>(e).id;
    s.surface = ground_under(e);
    auto sound = f.sounds.find(s.surface);
    if (sound == f.sounds.end()) sound = f.sounds.find("default");
    if (sound != f.sounds.end()) {
        s.sound = sound->second;
        play_sound(s.sound, 1.0f, e);
    }
    steps_.push_back(std::move(s));
    ++steps_taken_;
    if (steps_.size() > 64) steps_.erase(steps_.begin());
}

// Footsteps on animation events, or every so many metres; and the sounds characters' other
// animation events play.
void Game::update_footsteps() {
    for (const Characters::Event& ev : characters_.events()) {
        if (!scene_.registry.valid(ev.entity)) continue;
        if (const CharacterRef* c = scene_.registry.try_get<CharacterRef>(ev.entity))
            if (const auto it = c->sounds.find(ev.name); it != c->sounds.end())
                play_sound(it->second, 1.0f, ev.entity);
        if (const Footsteps* f = scene_.registry.try_get<Footsteps>(ev.entity);
            f && ev.name == f->event)
            step(ev.entity, *f);
    }
    for (const entt::entity e : scene_.registry.view<const Footsteps>()) {
        const Footsteps& f = scene_.registry.get<Footsteps>(e);
        if (f.every <= 0.0f) continue;
        const glm::dvec3 at = scene_.world_position(e);
        Walk& w = walks_[e];
        if (!w.started) {
            w = {at, 0.0, true};
            continue;
        }
        // Along the ground (not up and down).
        const glm::dvec3 up =
            scene_.settings.body ? scene_.settings.body->up(at) : glm::dvec3(0, 1, 0);
        const glm::dvec3 moved = at - w.last;
        w.travelled += glm::length(moved - up * glm::dot(moved, up));
        w.last = at;
        if (w.travelled >= f.every) {
            w.travelled = std::fmod(w.travelled, double(f.every));
            step(e, f);
        }
    }
    std::erase_if(walks_, [&](const auto& w) {
        return !scene_.registry.valid(w.first) || !scene_.registry.all_of<Footsteps>(w.first);
    });
}

void Game::update_sounds(double dt) {
    if (!sounds_) return;
    // Entities gone, or their sound changed or turned off: theirs stops.
    for (auto it = sources_.begin(); it != sources_.end();) {
        const SoundSource* s = scene_.registry.valid(it->first)
                                   ? scene_.registry.try_get<SoundSource>(it->first)
                                   : nullptr;
        if (!s || !s->autoplay || s->event != it->second.event) {
            sounds_->stop(it->second.voice);
            it = sources_.erase(it);
            continue;
        }
        if (s->volume != it->second.volume) {
            sounds_->set_volume(it->second.voice, s->volume);
            it->second.volume = s->volume;
        }
        ++it;
    }
    // New ones play (once each while their entity stays: a one-shot isn't played again), from
    // where their entity is.
    listen();
    for (const entt::entity e : scene_.registry.view<const SoundSource>()) {
        const SoundSource& s = scene_.registry.get<SoundSource>(e);
        if (!s.autoplay || sources_.contains(e)) continue;
        sources_[e] = {s.event, s.volume, play_sound(s.event, s.volume, e)};
    }
    // Placed sounds follow their entities (one that's gone leaves its sound where it was).
    for (auto it = attached_.begin(); it != attached_.end();) {
        if (!sounds_->playing(it->first)) {
            it = attached_.erase(it);
            continue;
        }
        if (scene_.registry.valid(it->second))
            sounds_->set_position(it->first, scene_.world_position(it->second));
        ++it;
    }
    update_reverb();
    update_footsteps();
    update_weather_sounds();
    if (music_ || ambience_) {
        const audio::MusicContext where = music_context();
        if (music_) {
            music_->set_context(where);
            music_->update(dt);
        }
        if (ambience_) {
            ambience_->set_context(where);
            ambience_->update(dt);
        }
    }
    sounds_->update(dt);
    for (std::string& p : sounds_->take_problems()) sound_problems_.push_back(std::move(p));
}

std::vector<std::string> Game::take_sound_problems() {
    return std::exchange(sound_problems_, {});
}

void Game::load_characters() {
    for (std::string& p :
         characters_.load(scene_, [this](const std::string& asset) { return read_texture(asset); }))
        character_problems_.push_back(std::move(p));
}

void Game::upload_meshes(render::Renderer& renderer) {
    load_characters();
    characters_.set_player(physics_->player());
    characters_.update(scene_, 0.0f);  // (posed from the start: a first frame draws them)
    characters_.upload(renderer);
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
    const size_t known = character_problems_.size();
    load_characters();
    errors.insert(errors.end(), character_problems_.begin() + std::ptrdiff_t(known),
                  character_problems_.end());
    characters_.upload(renderer);
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
    rebuild_weather();
}

void Game::rebuild_terrain() {
    nav_world_.reset();    // (its jobs read the terrain)
    minimap_job_.reset();  // (so does the minimap's: its future waits for it)
    if (terrain_ && terrain_renderer_) terrain_->release(*terrain_renderer_);
    terrain_.reset();
    place_again();  // (none without terrain)
    // The terrain's sea (its level may have changed): things float or sink, the player swims.
    if (physics_)
        physics_->world().set_water(scene_.settings.body && scene_.settings.terrain
                                        ? std::optional<double>(scene_.settings.terrain->sea_level)
                                        : std::nullopt);
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
    nav_world_ = std::make_unique<NavWorld>(terrain_->terrain(), jobs_.get());
    nav_stale_ = true;  // (its scenery is set on the next update)
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
    if (nav_world_) nav_world_->wait();  // (its jobs read the terrain being changed)
    terrain_->set_stamps(std::move(stamps));
    if (nav_world_) nav_world_->ground_changed();
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
    if (stream_all_) {  // a scene just loaded: its meshes at once (its ground as it comes, the
        if (renderer)   // picture dark until it's there: update_scene_change)
            for (const std::string& problem : upload_new_meshes(*renderer)) (void)problem;
        stream_all_ = false;
    }
    if (terrain_) {
        // From space the whole planet is in view (its far side too, round the curve).
        if (in_space() && scene_.settings.body)
            terrain_->set_range(std::max(double(kTerrainViewDistance),
                                         altitude() + 3.0 * scene_.settings.body->shape.radius()));
        terrain_->update(camera, renderer, wait);
        ++streams_since_load_;
        streamed_ = true;
    }
    if (renderer) characters_.upload(*renderer);  // (a player's first-person arms, as made)
    if (placed_changed_ && renderer) {            // the placed objects' meshes, as they come
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
    if (asset.starts_with("snd:")) {  // a sound: read again when it next plays
        if (sounds_) sounds_->forget(asset);
        return true;
    }
    // A texture (or a surface shader, 5.17): the materials using it are made again from the
    // new package; a shader in place of its old one, which stays if the new one can't be used.
    materials_.forget(asset);
    bool texture_used = false;
    for (entt::entity e : scene_.registry.view<const Material>())
        texture_used = texture_used || scene_.registry.get<Material>(e).uses(asset);
    if (texture_used)
        for (std::string& problem : materials_.upload(
                 scene_, renderer, [this](const std::string& t) { return read_texture(t); }))
            reload_problems_.push_back(std::move(problem));
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
    characters_.draws(scene_, origin, materials_, hidden, out);
    if (hidden != entt::null) {  // first person: the player's arms, held at the camera
        if (const auto cam = scene_.camera(1.0f)) {
            glm::mat4 frame(glm::inverse(glm::mat3(cam->view)));
            frame[3] = glm::vec4(glm::vec3(cam->origin - origin), 1.0f);
            characters_.first_person_draws(scene_, hidden, frame, materials_, out);
        }
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
    if (show_navigation_ && nav_world_) {
        const glm::vec4 colour(0.2f, 0.95f, 0.75f, 1.0f);
        for (const auto& [a, b] : nav_world_->edges()) {  // (lifted a little along up)
            const glm::dvec3 up_a = terrain_->terrain().body().up(a) * 0.05;
            const glm::dvec3 up_b = terrain_->terrain().body().up(b) * 0.05;
            lines.push_back({glm::vec3(a + up_a - origin), glm::vec3(b + up_b - origin), colour});
        }
    }
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

// Characters whose graphs have root motion move as their clips do (along their own frame: its
// scale and rotation), except the player (it walks as its controls say) and physics bodies.
void Game::apply_root_motion() {
    auto& reg = scene_.registry;
    const entt::entity player = physics_ ? physics_->player() : entt::entity(entt::null);
    for (const auto& [e, motion] : characters_.motions()) {
        if (!reg.valid(e) || e == player || reg.all_of<RigidBody>(e)) continue;
        Transform& t = reg.get_or_emplace<Transform>(e);
        const glm::mat3 r(rotation_matrix(t.rotation));
        t.position += glm::dvec3(r * (t.scale * motion));
    }
}

void Game::follow_path(std::vector<glm::dvec3> points) {
    route_ = std::move(points);
}

glm::vec3 Game::route_walk(entt::entity player) {
    if (player == entt::null || !scene_.registry.valid(player)) {
        route_.clear();
        return glm::vec3(0.0f);
    }
    const glm::dvec3 at = scene_.world_position(player);
    const glm::dvec3 up = scene_.settings.body ? scene_.settings.body->up(at) : glm::dvec3(0, 1, 0);
    // The next point not yet reached (within reach along the ground: heights differ a little).
    while (!route_.empty()) {
        glm::dvec3 d = route_.front() - at;
        d -= glm::dot(d, up) * up;
        if (glm::length(d) > (route_.size() == 1 ? 0.3 : 0.8)) {
            const Player& p = scene_.registry.get<Player>(player);
            return glm::vec3(glm::normalize(d) * double(p.speed));
        }
        route_.erase(route_.begin());
    }
    return glm::vec3(0.0f);
}

void Game::stream_navigation(bool wait) {
    if (!nav_world_) return;
    if (nav_stale_) {
        nav_stale_ = false;
        nav_world_->set_scenery(scene_,
                                [this](const std::string& asset) { return outline_mesh(asset); });
    }
    entt::entity player = entt::null;
    for (entt::entity e : scene_.registry.view<const Player>()) player = e;
    const glm::dvec3 around =
        player != entt::null ? scene_.world_position(player) : camera(1.0f).origin;
    nav_world_->update(around, wait);
}

const NavMesh* Game::navmesh() const {
    if (nav_world_) return nullptr;  // (terrain: nav_world())
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
    if (nav_world_) return nav_world_->find_path(from, to);
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
        arrival_settled_ = false;
        arrival_waited_ = 0.0f;
        streamed_ = false;
        return;
    }
    // Dark until where the player arrives is ready (made here when nothing else streams it).
    if (!arrival_settled_) {
        if (!streamed_ && terrain_) stream(nullptr, camera(1.0f).origin, false);
        streamed_ = false;
        arrival_waited_ += dt;
        if (!arrival_ready() && arrival_waited_ < kArrivalWait) {
            fade_ = 1.0f;
            c.t = 0.0f;
            return;
        }
        arrival_settled_ = true;
    }
    fade_ = 1.0f - part;
    if (part >= 1.0f) {
        change_.reset();
        fade_ = 0.0f;
    }
}

bool Game::arrival_ready() const {
    if (!terrain_) return true;
    return streams_since_load_ > 0 && terrain_->stats().complete;
}

// ---- system space (milestone 5.16) -------------------------------------------------------------

void Game::enter_space(float fade) {
    change_scene(kSystemSpace, "", fade);
}

double Game::altitude() const {
    if (!scene_.settings.body) return player_position().y;
    return scene_.settings.body->surface(player_position()).height;
}

Scene Game::make_space() const {
    const SceneSettings& from = scene_.settings;
    const Shape::Kind kind = from.body ? from.body->shape.kind() : Shape::Kind::BoundedArea;
    const bool planet = kind == Shape::Kind::SphereOutside;
    if (!planet && kind != Shape::Kind::SphereInside && kind != Shape::Kind::CurvedCylinder)
        throw SceneError(
            "only a planet, a habitat or a hollow world has space outside it: "
            "scene '" +
            scene_.id + "' isn't one");
    Scene space;
    space.id = kSystemSpace;
    if (planet) {
        // The planet as it is (its ground, sculpting and all), with no weather: space has none.
        space.settings.body = from.body;
        space.settings.terrain = from.terrain;
        space.settings.world = from.world;
        space.settings.recipe = from.recipe;
        if (space.settings.recipe) space.settings.recipe->weather.clear();
        space.settings.ceiling = from.ceiling;  // (its own, if set: the one crossed back down)
    }  // (an inside world: its hull, from outside, below; no body: up is +Y out here)
    space.settings.sun_mode = from.sun_mode;
    space.settings.sky = glm::vec3(0.0f);
    space.settings.ambient = glm::vec3(0.03f, 0.03f, 0.04f);
    space.terrain_stamps = scene_.terrain_stamps;
    space.objects = scene_.objects;
    space.environments = scene_.environments;
    space.sound_events = scene_.sound_events;
    space.playlists = scene_.playlists;
    space.ambience = scene_.ambience;
    space.bus_volumes = scene_.bus_volumes;
    space.weather_types = scene_.weather_types;
    // The ship: above where the player is (or, outside an inside world, just outside its hull
    // there, the hull drawn).
    const Body& body = *from.body;
    glm::dvec3 ship, up(0.0, 1.0, 0.0);
    if (planet) {
        SurfacePoint p = body.surface(player_position());
        p.height =
            space_start_.value_or(std::max(std::max(p.height, 0.0) + kShipStart, ceiling() + 50.0));
        ship = body.position(p);
        up = body.up(ship);
    } else {
        const double r = body.shape.radius();
        glm::dvec3 out = player_position() - body.origin;
        const bool tube = kind == Shape::Kind::CurvedCylinder;
        if (tube) out.z = 0.0;
        out = glm::length(out) > 1e-6 ? glm::normalize(out) : glm::dvec3(0.0, -1.0, 0.0);
        ship = body.origin + out * (r + 80.0);
        if (tube) ship.z = player_position().z;
        const double d = 2.0 * r;
        const auto hull_vec = [](const glm::dvec3& v) { return nlohmann::json{v.x, v.y, v.z}; };
        space.put_entity("space_hull", "Hull", "",
                         {{"transform",
                           {{"position", hull_vec(body.origin)},
                            {"scale", tube ? nlohmann::json{d, d, body.shape.face_size().y}
                                           : nlohmann::json{d, d, d}}}},
                          {"mesh", {{"asset", tube ? kBuiltinCylinder : kBuiltinSphere}}},
                          {"material",
                           {{"color", tube ? nlohmann::json{0.55, 0.57, 0.62, 1.0}
                                           : nlohmann::json{0.45, 0.38, 0.32, 1.0}},
                            {"metallic", tube ? 0.7 : 0.0},
                            {"roughness", tube ? 0.4 : 0.9}}}});
    }
    const auto vec = [](const glm::dvec3& v) { return nlohmann::json{v.x, v.y, v.z}; };
    space.put_entity(
        "space_ship", "Ship", "",
        {{"transform", {{"position", vec(ship)}, {"scale", {1.6, 0.6, 3.2}}}},
         {"player", {{"fly", true}, {"speed", 40}, {"radius", 1.0}, {"height", 2.0}}},
         {"mesh", {{"asset", kBuiltinCube}}},
         {"material",
          {{"color", {0.75, 0.78, 0.85, 1.0}}, {"metallic", 0.6}, {"roughness", 0.35}}}});
    space.put_entity("space_camera", "Camera", "",
                     {{"transform", {{"position", vec(ship + up * 6.0)}}},
                      {"camera",
                       {{"fov", 60},
                        {"near", 0.5},
                        {"far", 4.0e6},
                        {"view", "third"},
                        {"views",
                         {{"third",
                           {{"distance", 16},
                            {"height", 1.5},
                            {"min_distance", 6},
                            {"max_distance", 400},
                            {"radius", 0.0}}}}}}}});
    // The sun: the planet scene's, as placed (it follows the clock there and here alike).
    for (const entt::entity e : scene_.registry.view<const DirectionalLight, const Transform>()) {
        const DirectionalLight& l = scene_.registry.get<DirectionalLight>(e);
        const Transform& t = scene_.registry.get<Transform>(e);
        space.put_entity("space_sun", "Sun", "",
                         {{"transform", {{"rotation", {t.rotation.x, t.rotation.y, t.rotation.z}}}},
                          {"light",
                           {{"type", "directional"},
                            {"color", {l.color.x, l.color.y, l.color.z}},
                            {"intensity", l.intensity},
                            {"follows_clock", l.follows_clock}}}});
        break;
    }
    return space;
}

double Game::ceiling() const {
    const auto& body = scene_.settings.body;
    if (!body || body->shape.kind() != Shape::Kind::SphereOutside) return 0.0;
    return scene_.settings.ceiling.value_or(std::max(300.0, 0.2 * body->shape.radius()));
}

void Game::set_flying(entt::entity player, bool fly) {
    if (!scene_.registry.valid(player) || !scene_.registry.all_of<Player>(player)) return;
    Player& p = scene_.registry.get<Player>(player);
    if (p.fly == fly) return;
    p.fly = fly;
    physics_->rebuild(scene_, player);  // (a character again, or none)
}

// Up through a planet's ceiling into space; down through it from space, landing below; back
// to an inside world's hull and in through the way out.
void Game::cross_atmosphere() {
    if (change_) return;
    if (!in_space()) {
        const double top = ceiling();
        if (top > 0.0 && altitude() > top) {
            space_start_ = altitude() + 10.0;  // (on, from where they are)
            enter_space(0.6f);
        }
        return;
    }
    if (space_hull_) {
        const Hull& h = *space_hull_;
        const glm::dvec3 d = player_position() - h.centre;
        const double outside = h.kind == Shape::Kind::CurvedCylinder
                                   ? std::max(glm::length(glm::dvec2(d.x, d.y)) - h.radius,
                                              std::abs(d.z) - 0.5 * h.length)
                                   : glm::length(d) - h.radius;
        if (outside < 25.0) change_scene(space_from_, space_return_, 0.6f);
        return;
    }
    if (space_ceiling_ > 0.0 && scene_.settings.body && altitude() < space_ceiling_ - 10.0) {
        landing_ = scene_.settings.body->surface(player_position());
        change_scene(space_from_, "", 0.6f);
    }
}

// A flying player: along where the camera looks (move), up (jump) and down (descend), at its
// speed or a twentieth of its height above the ground a second, whichever is more (run: times
// its run), turned to face the way it goes.
void Game::fly(const Actions& actions, float dt) {
    entt::entity e = entt::null;
    for (const entt::entity p : scene_.registry.view<const Player>())
        if (scene_.registry.get<Player>(p).fly) e = p;
    if (e == entt::null) return;
    const Player& player = scene_.registry.get<Player>(e);
    const render::Camera cam = camera(1.0f);
    const glm::mat3 to_world = glm::inverse(glm::mat3(cam.view));
    const glm::dvec3 ahead = glm::dvec3(to_world * glm::vec3(0.0f, 0.0f, -1.0f));
    const glm::dvec3 right = glm::dvec3(to_world * glm::vec3(1.0f, 0.0f, 0.0f));
    Transform& t = scene_.registry.get<Transform>(e);
    const glm::dvec3 up =
        scene_.settings.body ? scene_.settings.body->up(t.position) : glm::dvec3(0.0, 1.0, 0.0);
    glm::dvec3 way =
        ahead * double(actions.axis("move_y")) + right * double(actions.axis("move_x")) +
        up * double((actions.down("jump") ? 1.0f : 0.0f) - (actions.down("descend") ? 1.0f : 0.0f));
    const double amount = glm::length(way);
    if (amount < 1e-6) return;
    way /= std::max(amount, 1.0);
    const double speed = std::max(double(player.speed), std::abs(altitude()) / 20.0) *
                         (actions.down("run") ? double(player.run) : 1.0);
    t.position += way * speed * double(dt);
    const glm::dvec3 heading = glm::normalize(way);
    t.rotation = look_rotation(glm::vec3(0.0f), glm::vec3(heading));
}

// ---- rooms (milestone 5.16) --------------------------------------------------------------------

namespace {

nlohmann::json transform_json(const Transform& t) {
    return {{"position", {t.position.x, t.position.y, t.position.z}},
            {"rotation", {t.rotation.x, t.rotation.y, t.rotation.z}},
            {"scale", {t.scale.x, t.scale.y, t.scale.z}}};
}

bool moved(const Transform& a, const Transform& b) {
    return glm::length(a.position - b.position) > 1e-3 ||
           glm::length(a.rotation - b.rotation) > 0.01f || glm::length(a.scale - b.scale) > 1e-4f;
}

// An entity's components as its row in the package's database has them (JSON text).
std::string designed_components(const std::filesystem::path& database, const std::string& id) {
    sqlite3* db = nullptr;
    const std::u8string u8 = database.u8string();
    std::string out;
    if (sqlite3_open_v2(std::string(u8.begin(), u8.end()).c_str(), &db, SQLITE_OPEN_READONLY,
                        nullptr) == SQLITE_OK) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT components FROM entity WHERE id = ?", -1, &st,
                               nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_ROW)
                if (const auto* c = sqlite3_column_text(st, 0))
                    out = reinterpret_cast<const char*>(c);
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

}  // namespace

void Game::note_loaded() {
    loaded_.clear();
    for (const entt::entity e : scene_.registry.view<const EntityInfo, const Transform>())
        loaded_[scene_.registry.get<EntityInfo>(e).id] = scene_.registry.get<Transform>(e);
}

std::string Game::capture_room() const {
    nlohmann::json moved_ = nlohmann::json::object(), removed = nlohmann::json::array();
    for (const auto& [id, was] : loaded_) {
        const entt::entity e = scene_.find(id);
        if (e == entt::null) {
            removed.push_back(id);
            continue;
        }
        if (scene_.registry.all_of<Player>(e)) continue;  // (the player goes on with them)
        const Transform& now = scene_.registry.get<Transform>(e);
        if (moved(now, was)) moved_[id] = transform_json(now);
    }
    return nlohmann::json{{"moved", moved_}, {"removed", removed}}.dump();
}

void Game::apply_room(const std::string& room) {
    if (room.empty()) return;
    const nlohmann::json j = nlohmann::json::parse(room, nullptr, false);
    if (!j.is_object()) return;
    const nlohmann::json moved_ones = j.value("moved", nlohmann::json::object());
    for (const auto& [id, t] : moved_ones.items()) {  // (named: items() of a temporary dangles)
        const entt::entity e = scene_.find(id);
        if (e == entt::null || !scene_.registry.all_of<Transform>(e)) continue;
        Transform& to = scene_.registry.get<Transform>(e);
        const auto vec = [&](const char* key, auto& into) {
            const auto it = t.find(key);
            if (it != t.end() && it->is_array() && it->size() == 3)
                for (int k = 0; k < 3; ++k)
                    into[k] = static_cast<std::remove_reference_t<decltype(into[k])>>(
                        (*it)[size_t(k)].template get<double>());
        };
        vec("position", to.position);
        vec("rotation", to.rotation);
        vec("scale", to.scale);
    }
    for (const auto& id : j.value("removed", nlohmann::json::array()))
        if (id.is_string()) scene_.remove_entity(id.get<std::string>());
}

void Game::carry_player() {
    if (carried_player_.empty()) return;
    nlohmann::json c = nlohmann::json::parse(carried_player_, nullptr, false);
    if (!c.is_object()) return;
    entt::entity here = entt::null;
    for (const entt::entity e : scene_.registry.view<const Player>()) here = e;
    std::string id = carried_id_, name = carried_name_, parent;
    if (here != entt::null) {  // (in the place of this scene's own, where it stands)
        const EntityInfo& info = scene_.registry.get<EntityInfo>(here);
        id = info.id;
        name = info.name;
        if (info.parent != entt::null && scene_.registry.valid(info.parent))
            parent = scene_.registry.get<EntityInfo>(info.parent).id;
        c["transform"] = transform_json(scene_.registry.get<Transform>(here));
    }
    scene_.put_entity(id, name, parent, c);
}

std::string Game::room_state(const std::string& scene) const {
    if (scene == scene_.id) return capture_room();
    const auto it = rooms_.find(scene);
    return it == rooms_.end() ? std::string() : it->second;
}

void Game::load_scene(const std::string& id, const std::string& arrival) {
    // Everything that can fail first, so a failed change leaves the scene as it was.
    const bool to_space = id == kSystemSpace;
    Scene next =
        to_space ? make_space() : Scene::load(package_.root / package_.manifest.database, id);
    std::optional<glm::dmat4> at;
    if (!arrival.empty() && !to_space) {  // (into space, it's the way back in)
        const entt::entity a = next.find(arrival);
        if (a == entt::null || !next.registry.all_of<Arrival>(a))
            throw SceneError("scene '" + id + "' has no arrival '" + arrival + "'");
        at = next.world_matrix(a);
    }
    const bool had_camera = camera_ && camera_->active();
    const CameraRig::State view = had_camera ? camera_->state() : CameraRig::State{};

    // Leave the scene: its ground, placed objects and physics go, and its entities' sounds stop
    // (a script's music plays on).
    // The scene left is remembered as it is (not space: it's made afresh); the player goes on,
    // as the game began with them.
    if (!in_space()) rooms_[scene_.id] = capture_room();
    if (to_space) {
        space_from_ = scene_.id;
        space_return_ = arrival;
        space_ceiling_ = ceiling();
        space_hull_.reset();
        const auto& body = scene_.settings.body;
        if (body && body->shape.kind() != Shape::Kind::SphereOutside)
            space_hull_ = Hull{body->shape.kind(), body->origin, body->shape.radius(),
                               body->shape.face_size().y};
    } else {
        space_from_.clear();
        space_return_.clear();
        space_ceiling_ = 0.0;
        space_hull_.reset();
    }
    space_start_.reset();
    if (carried_player_.empty() && scene_.id != kSystemSpace) {
        for (const entt::entity e : scene_.registry.view<const Player>()) {
            const EntityInfo& info = scene_.registry.get<EntityInfo>(e);
            carried_id_ = info.id;
            carried_name_ = info.name;
            carried_player_ =
                designed_components(package_.root / package_.manifest.database, info.id);
        }
    }
    stop_sources();
    nav_world_.reset();
    minimap_job_.reset();  // (it reads the terrain: its future waits for it)
    if (terrain_ && terrain_renderer_) terrain_->release(*terrain_renderer_);
    terrain_.reset();
    placed_.reset();
    ground_.reset();
    inside_.clear();
    exits_inside_.clear();
    trigger_events_.clear();
    outline_meshes_.clear();
    characters_.clear_states();
    scene_ = std::move(next);
    note_loaded();  // (as designed: what moves from here on is what's remembered)
    if (const auto room = rooms_.find(id); room != rooms_.end()) apply_room(room->second);
    if (!to_space) carry_player();  // (in space, the player is the ship)
    if (sounds_)                    // (the game's events, as this scene's package has them)
        for (const auto& [eid, ev] : scene_.sound_events) sounds_->add(ev);
    if (music_)  // (the music plays on: the next update chooses for the new scene)
        for (const auto& [pid, p] : scene_.playlists) music_->add(p);
    if (ambience_)
        for (const auto& [aid, a] : scene_.ambience) ambience_->add(a);

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
    // Coming down from space: below where the ship came down (flying, just under the ceiling;
    // walking, on the ground there).
    bool to_ground = false;
    if (landing_ && player != entt::null && scene_.settings.body) {
        SurfacePoint p = *landing_;
        const bool flies = scene_.registry.get<Player>(player).fly;
        p.height = flies ? ceiling() - 30.0 : 0.0;
        scene_.registry.get<Transform>(player).position = scene_.settings.body->position(p);
        to_ground = !flies;
    }
    landing_.reset();
    rebuild_terrain();  // (lifts the player onto the ground there, and makes it solid)
    if (to_ground && terrain_) put_on_ground(player);
    rebuild_weather();
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
    streams_since_load_ = 0;
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
    if (scene == kSystemSpace) {  // (space: outside a planet, a habitat or a hollow world)
        const auto& body = scene_.settings.body;
        const bool has = body && (body->shape.kind() == Shape::Kind::SphereOutside ||
                                  body->shape.kind() == Shape::Kind::SphereInside ||
                                  body->shape.kind() == Shape::Kind::CurvedCylinder);
        return has ? std::string()
                   : "only a planet, a habitat or a hollow world has space outside it: scene '" +
                         scene_.id + "' isn't one";
    }
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
