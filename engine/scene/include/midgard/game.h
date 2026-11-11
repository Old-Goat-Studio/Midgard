// SPDX-License-Identifier: Apache-2.0
// A loaded game: package + game.pak + entry scene, with meshes uploaded to the renderer.
#pragma once

#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "midgard/calendar.h"
#include "midgard/camera_rig.h"
#include "midgard/characters.h"
#include "midgard/compass.h"
#include "midgard/game_settings.h"
#include "midgard/hud.h"
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
#include "midgard/weather_look.h"
#include "midgard/weather_sim.h"
#include "midgard/world_light.h"
#include "midgard/world_save.h"

namespace midgard {

// Mesh asset ids that are built into the engine rather than stored in game.pak.
inline constexpr const char* kBuiltinCube = "builtin:cube";
inline constexpr const char* kBuiltinSphere = "builtin:sphere";      // radius 0.5
inline constexpr const char* kBuiltinCylinder = "builtin:cylinder";  // radius 0.5, along Z

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
    // What went wrong in reload_asset since last asked (a shader the GPU refused, ...).
    std::vector<std::string> take_reload_problems() { return std::exchange(reload_problems_, {}); }

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
    // The scene's characters (midgard/characters.h).
    const Characters& characters() const { return characters_; }
    Characters& characters() { return characters_; }
    // What went wrong reading characters' models and clips (each once, as it happened).
    const std::vector<std::string>& character_problems() const { return character_problems_; }

    // Sound (milestone 5.14): the mixer the game plays through (the runtime's: a device, or
    // one mixed offline; none: silent, and scripts' sounds do nothing). Its sound events come
    // from the package; entities with a sound component play theirs (autoplay) while they're
    // in the scene.
    void set_audio(audio::Mixer* mixer);
    // The Audio panel's edits while the game plays (the live link's audio.put): every sound
    // event, playlist and ambience set, and the buses' volumes, at once. Throws SceneError for
    // malformed ones (nothing changes then).
    void set_audio_data(const nlohmann::json& audio);
    audio::SoundPlayer* sounds() { return sounds_.get(); }
    const audio::SoundPlayer* sounds() const { return sounds_.get(); }
    // Sounds that couldn't be read, events that aren't there (each once), since last asked.
    std::vector<std::string> take_sound_problems();
    // Plays a sound event for a script: from an entity (following it as it moves), from a
    // place, or (neither, or an event that isn't spatial) heard the same everywhere. 0 when it
    // doesn't play (silent game, unknown event, cooling down).
    audio::VoiceId play_sound(const std::string& event, float volume,
                              entt::entity from = entt::null,
                              std::optional<glm::dvec3> at = std::nullopt);
    // How much the listener hears of the reverb zones now (the echo's settings).
    audio::ReverbSettings reverb() const { return reverb_; }
    // The music (midgard/music.h): playlists chosen by where the player is (none: silent).
    audio::MusicDirector* music() { return music_.get(); }
    const audio::MusicDirector* music() const { return music_.get(); }
    // The ambience (midgard/ambience.h): the background sound of where the player is.
    audio::AmbienceDirector* ambience() { return ambience_.get(); }
    const audio::AmbienceDirector* ambience() const { return ambience_.get(); }
    // Where the player is, as the music sees it: the scene, its world type, the biome at the
    // listener and the time of day.
    audio::MusicContext music_context() const;

    // ---- stats, scores and counters (milestone 6.0) ----
    // The player's (midgard/stats.h): defined by the package, kept in the world's save.
    stats::Stats& stats() { return stats_; }
    const stats::Stats& stats() const { return stats_; }
    // The stats that changed in the last update (by themselves, and by scripts since the one
    // before), each once: before and after.
    const std::vector<stats::Change>& stat_changes() const { return stat_changes_; }
    // The Stats panel's edits while the game plays (the live link's stats.put): every stat
    // ({id, name, stat} rows, in order). Values are kept, fitted to the new ranges. Throws
    // SceneError, changing nothing, if one isn't a stat.
    void set_stat_data(const nlohmann::json& rows);

