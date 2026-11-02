// SPDX-License-Identifier: Apache-2.0
// A loaded game: package + game.pak + entry scene, with meshes uploaded to the renderer.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "midgard/calendar.h"
#include "midgard/camera_rig.h"
#include "midgard/input.h"
#include "midgard/materials.h"
#include "midgard/nav_world.h"
#include "midgard/navmesh.h"
#include "midgard/package.h"
#include "midgard/pak.h"
#include "midgard/placed_objects.h"
#include "midgard/player_profile.h"
#include "midgard/render/renderer.h"
#include "midgard/scene.h"
#include "midgard/scene_physics.h"
#include "midgard/terrain_collision.h"
#include "midgard/terrain_streamer.h"
#include "midgard/time_service.h"
#include "midgard/world_light.h"
#include "midgard/world_save.h"

namespace midgard {

// Mesh asset ids that are built into the engine rather than stored in game.pak.
inline constexpr const char* kBuiltinCube = "builtin:cube";

// The player entered or left a trigger this frame.
struct TriggerEvent {
    entt::entity trigger = entt::null;
    entt::entity other = entt::null;  // the player
    bool entered = true;              // false: left
};

class Game {
public:
    // Opens <dir>/manifest.json, its database and (if present) game.pak, and loads the entry
    // scene. Throws PackageError, ManifestError, SceneError or AssetError.
    explicit Game(const std::filesystem::path& package_dir);

    const PackageInfo& package() const { return package_; }
    Scene& scene() { return scene_; }
    const Scene& scene() const { return scene_; }

    // Reads every mesh and texture the scene uses (from game.pak or the built-ins) and uploads it
    // once, with the materials that use the textures.
    void upload_meshes(render::Renderer& renderer);

    // Live editing: uploads meshes and textures the scene uses that aren't on the GPU yet. Those
    // that fail to load are skipped (and not retried); the returned messages say why.
    std::vector<std::string> upload_new_meshes(render::Renderer& renderer);

    // An object's template changed (the editor's live link): its instances are filled in again.
    void set_object(const std::string& object_id, const std::string& parts_json);

    // Live editing (the editor's live link). Each throws SceneError on malformed data.
    // entity: {"id", "name", "parent", "components"} as in the entity table.
    void put_entity(const nlohmann::json& entity);
    void set_component(const std::string& entity_id, const std::string& component,
                       const nlohmann::json& value);
    void remove_entity(const std::string& entity_id);

    // Hot reload (the editor rewrote part of the package on disk, then said which part).
    void reload_scene_settings();
    void reload_entity(const std::string& entity_id);
    // Re-reads manifest.json and game.pak, then the mesh or texture with this content id.
    // Returns false if nothing in the scene uses it. Throws on a broken package.
    bool reload_asset(const std::string& asset, render::Renderer& renderer);

    // Terrain (midgard/terrain_streamer.h): chooses the chunks to draw from `camera`, generates
    // those missing on the game's job system and uploads them. Call once a frame before
    // draws(); wait: finish every chunk wanted now (the first frame, screenshots).
    void stream_terrain(render::Renderer& renderer, const glm::dvec3& camera, bool wait = false);
    // The same without a renderer (headless runs and tests): chunks are made, not uploaded.
    void stream_terrain(const glm::dvec3& camera, bool wait = false);
    const TerrainStreamer* terrain() const { return terrain_.get(); }
    const TerrainCollision* ground() const { return ground_.get(); }
    const PlacedObjects* placed() const { return placed_.get(); }
    // New sculpting and painting (the editor's live link): the ground drawn and the solid ground
    // follow.
    void set_terrain_stamps(std::vector<TerrainStamp> stamps);

    // One draw per entity with a mesh, and the terrain's chunks.
    // What to draw, relative to the camera's origin (camera-relative rendering).
    std::vector<render::DrawItem> draws(const glm::dvec3& origin) const;
    // The terrain's look for the renderer (its world's shape, climate and biomes; the season
    // as draws() last set it), or a flat one without terrain.
    render::TerrainLook terrain_look() const;

    // Outlines of the scene's colliders, triggers and player for debug drawing, relative to the
    // camera's origin (midgard/collider_outlines.h).
    std::vector<render::DebugLine> debug_lines(const glm::dvec3& origin) const;

    // The scene's camera, or a default view of the origin if it has none.
    render::Camera camera(float aspect) const;

    // The package's input bindings (built-in defaults for packages older than schema 3).
    InputMap input_map() const;

