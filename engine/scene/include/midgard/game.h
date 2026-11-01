// SPDX-License-Identifier: Apache-2.0
// A loaded game: package + game.pak + entry scene, with meshes uploaded to the renderer.
#pragma once

#include <filesystem>
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
};

}  // namespace midgard