    // ---- the HUD (milestone 6.0) ----
    // Its notes, prompt, timers and switches (midgard/hud.h): scripts' (midgard.ui).
    hud::HudState& hud() { return hud_; }
    const hud::HudState& hud() const { return hud_; }
    // The timers that ended in the last update.
    const std::vector<std::string>& finished_timers() const { return finished_timers_; }

    // ---- screens (milestone 6.0: the Menu editor's; midgard/screen.h) ----
    const std::vector<ScreenDef>& screens() const { return scene_.screens; }
    const ScreenDef* screen(const std::string& id) const;
    // The Menu editor's edits while the game plays (the live link's screens.put): every screen
    // ({id, name, screen} rows, in order). Shown screens stay shown (and go if deleted).
    // Throws SceneError for one that can't be read.
    void set_screen_data(const nlohmann::json& rows);
    // The player clicked a button on a shown screen: it does what it says (closes its screen,
    // opens another), and scripts hear it (midgard.ui.on_click). True if the button quits the
    // game.
    bool ui_click(const std::string& screen, const std::string& element);
    // The player moved a slider or ticked a check: its value, for scripts (midgard.ui.value,
    // on_change).
    void ui_change(const std::string& screen, const std::string& element, double value);
    // The clicks and changes of the last update (and since), for scripts.
    const std::vector<hud::UiEvent>& ui_events() const { return ui_events_; }
    // ---- the game's settings (the Project Page's: splash, crosshair, pointer, menu music) ----
    const GameSettings& game_settings() const { return settings_; }
    // The Project Page's edits while the game plays (the live link's settings.put).
    void set_game_settings(const nlohmann::json& settings);
    // A screen that pauses the game shows: the world stands still (updates take no time, and
    // scripts' on_update isn't called), but its sounds and music carry on (menu music fades in)
    // and scripts hear its buttons. Frozen: set so, or a shown screen pauses.
    void set_frozen(bool frozen) { frozen_ = frozen; }
    // The input context now (milestone 6.1; midgard/input.h): a script's if it chose one, else
    // "menu" while a menu shows, "flying" while the player flies, else "on_foot".
    std::string input_context() const;
    void set_input_context(const std::string& context) { script_context_ = context; }
    const std::string& script_input_context() const { return script_context_; }
    bool frozen() const;

    // ---- the game's own screens (6.0 step 7: midgard/screen.h's roles) ----
    // The screen used as the title, pause menu, settings or game over (nullptr: none).
    const ScreenDef* screen_for(const std::string& role) const;
    // The menu action (Esc, Start, a phone's Back): goes back from the menu on top (the pause
    // menu, settings; not the title or game over), or opens the pause menu. False if there is
    // no pause menu and nothing to go back from (the game then quits, as before menus).
    bool menu();
    // The player back where it arrived in this scene, its stats refilled, standing still; a
    // game over screen goes.
    void respawn();
    // A key button was clicked (binds "key:<action>"): the action waiting for its new key.
    const std::string& rebinding() const { return rebinding_; }
    // The key pressed (SDL's scancode name; "" to give up): the action's keyboard control from
    // now on, kept in the player's profile. Returns the action ("" if none was waiting).
    std::string key_chosen(const std::string& key);
    // The player's keys (profile), by action: the runtime rebinds its input with them.
    std::map<std::string, std::string> player_keys() const;
    // The player's volume for a bus (0 to 1; the profile's, else 1), and setting it.
    double player_volume(const std::string& bus) const;
    void set_player_volume(const std::string& bus, double volume);
    // A packaged file's bytes by content id (a screen's picture). Throws AssetError if the
    // package hasn't it.
    std::vector<uint8_t> read_content(const std::string& id) const { return read_texture(id); }
    // The way the camera faces, in degrees from north (0 north, 90 east; midgard/compass.h), on
    // the world's shape where the player is.
    double heading() const;
    // The minimap's ground around the player (north up): made only once something wants it
    // (a game with a HUD), and again as the player walks away from its middle. None without
    // terrain (flat scenes: the minimap shows its marks alone).
    struct MinimapGround {
        glm::dvec3 centre{0.0};
        double radius = 0.0;  // metres from the middle to each edge
        int size = 0;         // pixels across
        std::vector<std::uint8_t> pixels;
        std::uint64_t version = 0;  // goes up each time it is made again
    };
    // Something wants the minimap's ground from now on: it is made now, and kept up to date
    // (in the background as the player walks: it takes a phone tens of milliseconds).
    void want_minimap() {
        minimap_wanted_ = true;
        update_minimap(true);
    }
    const MinimapGround& minimap() const { return minimap_; }
    // Where the player is from the minimap ground's middle: metres east and north.
    glm::dvec2 minimap_offset() const;
    // What the minimap marks within `radius` metres: scene exits, and entities scripts marked
    // (midgard.ui.mark), each at metres east and north of the player.
    struct MapMark {
        std::string kind;  // "exit" or "mark"
        std::string label;
        double east = 0.0, north = 0.0;
    };
    std::vector<MapMark> map_marks(double radius) const;
    // What a stat that only shows (a position or a clock: stats::StatDef::display) says now:
    // where the player is (midgard/compass.h's place_text), or the date and time on the local
    // calendar (midgard/calendar.h's date_text). Empty for other stats.
    std::string stat_text(const stats::StatDef& stat) const;
    static constexpr double kMinimapRadius = 120.0;  // metres shown each side of the player
    // Its ground is made twice as wide as what shows, and made again when the player is a
    // quarter of the way to its edge, so running never shows past it while the next is made.
    static constexpr double kMinimapGroundRadius = 2.0 * kMinimapRadius;
    static constexpr int kMinimapSize = 256;  // its ground's pixels across