    // One frame of gameplay: the player moves, the third-person camera follows, physics steps
    // (bodies follow moved entities, dynamic bodies move theirs), and triggers note who entered
    // or left them (trigger_events, until the next update).
    void update(const Actions& actions, float dt);
    const std::vector<TriggerEvent>& trigger_events() const { return trigger_events_; }
    const CameraRig& camera_rig() const { return *camera_; }
    CameraRig& camera_rig() { return *camera_; }
    ScenePhysics& physics() { return *physics_; }

    // The player's profile file (midgard/player_profile.h). Its camera view and zoom are used
    // now; from then on the profile is saved whenever the view changes and by save_profile()
    // (call it when the game ends). Without a profile nothing is saved. Throws ProfileError if
    // the file is damaged (it is still used, and replaced at the next save).
    void use_profile(const std::filesystem::path& file);
    void save_profile();
    const std::filesystem::path& profile_file() const { return profile_file_; }
    const PhysicsLayers& physics_layers() const { return layers_; }

    // ---- time (milestone 5.7) ----
    // The clock: it follows the standard calendar (a built-in one for packages from before
    // calendars), starts at its start date and runs at its time scale. Game::update moves it on.
    TimeService& time() { return time_; }
    const TimeService& time() const { return time_; }
    const Calendar& standard_calendar() const { return time_.calendar(0); }
    // The calendar of the body the player is on: the standard one, until bodies have their own.
    const Calendar& local_calendar() const { return time_.calendar(0); }
    // The clock's signals of the last update, and of advance_time() calls made before it.
    const std::vector<TimeEvent>& time_events() const { return time_events_; }
    // Moves the clock on at once (sleeping, waiting). Its signals come with the next update.
    void advance_time(std::int64_t game_seconds, SkipPolicy policy = SkipPolicy::All);
    // Puts the clock at a moment, reporting nothing (the editor's scrubber, over the live link).
    void set_time(std::int64_t time);
    // Player-facing text in English: a key of the package's string table (the key itself if
    // it has none).
    std::string text(const std::string& key) const;

    // The world being played (midgard/world_save.h): its saved clock is restored now (to the
    // second), and save_world() keeps it (call it when the game ends). Without one, nothing is
    // kept. Throws WorldError.
    void use_world(const std::filesystem::path& folder);
    void save_world();
    const WorldSave* world() const { return world_ ? &*world_ : nullptr; }

    // ---- navigation (milestone 5.12) ----
    // Where agents can walk in a scene without terrain: built from its static colliders when
    // first asked for, and again after the scene changes (live edits, a scene change). None
    // where "up" isn't the same everywhere, or with terrain (see nav_world()).
    const NavMesh* navmesh() const;
    // A scene with terrain: its navigation, a mesh per face made round the player as they go
    // (midgard/nav_world.h). Update moves it on; stream_navigation(true) makes it all now.
    const NavWorld* nav_world() const { return nav_world_.get(); }
    void stream_navigation(bool wait = false);
    // The player walks along these points at its speed (instead of following the controls)
    // until it reaches the last; an empty list stops it. Tests, cutscenes, click-to-move.
    void follow_path(std::vector<glm::dvec3> points);
    bool following_path() const { return !route_.empty(); }
    // The way from one place to another, in the scene's frame, or none (either too far from
    // anywhere walkable, or no navigation in this scene).
    std::optional<NavMesh::Path> find_path(const glm::dvec3& from, const glm::dvec3& to) const;
    // Whether debug_lines() includes the navigation mesh's edges.
    void set_show_navigation(bool show) { show_navigation_ = show; }

    // ---- moving between scenes (milestone 5.11a) ----
    // Takes the player to another scene: the picture fades out over `fade` seconds, the scene
    // being left is unloaded and the target loaded with the player at the arrival (an entity
    // with an arrival; empty: where the scene puts its player), its ground round the player is
    // made, and the picture fades in again. A scene exit the player walks into (activation
    // "enter") calls this; so do scripts. Asked again while fading out: ignored; while fading in
    // (a script's on_room_enter): it fades out again for the new one.
    void change_scene(const std::string& scene, const std::string& arrival = "", float fade = 0.5f);
    // Takes a scene exit (an entity with a scene_exit, whatever its activation: a script's
    // choice). False if the entity isn't one.
    bool take_exit(entt::entity exit);
    // Whether the package has a scene with this id.
    bool has_scene(const std::string& scene) const;
    // Why a change to this scene and arrival can't be made, or empty if it can.
    std::string arrival_problem(const std::string& scene, const std::string& arrival) const;
    // The scene the player came from (empty until the first change).
    const std::string& previous_scene() const { return previous_scene_; }
    // Called once the picture is dark, just before the scene is left, with the target's id
    // (the scripts' on_room_exit).
    void on_leaving_scene(std::function<void(const std::string&)> callback) {
        leaving_ = std::move(callback);
    }
    bool changing_scene() const { return change_.has_value(); }
    // How dark the picture is: 0 clear, 1 black (the renderer's exposure scales by 1 - fade).
    float fade() const { return fade_; }
    // Whether the last update swapped the scene (scripts start again from it).
    bool scene_changed() const { return scene_changed_; }
    // Why the last change failed (a missing scene or arrival), or empty: the player stays.
    const std::string& scene_change_error() const { return scene_change_error_; }

private:
    render::MeshData load_mesh(const std::string& asset) const;
    void refresh_camera(entt::entity edited);
    CameraSweep camera_sweep() const;
    void rebuild_physics();
    void update_triggers();
    void update_sun();  // suns that follow the clock: the solar phase now

    PackageInfo package_;
    std::unique_ptr<Pak> pak_;
    Scene scene_;
    std::unique_ptr<CameraRig> camera_;
    std::filesystem::path profile_file_;
    PlayerProfile profile_;
    CameraView saved_view_ = CameraView::Third;
    PhysicsLayers layers_;
    std::unique_ptr<ScenePhysics> physics_;
    std::unordered_map<std::string, render::MeshId> meshes_;
    std::unordered_set<std::string> failed_meshes_;
    MaterialCache materials_;
    std::vector<uint8_t> read_texture(const std::string& asset) const;
    // Meshes read for collider outlines (once each; nullptr: couldn't be read).
    mutable std::unordered_map<std::string, std::unique_ptr<render::MeshData>> outline_meshes_;
    std::unordered_set<entt::entity> inside_;  // triggers the player is in
    std::vector<TriggerEvent> trigger_events_;
    TimeService time_;
    std::vector<TimeEvent> time_events_, skipped_;          // this frame's; from advances since
    std::unordered_map<std::string, std::string> strings_;  // key -> English
    std::optional<WorldSave> world_;
    // Terrain: its job system and streamer (none without a world shape and terrain settings),
    // and the renderer its chunks are on (to free them when the settings change).
    void rebuild_terrain();
    std::unique_ptr<JobSystem> jobs_;
    std::unique_ptr<TerrainStreamer> terrain_;
    WorldLight world_light_;  // a curved cylinder's light rod or a hollow world's light sphere
    render::Renderer* terrain_renderer_ = nullptr;
    // The terrain's solid ground round the player (in physics_'s world).
    std::unique_ptr<TerrainCollision> ground_;
    // Objects the world type places over the ground, round the player (midgard/placed_objects.h).
    std::unique_ptr<PlacedObjects> placed_;
    bool placed_changed_ = false;  // new instances whose meshes may need uploading
    void place_again();            // the terrain changed: its placed objects are made again
    void update_ground(int max_tiles = 6);
    void put_on_ground(entt::entity e);
    // Moving between scenes.
    struct SceneChange {
        std::string scene, arrival;
        float fade = 0.5f, t = 0.0f;
        bool swapped = false;
    };
    std::optional<SceneChange> change_;
    float fade_ = 0.0f;
    bool scene_changed_ = false;
    bool stream_all_ = false;  // a scene was just loaded: its view is made in full, at once
    std::string scene_change_error_;
    std::string previous_scene_;
    std::function<void(const std::string&)> leaving_;
    std::unordered_set<entt::entity> exits_inside_;  // scene exits the player is in
    // Navigation, made when asked for after it went stale.
    mutable std::unique_ptr<NavMesh> nav_;
    mutable bool nav_stale_ = true;
    bool show_navigation_ = false;
    const render::MeshData* outline_mesh(const std::string& asset) const;
    // (Last: made from the terrain and the job system, so it goes before them.)
    std::unique_ptr<NavWorld> nav_world_;
    std::vector<glm::dvec3> route_;  // follow_path's points still ahead
    glm::vec3 route_walk(entt::entity player);
    void load_scene(const std::string& scene, const std::string& arrival);
    void update_scene_change(float dt);
    void update_exits(const Actions& actions);
    void note_exits_inside();
    void stream(render::Renderer* renderer, const glm::dvec3& camera, bool wait);
    bool inside_exit(entt::entity exit, const glm::dvec3& p) const;
};

}  // namespace midgard