    // ---- weather (milestone 5.15) ----
    // The world's weather (midgard/weather_sim.h), from the scene's weather settings or its
    // world type's; none without any (only a script's forced weather, then).
    weather::WeatherSim* weather() { return weather_.get(); }
    // The weather where the player is (the camera, without one), as of the last update. While
    // a script forces the weather, that, everywhere.
    const weather::Now& weather_now() const { return weather_now_; }
    // Its weather type (none: no weather).
    const weather::WeatherType* weather_type() const;
    // The weather changes of the last update (and those scripts made since the one before):
    // what it was, and what it became.
    struct WeatherChange {
        weather::Now from, to;
    };
    const std::vector<WeatherChange>& weather_changes() const { return weather_changes_; }
    // The forecast where the player is, from now for so many of the calendar's hours: the
    // spells as they will come, forced weather and all.
    std::vector<weather::Spell> weather_forecast(double hours);
    // A script's weather, everywhere, from now: for so many hours (0: until released), then
    // the weather there would have been. False if there is no such weather type.
    bool force_weather(const std::string& weather, double hours);
    void release_weather();
    // The Weather tab's edits while the game plays (the live link's weather.put): every weather
    // type ({id, name, weather} rows, as their table has them), and the weather settings of the
    // scene's world ("settings": null for its world type's own). Throws SceneError for
    // malformed ones (nothing changes then).
    void set_weather_data(const nlohmann::json& data);
    // The weather cell the player is in (none without weather settings).
    std::optional<weather::Cell> weather_cell() const;
    // How the weather looks now (midgard/weather_look.h): easing towards the weather's own over
    // several seconds as it changes, the clouds drifting with its wind, lightning flashing.
    const WeatherLook& weather_look() const { return look_; }
    // The sky, fog and light of the time of day at `origin`, with the weather on them: what the
    // renderer draws.
    render::Environment environment(const glm::dvec3& origin) const;
    render::Light light(const glm::dvec3& origin) const;
    // Lightning flashes since the game started (each followed by its weather's thunder).
    std::size_t lightning_flashes() const { return flashes_; }
    // What falls from the sky now (render::Falling): the weather's rain, snow, ash or sand,
    // easing in and out as it changes (one kind giving way before the next), carried by its wind
    // and gusts.
    const render::Falling& falling() const { return falling_; }
    // What the weather has left on the ground where the player is (render::GroundCover), as the
    // clock runs: wet while it rains (up to the weather's wetness, in about a quarter of an
    // hour), drying over a couple of hours after; snow settling while it snows (covered in two
    // to four hours) and melting after (in a few hours; at once in rain; not while it's below
    // freezing in a world with a climate), its water wetting the ground.
    const render::GroundCover& ground_cover() const { return ground_cover_; }
    // Scripts and tests: what lies there now.
    void set_ground_cover(const render::GroundCover& cover) { ground_cover_ = cover; }
    // The temperature where the player is (degrees Celsius): the climate's there (with the
    // season), and the weather's change; 15 without a climate.
    double temperature_here() const;
    // How the weather treats the player (milestone 5.15's survival groundwork), a few times a
    // second: whether they're under a roof (rays upward) and between walls (rays sideways),
    // the wind reaching them (walls cut it), how wet they are (out in the rain or snow they get
    // wet; under a roof they dry, faster by a fire), the warmth of heat sources near them, and
    // so how warm it feels (midgard/survival.h). Inside a building (a roof and walls all round)
    // no rain or snow is drawn.
    struct Exposure {
        double air = 15.0;         // degrees Celsius
        double feels_like = 15.0;  // degrees Celsius
        double wind = 0.0;         // metres a second, where the player is
        double heat = 0.0;         // degrees from heat sources
        float wetness = 0.0f;      // the player's: 0 dry .. 1 soaked
        float roof = 0.0f;         // how much of the sky above is covered, 0 .. 1
        float walls = 0.0f;        // how much is walled in round about, 0 .. 1
        bool sheltered = false;    // under a roof: rain and snow don't reach them
    };
    const Exposure& exposure() const { return exposure_; }
    // Scripts: how wet the player is (they dried off, or fell in a river).
    void set_player_wetness(float wetness);
    // What is underfoot of an entity (midgard/footsteps.h): an object's surface, the terrain's
    // ground, or "default".
    std::string ground_under(entt::entity e) const;
    // The last steps taken (at most 64, oldest first): who, on what, and the sound it played.
    struct Step {
        std::string entity, surface, sound;
    };
    const std::vector<Step>& footsteps() const { return steps_; }
    std::size_t footsteps_taken() const { return steps_taken_; }  // (all, since the start)
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
    // ---- system space (milestone 5.16) ----
    // The space round a planet: a room the game makes, not one designed in the editor. The
    // planet is there below, its own ground drawn from far off (the same seas, hills and snow),
    // the sky black with stars from the world's seed, the sun where it is for the planet; the
    // player flies a ship (Player::fly: where the camera looks, up with jump, down with
    // descend; faster the higher it is). Built so Phase 6's star systems can replace it.
    static constexpr const char* kSystemSpace = "@space";
    static constexpr double kShipStart = 300.0;  // metres above the ground it leaves from
    // Into space from the planet the player is on, above where they are (a change of scene,
    // faded): at least kShipStart above the ground, and above the atmosphere's ceiling. From an
    // inside world (a habitat, a hollow world: through a Scene Exit leading to kSystemSpace, its
    // docks and exit sites) the ship is just outside its hull there, the world seen from outside;
    // flying back to the hull goes back in, at the exit's arrival. SceneError (as
    // scene_change_error) on a bounded area or a flat-wrap cylinder: they have no outside.
    void enter_space(float fade = 1.0f);
    // A planet's atmosphere: fly higher than its ceiling (metres above its sea level; the
    // scene's "ceiling" setting, else a fifth of its radius and at least 300 m) and you're in
    // space; fly down through it from space and you land below where you came down (on the
    // ground; or still flying, just under it, if the player flies). 0: not a planet.
    double ceiling() const;
    // Players fly or walk (Player::fly), as a script says (a jetpack, a ship taking off).
    void set_flying(entt::entity player, bool fly);
    bool in_space() const { return scene_.id == kSystemSpace; }
    // The planet's scene the player left for space ("" when not in space).
    const std::string& space_from() const { return space_from_; }
    // How high the player is above the world's ground (sea level, its body's surface): metres.
    double altitude() const;

    // ---- rooms (milestone 5.16) ----
    // Each scene the player has left, as they left it (JSON text): its entities that had moved
    // since it loaded ({"moved": {id: {"position", "rotation", "scale"}}}) and those taken
    // away ({"removed": [id]}); none for one never left. Coming back, it is as they left it;
    // the world save keeps them. (Entities scripts made are not kept yet.)
    std::string room_state(const std::string& scene) const;
    // The player is the same in every scene: the player of the scene the game started in (its
    // components, as designed), carried into each scene it goes to (in the place of that
    // scene's own player, where it stands, or added). Empty until the first change of scene.
    const std::string& carried_player() const { return carried_player_; }
    // Called once the picture is dark, just before the scene is left, with the target's id
    // (the scripts' on_room_exit).
    void on_leaving_scene(std::function<void(const std::string&)> callback) {
        leaving_ = std::move(callback);
    }
    bool changing_scene() const { return change_.has_value(); }
    // Arriving (milestone 5.16): after the swap the picture stays dark, the game going on (the
    // player standing still), while the ground in view where the player arrives is made, and
    // fades in once it is all there (at most kArrivalWait seconds dark, then it fades in
    // anyway). Without a renderer streaming the terrain, the game makes it itself.
    static constexpr float kArrivalWait = 20.0f;
    bool arrival_ready() const;
    bool arriving() const { return change_ && change_->swapped && !arrival_settled_; }
    // How dark the picture is: 0 clear, 1 black (the renderer's exposure scales by 1 - fade).
    float fade() const { return fade_; }
    // Whether the last update swapped the scene (scripts start again from it).
    bool scene_changed() const { return scene_changed_; }
    // Why the last change failed (a missing scene or arrival), or empty: the player stays.
    const std::string& scene_change_error() const { return scene_change_error_; }

private:
    std::vector<std::string> reload_problems_;  // (take_reload_problems)
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
    bool stream_all_ = false;       // a scene was just loaded: its meshes uploaded at once
    int streams_since_load_ = 0;    // terrain streamed since the scene loaded
    bool streamed_ = false;         // the terrain was streamed since the last update
    bool arrival_settled_ = false;  // the arrival is ready (or waited for long enough)
    float arrival_waited_ = 0.0f;   // seconds dark, waiting for it
    std::string scene_change_error_;
    std::string previous_scene_;
    std::map<std::string, std::string> rooms_;  // by scene id
    std::string space_from_, space_return_;     // the scene left for space; its arrival back
    std::optional<double> space_start_;         // the ship's altitude on arriving (else the rule)
    double space_ceiling_ = 0.0;                // the planet's, while in space above it
    struct Hull {                               // an inside world's, seen from space
        Shape::Kind kind = Shape::Kind::SphereInside;
        glm::dvec3 centre{0.0};
        double radius = 0.0, length = 0.0;
    };
    std::optional<Hull> space_hull_;
    std::optional<SurfacePoint> landing_;  // where the player comes down on the planet
    void cross_atmosphere();
    Scene make_space() const;
    void fly(const Actions& actions, float dt);
    std::map<std::string, Transform> loaded_;                 // the scene's entities as they loaded
    std::string carried_player_, carried_id_, carried_name_;  // components (JSON), id, name
    void note_loaded();
    std::string capture_room() const;
    void apply_room(const std::string& room);
    void carry_player();
    std::function<void(const std::string&)> leaving_;
    std::unordered_set<entt::entity> exits_inside_;  // scene exits the player is in
    // Navigation, made when asked for after it went stale.
    mutable std::unique_ptr<NavMesh> nav_;
    mutable bool nav_stale_ = true;
    bool show_navigation_ = false;
    const render::MeshData* outline_mesh(const std::string& asset) const;
    // (Last: made from the terrain and the job system, so it goes before them.)
    std::unique_ptr<NavWorld> nav_world_;
    Characters characters_;
    std::vector<std::string> character_problems_;
    void load_characters();
    // Sound: the player of the game's events, and what each sound component is playing.
    std::unique_ptr<audio::SoundPlayer> sounds_;
    struct Source {
        std::string event;
        float volume = 1.0f;
        audio::VoiceId voice = 0;
    };
    std::unordered_map<entt::entity, Source> sources_;
    // Voices that follow an entity (sound components', and scripts' played from one).
    std::unordered_map<audio::VoiceId, entt::entity> attached_;
    audio::ReverbSettings reverb_;
    std::unique_ptr<audio::MusicDirector> music_;
    std::unique_ptr<audio::AmbienceDirector> ambience_;
    // Weather: the simulation, what it is where the player is, a script's forcing, and the
    // weather sounds playing (those of `weather_sounding_`).
    std::unique_ptr<weather::WeatherSim> weather_;
    weather::Now weather_now_;
    std::vector<WeatherChange> weather_changes_, weather_pending_;
    stats::Stats stats_;
    std::vector<stats::Change> stat_changes_;
    hud::HudState hud_;
    std::vector<std::string> finished_timers_;
    std::vector<hud::UiEvent> ui_events_;
    std::string rebinding_;  // an action waiting for its new key
    GameSettings settings_;
    bool frozen_ = false;
    std::string script_context_;  // the input context a script chose ("": the game's)
    bool menu_music_on_ = false;  // the menu music forced over the scene's
    bool dead_ = false;           // the game over screen showed (until respawn)
    struct Place {
        glm::dvec3 position;
        glm::vec3 rotation;
    };
    std::optional<Place> respawn_at_;      // where the player arrived in this scene
    std::optional<InputMap> input_cache_;  // the package's controls, for key buttons
    audio::Mixer* mixer_ = nullptr;
    void apply_volumes();
    void save_settings();
    void update_screens();  // death; the bound sliders' and keys' values and texts
    void lift_swimmer();    // a swimming player's head above the water (Characters::set_lift)
    bool minimap_wanted_ = false;
    MinimapGround minimap_;
    std::string minimap_scene_;  // (made again in another scene)
    struct MinimapJob {
        glm::dvec3 centre{0.0};
        std::string scene;
        std::future<std::vector<std::uint8_t>> pixels;
    };
    // (resetting it waits for its job: std::async's future does, so do it before the terrain
    // it reads goes; as a member after terrain_, it goes first when the game does)
    std::optional<MinimapJob> minimap_job_;
    void update_minimap(bool now = false);
    struct Forced {
        std::string weather;
        std::optional<std::int64_t> until;  // none: until released
    };
    std::optional<Forced> forced_;
    std::string weather_sounding_;
    std::vector<audio::VoiceId> weather_voices_;
    void rebuild_weather();
    void update_weather();
    void update_weather_look(float dt);
    WeatherLook look_;
    std::uint64_t lightning_rng_ = 0x6c696768746e696eULL;
    double flash_clock_ = 0.0, next_flash_ = -1.0;         // real seconds
    std::vector<double> strikes_;                          // when this flash's strikes came
    std::vector<std::pair<double, std::string>> thunder_;  // when, and the sound event
    std::size_t flashes_ = 0;
    render::Falling falling_;
    void update_falling(float dt);
    render::GroundCover ground_cover_;
    void update_ground_cover(float dt);
    Exposure exposure_;
    double shelter_due_ = 0.0;  // real seconds until the next shelter rays
    void update_exposure(float dt);
    void update_weather_sounds();
    glm::dvec3 player_position() const;
    // Footsteps: how far each walker has come since its last step, and the steps taken.
    struct Walk {
        glm::dvec3 last{0.0};
        double travelled = 0.0;
        bool started = false;
    };
    std::unordered_map<entt::entity, Walk> walks_;
    std::vector<Step> steps_;
    std::size_t steps_taken_ = 0;
    void update_footsteps();
    void step(entt::entity e, const Footsteps& f);
    void listen();
    bool sound_blocked(audio::VoiceId voice, const glm::dvec3& from, const glm::dvec3& to) const;
    void update_reverb();
    std::vector<std::string> sound_problems_;
    void update_sounds(double dt);
    void stop_sources();
    void apply_root_motion();
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
