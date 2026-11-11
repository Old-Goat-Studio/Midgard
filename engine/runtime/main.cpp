// SPDX-License-Identifier: Apache-2.0
// midgard_runtime: the game executable.
//
//   midgard_runtime --version
//   midgard_runtime --check-python      start the game's Python and import the standard library
//   midgard_runtime --dump-api          the midgard script API's names, as JSON
//   midgard_runtime --package <dir> --simulate N [--dump-scene] [--record-audio out.wav]
//                   [--hold KEY:FROM:TO ...]
//                                       N frames of gameplay and scripts, no window (tests);
//                                       its sound mixed offline, and written to a WAV if asked;
//                                       --hold holds a key ("W", "Left Ctrl") from frame FROM up
//                                       to frame TO, as a player would (repeatable)
//   midgard_runtime --package <dir> --headless --editor-link host:port
//                                       play under the editor without a window or GPU (tests, CI)
//   midgard_runtime [--package <dir>] [--frames N] [--size WxH] [--editor-link host:port]
//   midgard_runtime [--package <dir>] --screenshot out.png [--size WxH] [--overlay-text "a|b"]
//                                       offscreen, no window (image tests); --origin-offset x,y,z
//                                       places the demo scene far from the world's origin;
//                                       --exposure brightens it (highlights roll off);
//                                       --scene <name> draws a built-in test scene instead
//                                       (runtime/test_scenes.h lists them); --frames N then
//                                       draws it N more times and says the median frame time
//   --ui-test                           the built-in UI test screen (milestone 6.0) over the
//                                       game, in a window or a screenshot (whose screen checks,
//                                       with --ui-touch also a finger's, fail the run);
//                                       --ui-scale S scales it, --safe-area l,t,r,b (pixels)
//                                       keeps it clear of a pretend notch
//   --screen-test file.json             a screen as the Menu editor makes it (6.0), drawn
//                                       over the game in a screenshot and checked as --ui-test
//                                       checks (with --ui-touch, a finger's checks too)
//   --touch-controls                    the on-screen touch controls (6.0) from the start,
//                                       with the mouse as a finger (to try them without a
//                                       touch screen; a phone shows them by itself)
//   --hud-test                          the HUD (6.0) with made-up stats, notes, a timer, a
//                                       prompt and the crosshair, in a window or a screenshot;
//                                       --show-hud puts a game's own HUD in a screenshot (which
//                                       otherwise shows the world alone, for picture tests)
//   --quality desktop|phone             the renderer's quality tier (by default phone on phones,
//                                       desktop elsewhere), for a window, a screenshot or the
//                                       self-test
//   midgard_runtime --selftest [--show]  checks this device can run games (jobs, identical data,
//                                       rendering and its frame time, the package, Python);
//                                       --show puts the results
//                                       on screen. On Android, an APK with a selftest.txt asset
//                                       starts this way (the self-test APK built in CI).
//   any of these with --profile trace.json   timed zones, for ui.perfetto.dev or chrome://tracing
//   --player-profile <dir>              keep the player's profile (its last camera view) in
//                                       <dir>/profile.json. An exported game in a window keeps
//                                       it in the user's data folder without this; under the
//                                       editor and in tests there is none unless it is given.
//   --world <dir>                       play the world in <dir> (world.json, world.db: its clock
//                                       is restored, and saved when the game ends). An exported
//                                       game in a window keeps worlds/default in the user's data
//                                       folder without this; under the editor and in tests there
//                                       is none unless it is given.
//
// In a window, F3 (or --debug-overlay) shows the frame rate and timings over the game, and F4
// (or --debug-physics, which also works with --screenshot) outlines the physics shapes.
//
// --editor-link connects to the editor's live link (play-in-editor): the editor's edits reach the
// running game, and the game's log and frame rate reach the editor. The game quits when the
// editor stops it or goes away.
//
// With --package, the runtime shows that package's entry scene. Without it, an exported game
// runs the package in the data/ folder next to the executable, and a bare runtime shows a demo.
// On Android the runtime is libmain.so inside the game's APK, and the package is in the APK's
// assets/data/ (copied to internal storage on first start, since SQLite needs real files).
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>  // WinMain for the windowed Windows build
#include <SDL3/SDL_vulkan.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#ifdef __ANDROID__
#include <miniz.h>  // unpacking the game's Python
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

#include "midgard/audio.h"
#include "midgard/game.h"
#include "midgard/game_settings.h"
#include "midgard/input.h"
#include "midgard/link.h"
#include "midgard/manifest.h"
#include "midgard/profile.h"
#include "midgard/render/renderer.h"
#include "midgard/ui/hud.h"
#include "midgard/ui/screens.h"
#include "midgard/ui/touch.h"
#include "midgard/ui/ui.h"
#include "midgard/version.h"
#include "selftest.h"
#include "test_scenes.h"
#ifdef MIDGARD_HAS_SCRIPTING
#include "midgard/script.h"
#include "midgard/script_system.h"
#endif

namespace render = midgard::render;

namespace {

struct Options {
    std::optional<std::string> package_dir;
    std::optional<std::string> screenshot;
    std::optional<std::string> editor_link;
    long simulate = -1;  // frames to run without a window
    bool dump_scene = false;
    std::optional<std::string> record_audio;  // --simulate: the sound mixed, to this WAV
    struct Hold {                             // --simulate: a key held, frames [from, to)
        std::string key;
        long from = 0, to = 0;
    };
    std::vector<Hold> holds;
    bool headless = false;  // the game loop and live link, without a window or GPU
    long frames = -1;       // run until closed
    uint32_t width = 1280, height = 720;
    bool debug_overlay = false;
    bool debug_physics = false;                 // collider outlines (F4)
    std::optional<std::string> overlay_text;    // fixed overlay lines, separated by '|'
    std::optional<std::string> profile;         // where to write the profiling trace
    std::optional<std::string> player_profile;  // the player's profile folder
    std::optional<std::string> world;           // the world's folder
    bool selftest = false;
    bool show = false;              // the self-test's results in a window
    glm::dvec3 origin_offset{0.0};  // where the demo scene is placed (precision tests)
    float exposure = 1.0f;
    std::optional<std::string> scene;        // a built-in test scene
    std::optional<render::Quality> quality;  // the renderer's tier (default: the device's)
    // Overlay lines shown for the whole run (the self-test's results); F3 is off then.
    std::vector<render::OverlayLine> fixed_overlay;
    bool ui_test = false;   // the built-in UI test screen (6.0)
    bool ui_touch = false;  // a screenshot's screen checks include a finger's (target sizes)
    float ui_scale = 1.0f;  // the player's UI scale, on top of the display's density
    std::optional<midgard::ui::Insets> safe_area;  // a pretend safe area, in pixels
    bool hud_test = false;        // the HUD with made-up stats, notes, a timer and a prompt
    bool show_hud = false;        // a screenshot of a game with its HUD (a window always has it)
    bool touch_controls = false;  // the touch controls from the start, the mouse a finger
    std::string screen_test;      // a screen (Menu editor JSON) to draw and check, in a screenshot
};

int usage() {
    std::puts(
        "usage: midgard_runtime [--version] [--package <dir>] [--frames N] [--size WxH]\n"
        "                       [--screenshot out.png [--overlay-text \"a|b\"]]\n"
        "                       [--editor-link host:port] [--debug-overlay] [--debug-physics]\n"
        "                       [--profile out.json] [--player-profile <dir>] [--world <dir>]\n"
        "                       [--quality desktop|phone] [--ui-test [--ui-touch]]\n"
        "                       [--ui-scale S] [--safe-area l,t,r,b] [--hud-test]\n"
        "                       [--show-hud] [--touch-controls] [--screen-test file.json]\n"
        "       midgard_runtime --selftest [--show] [--quality desktop|phone]");
    return 2;
}

std::vector<render::OverlayLine> overlay_lines(const std::string& text) {
    std::vector<render::OverlayLine> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t bar = text.find('|', start);
        lines.push_back(
            {text.substr(start, bar == std::string::npos ? std::string::npos : bar - start)});
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    return lines;
}

// The editor's live link, while the game runs under the editor.
midgard::link::Client* g_link = nullptr;

void send_log(const char* level, std::string text) {
    while (!text.empty() && text.back() == '\n') text.pop_back();
    g_link->send({{"type", "log"}, {"level", level}, {"text", text}});
}

// Messages go to the editor when the game runs under it, otherwise to standard output; on
// Android also to the system log (logcat).
void say_level(const char* level, const char* fmt, va_list args) {
#ifdef __ANDROID__
    va_list copy;
    va_copy(copy, args);
    SDL_LogMessageV(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO, fmt, copy);
    va_end(copy);
#endif
    if (g_link && g_link->connected()) {
        char line[2048];
        std::vsnprintf(line, sizeof(line), fmt, args);
        send_log(level, line);
    } else {
        std::vfprintf(stdout, fmt, args);
        std::fflush(stdout);
    }
}

void say(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    say_level("info", fmt, args);
    va_end(args);
}

void warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    say_level("warn", fmt, args);
    va_end(args);
}

// Errors also appear on screen on Android, where there is no terminal to read them in.
void fail(const std::string& message) {
    std::fprintf(stderr, "error: %s\n", message.c_str());
    if (g_link && g_link->connected()) send_log("error", message);
#ifdef __ANDROID__
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", message.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Midgard", message.c_str(), nullptr);
#endif
}

bool parse_size(std::string_view text, uint32_t& w, uint32_t& h) {
    unsigned a = 0, b = 0;
    if (std::sscanf(std::string(text).c_str(), "%ux%u", &a, &b) != 2 || !a || !b) return false;
    w = a;
    h = b;
    return true;
}

// What the runtime draws: a package's scene, or the built-in demo.
class Source {
public:
    virtual ~Source() = default;
    virtual void upload(render::Renderer& renderer) = 0;
    // Streams what changes with where the camera is (terrain), before draws(); wait: all of it
    // now (the first frame, screenshots).
    virtual void stream(render::Renderer&, const glm::dvec3&, bool) {}
    // What to draw, relative to the camera's origin (camera-relative rendering).
    virtual std::vector<render::DrawItem> draws(float seconds, const glm::dvec3& origin) const = 0;
    virtual render::Camera camera(float aspect) const = 0;
    // The lighting, with local lights relative to the camera's origin.
    virtual render::Light light(const glm::dvec3& origin) const = 0;
    virtual glm::vec4 sky() const = 0;
    // The sky and fog (by default: a flat sky in sky()'s colour, no fog).
    virtual render::Environment environment(const glm::dvec3&) const { return {}; }
    virtual render::TerrainLook terrain_look() const { return {}; }
    virtual midgard::InputMap input_map() const { return midgard::InputMap::defaults(); }
    virtual void update(const midgard::Actions&, float) {}
    // The mixer to play through (milestone 5.14; none: silent). Set back to none before it goes.
    virtual void set_audio(midgard::audio::Mixer*) {}
    // Background work a frame would have had time for (navigation tiles being made): done
    // now. Headless simulations run frames far faster than real time.
    virtual void settle() {}
    virtual bool wants_mouse_look() const { return false; }
    virtual std::string title() const { return "Midgard"; }
    // The game's icon (icon.png in its package), for the window and the taskbar.
    virtual std::optional<std::filesystem::path> icon() const { return std::nullopt; }
    // Collider outlines for debug drawing (F4), relative to the camera's origin.
    virtual std::vector<render::DebugLine> debug_lines(const glm::dvec3&) const { return {}; }
    // The scene as JSON (entities and their transforms and colours), for tests.
    virtual std::string dump() const { return "{}"; }
    // Lines about the game for the debug overlay (F3): the weather and how it treats the player.
    virtual std::vector<std::string> status() const { return {}; }
    // What the HUD shows (6.0); none: no HUD (the demo and test scenes).
    virtual std::optional<midgard::ui::HudData> hud() const { return std::nullopt; }
    // The player is flying (the touch controls show their Down button).
    virtual bool flying() const { return false; }
    // The input context the actions are in this frame (midgard/input.h).
    virtual std::string input_context() const { return "on_foot"; }
    // The touch controls' buttons (the Input editor's Touch Layout).
    virtual midgard::TouchLayout touch_layout() const { return midgard::default_touch_layout(); }
    // The Menu editor's screens (6.0), and the game's state of them: which are shown, and the
    // text and values scripts set. None without a game.
    virtual const std::vector<midgard::ScreenDef>* screen_defs() const { return nullptr; }
    virtual const midgard::hud::HudState* hud_state() const { return nullptr; }
    // The player clicked a button (true: it quits the game), or changed a slider or check.
    virtual bool ui_click(const std::string& screen, const std::string& element) {
        (void)screen;
        (void)element;
        return false;
    }
    virtual void ui_change(const std::string& screen, const std::string& element, double value) {
        (void)screen;
        (void)element;
        (void)value;
    }
    // The game's own screens (6.0 step 7): the menu action (true: a screen took it), the
    // action waiting for a new key, that key ("" to give up; returns the action), and the
    // player's keys.
    virtual bool menu() { return false; }
    virtual bool has_pause_menu() const { return false; }
    // The game's settings (the Project Page's), and freezing the world under a pausing screen.
    virtual const midgard::GameSettings* game_settings() const { return nullptr; }
    virtual void set_frozen(bool frozen) { (void)frozen; }
    virtual std::string rebinding() const { return {}; }
    virtual std::string key_chosen(const std::string& key) {
        (void)key;
        return {};
    }
    virtual std::map<std::string, std::string> player_keys() const { return {}; }
    // An Asset Library font's files (by its "ui:" folder): names and bytes.
    virtual std::vector<std::pair<std::string, std::vector<uint8_t>>> ui_fonts(
        const std::string& folder) const {
        (void)folder;
        return {};
    }
    // A screen's picture: its file's bytes by "ui:" content id (empty: none).
    virtual std::vector<uint8_t> ui_image(const std::string& content) const {
        (void)content;
        return {};
    }
    // A HUD is shown from now on (the game makes what only it needs: the minimap's ground).
    virtual void hud_shown() {}
    // How dark the picture is (a scene change fading out and in): 0 clear, 1 black.
    virtual float fade() const { return 0.0f; }
    // The player's profile file: used from now on, and saved when the game ends.
    virtual void use_profile(const std::filesystem::path&) {}
    // The world's folder: its clock is restored now and saved when the game ends.
    virtual void use_world(const std::filesystem::path&) {}
    // A message from the editor's live link (entity edits and the like).
    virtual void apply(const nlohmann::json& message, render::Renderer*) {
        warn("live link: '%s' needs a game package\n", message.value("type", "").c_str());
    }
};

class DemoSource : public Source {
public:
    // The whole scene can be placed far from the world's origin (--origin-offset): with
    // camera-relative rendering it must look exactly the same there.
    explicit DemoSource(glm::dvec3 offset = glm::dvec3(0.0)) : offset_(offset) {}
    void upload(render::Renderer& renderer) override {
        cube_ = renderer.upload_mesh(render::make_cube());
    }
    std::vector<render::DrawItem> draws(float seconds, const glm::dvec3& origin) const override {
        const glm::dmat4 spin =
            glm::rotate(glm::dmat4(1.0), double(seconds * 0.6f), glm::dvec3(0, 1, 0));
        const glm::dmat4 cube =
            glm::translate(glm::dmat4(1.0), offset_ + glm::dvec3(0, 0.5, 0)) * spin;
        const glm::dmat4 ground =
            glm::scale(glm::translate(glm::dmat4(1.0), offset_ + glm::dvec3(0, -0.05, 0)),
                       glm::dvec3(6.0, 0.1, 6.0));
        return {
            {cube_, render::relative_to(cube, origin), glm::vec4(0.79f, 0.64f, 0.15f, 1.0f)},
            {cube_, render::relative_to(ground, origin), glm::vec4(0.22f, 0.32f, 0.18f, 1.0f)},
        };  // knotwork gold, moss
    }
    render::Camera camera(float aspect) const override {
        render::Camera c;
        c.origin = offset_ + glm::dvec3(2.6, 2.0, 3.4);
        const glm::vec3 to_target(offset_ + glm::dvec3(0, 0.4, 0) - c.origin);
        c.view = glm::lookAt(glm::vec3(0.0f), to_target, glm::vec3(0, 1, 0));
        c.projection = render::perspective(glm::radians(55.0f), aspect, 0.05f, 200.0f);
        return c;
    }
    render::Light light(const glm::dvec3&) const override { return {}; }
    glm::vec4 sky() const override { return {0.08f, 0.11f, 0.18f, 1.0f}; }  // night blue

private:
    glm::dvec3 offset_;
    render::MeshId cube_ = 0;
};

// A built-in test scene (--scene), for image tests of the renderer.
class TestSceneSource : public Source {
public:
    explicit TestSceneSource(std::unique_ptr<midgard::test_scenes::TestScene> scene)
        : scene_(std::move(scene)) {}
    void upload(render::Renderer& renderer) override { scene_->upload(renderer); }
    void stream(render::Renderer& renderer, const glm::dvec3& camera, bool) override {
        scene_->stream(renderer, camera);
    }
    std::vector<render::DrawItem> draws(float, const glm::dvec3& origin) const override {
        return scene_->draws(origin);
    }
    render::Camera camera(float aspect) const override { return scene_->camera(aspect); }
    render::Light light(const glm::dvec3& origin) const override { return scene_->light(origin); }
    glm::vec4 sky() const override { return scene_->sky(); }
    render::Environment environment(const glm::dvec3&) const override {
        return scene_->environment();
    }
    render::TerrainLook terrain_look() const override { return scene_->terrain_look(); }
    std::vector<render::DebugLine> debug_lines(const glm::dvec3& origin) const override {
        return scene_->debug_lines(origin);
    }

private:
    std::unique_ptr<midgard::test_scenes::TestScene> scene_;
};

#if defined(__ANDROID__) && defined(MIDGARD_HAS_SCRIPTING)
std::filesystem::path android_python_home();  // below, with the other Android file handling
#endif

class GameSource : public Source {
public:
    explicit GameSource(const std::string& dir) : game_(dir) {
        const midgard::Manifest& m = game_.package().manifest;
        say("%s %s: scene '%s', %zu entities, built by editor %s\n", m.game_name.c_str(),
            m.game_version.c_str(), m.entry_scene.c_str(),
            game_.scene().registry.view<midgard::EntityInfo>().size(), m.editor_build.c_str());
        game_.set_show_navigation(true);  // (with the collider outlines: F4)
    }
    ~GameSource() override {
        try {
            game_.save_profile();
        } catch (const midgard::ProfileError& e) {
            warn("%s\n", e.what());
        }
        try {
            game_.save_world();
        } catch (const midgard::WorldError& e) {
            warn("%s\n", e.what());
        }
    }
    void use_world(const std::filesystem::path& folder) override {
        try {
            game_.use_world(folder);
            say("World: %s\n", folder.string().c_str());
        } catch (const midgard::WorldError& e) {
            warn("%s; playing without saving\n", e.what());
        }
    }
    void use_profile(const std::filesystem::path& file) override {
        try {
            game_.use_profile(file);
            say("Player profile: %s\n", file.string().c_str());
        } catch (const midgard::ProfileError& e) {
            warn("%s; starting a new one\n", e.what());
        }
    }
    void upload(render::Renderer& renderer) override {
        game_.upload_meshes(renderer);
        // The ground's picture textures (6.0b), from the package.
        renderer.set_ground_reader(
            [this](const std::string& id) { return game_.read_content(id); });
    }
    void stream(render::Renderer& renderer, const glm::dvec3& camera, bool wait) override {
        game_.stream_terrain(renderer, camera, wait);
    }
    std::vector<render::DrawItem> draws(float, const glm::dvec3& origin) const override {
        return game_.draws(origin);
    }
    render::Camera camera(float aspect) const override { return game_.camera(aspect); }
    render::Environment environment(const glm::dvec3& origin) const override {
        return game_.environment(origin);  // (with the weather)
    }
    render::TerrainLook terrain_look() const override { return game_.terrain_look(); }
    float fade() const override { return game_.fade(); }
    void settle() override { game_.stream_navigation(true); }
    render::Light light(const glm::dvec3& origin) const override { return game_.light(origin); }
    glm::vec4 sky() const override { return {game_.scene().settings.sky, 1.0f}; }
    std::vector<render::DebugLine> debug_lines(const glm::dvec3& origin) const override {
        return game_.debug_lines(origin);
    }
    midgard::InputMap input_map() const override { return game_.input_map(); }
    void update(const midgard::Actions& actions, float dt) override {
        game_.update(actions, dt);
        if (!scripts_tried_ && scripts_needed()) start_scripts();
#ifdef MIDGARD_HAS_SCRIPTING
        if (scripts_) scripts_->update(actions, dt);
#endif
        for (const std::string& p : game_.take_sound_problems()) warn("%s\n", p.c_str());
    }
    void set_audio(midgard::audio::Mixer* mixer) override { game_.set_audio(mixer); }

    std::string dump() const override {
        nlohmann::json entities = nlohmann::json::array();
        const auto& reg = game_.scene().registry;
        for (entt::entity e : reg.view<const midgard::EntityInfo>()) {
            const auto& t = reg.get<midgard::Transform>(e);
            const midgard::Material* m = reg.try_get<midgard::Material>(e);
            // A character: what it plays, and where some of its bones are.
            nlohmann::json character;
            if (reg.all_of<midgard::CharacterRef>(e)) {
                const midgard::Characters& c = game_.characters();
                nlohmann::json bones = nlohmann::json::object();
                for (const midgard::HumanBone b :
                     {midgard::HumanBone::Hips, midgard::HumanBone::Head,
                      midgard::HumanBone::LeftFoot, midgard::HumanBone::RightFoot,
                      midgard::HumanBone::LeftHand, midgard::HumanBone::RightHand})
                    if (const auto p = c.bone_position(game_.scene(), e, b))
                        bones[midgard::human_bone_name(b)] = {p->x, p->y, p->z};
                const std::string state = c.state_of(e);
                character = {{"state", state.empty() ? c.clip_of(e) : state}, {"bones", bones}};
            }
            entities.push_back(
                {{"id", reg.get<midgard::EntityInfo>(e).id},
                 {"character", character},
                 {"name", reg.get<midgard::EntityInfo>(e).name},
                 {"position", {t.position.x, t.position.y, t.position.z}},
                 {"rotation", {t.rotation.x, t.rotation.y, t.rotation.z}},
                 {"scale", {t.scale.x, t.scale.y, t.scale.z}},
                 {"color", m ? nlohmann::json{m->color.r, m->color.g, m->color.b, m->color.a}
                             : nlohmann::json(nullptr)}});
        }
        const glm::vec3 sky = game_.scene().settings.sky;
        // The sun as the clock has it: where the light comes from, how high it is (-1 to 1,
        // along "up" at the camera) and how bright.
        const glm::dvec3 at = game_.camera(1.0f).origin;
        const midgard::render::Light light = game_.scene().light(at);
        const glm::vec3 to_sun = -glm::normalize(light.direction);
        const nlohmann::json sun{{"direction", {to_sun.x, to_sun.y, to_sun.z}},
                                 {"height", glm::dot(to_sun, glm::normalize(light.up))},
                                 {"intensity", light.intensity},
                                 {"color", {light.color.x, light.color.y, light.color.z}},
                                 {"ambient", {light.ambient.x, light.ambient.y, light.ambient.z}}};
        const auto phase = game_.scene().solar_phase;
        // Sound (milestone 5.14): what's playing, by event.
        nlohmann::json audio = nullptr;
        if (const auto* sounds = game_.sounds()) {
            nlohmann::json voices = nlohmann::json::array();
            for (const auto& v : sounds->voices())
                voices.push_back({{"event", v.event},
                                  {"spatial", v.spatial},
                                  {"gain", v.gain},
                                  {"occlusion", v.occlusion}});
            const glm::dvec3 ear = game_.camera(1.0f).origin;  // (the listener)
            audio = {{"playing", sounds->playing_events()},
                     {"voices", voices},
                     {"reverb", game_.reverb().wet},
                     {"listener", {ear.x, ear.y, ear.z}}};
            nlohmann::json steps = nlohmann::json::array();
            for (const auto& s : game_.footsteps())
                steps.push_back({{"entity", s.entity}, {"surface", s.surface}, {"sound", s.sound}});
            audio["footsteps"] = steps;
            if (const auto* ambience = game_.ambience())
                audio["ambience"] = {{"set", ambience->playing()},
                                     {"layers", ambience->layers()},
                                     {"sprinkled", ambience->sprinkled()}};
            if (const auto* music = game_.music()) {
                const auto c = game_.music_context();
                audio["music"] = {{"playlist", music->playing()},
                                  {"track", music->track()},
                                  {"duck", music->duck_level()},
                                  {"context",
                                   {{"scene", c.scene},
                                    {"world_type", c.world_type},
                                    {"biome", c.biome},
                                    {"time", c.time}}}};
            }
        }
        // The world as generated (midgard/world_type.h): the editor's preview must match it.
        nlohmann::json world_hash;
        const midgard::SceneSettings& settings = game_.scene().settings;
        if (settings.body && settings.terrain) {
            char hex[17];
            std::snprintf(hex, sizeof hex, "%016llx",
                          static_cast<unsigned long long>(midgard::world_hash(
                              midgard::Terrain(*settings.body, *settings.terrain))));
            world_hash = hex;
        }
        // The weather (milestone 5.15): where the player is, and the next day's forecast there.
        // (Forecasting fills the weather's caches: no change to what the game shows.)
        auto& game = const_cast<midgard::Game&>(game_);
        nlohmann::json weather = nullptr;
        if (game.weather() || game.weather_now().forced) {
            const auto& now = game.weather_now();
            nlohmann::json forecast = nlohmann::json::array();
            for (const auto& s : game.weather_forecast(24.0))
                forecast.push_back({{"weather", s.weather},
                                    {"start", s.start},
                                    {"end", s.end},
                                    {"front", s.front},
                                    {"forced", s.forced}});
            nlohmann::json cell = nullptr;
            if (const auto c = game.weather_cell()) cell = {c->face, c->i, c->j};
            nlohmann::json sounding = nlohmann::json::array();
            if (const auto* type = game.weather_type())
                for (const auto& s : type->sounds) sounding.push_back(s.event);
            const auto& look = game.weather_look();
            weather = {
                {"now", now.weather},
                {"look",
                 {{"clouds", look.clouds},
                  {"darkness", look.darkness},
                  {"fog", look.fog},
                  {"flash", look.flash}}},
                {"flashes", game.lightning_flashes()},
                {"ground",
                 {{"wetness", game.ground_cover().wetness}, {"snow", game.ground_cover().snow}}},
                {"temperature", game.temperature_here()},
                {"exposure",
                 {{"feels_like", game.exposure().feels_like},
                  {"wind", game.exposure().wind},
                  {"heat", game.exposure().heat},
                  {"wetness", game.exposure().wetness},
                  {"roof", game.exposure().roof},
                  {"walls", game.exposure().walls},
                  {"sheltered", game.exposure().sheltered}}},
                {"falling",
                 {{"kind", int(game.falling().kind)},
                  {"amount", game.falling().amount},
                  {"velocity",
                   {game.falling().velocity.x, game.falling().velocity.y,
                    game.falling().velocity.z}}}},
                {"front", now.front},
                {"forced", now.forced},
                {"cell", cell},
                {"sounds", sounding},
                {"forecast", forecast},
                {"changes", game.weather_changes().size()}};
        }
        return nlohmann::json{{"entities", entities},
                              {"scene", game_.scene().id},
                              {"world_hash", world_hash},
                              {"camera", view_name(game_.camera_rig().view())},
                              {"sky", {sky.x, sky.y, sky.z}},
                              {"time", game_.time().now()},
                              {"solar_phase", phase ? nlohmann::json(*phase) : nlohmann::json()},
                              {"sun", sun},
                              {"season", game_.scene().sun_clock.season},
                              {"audio", audio},
                              {"weather", weather},
                              {"stats", game_.stats().save()},
                              {"hud", game_.hud().dump()},
                              // Rooms and space (milestone 5.16): how high, and where space is.
                              {"rooms",
                               {{"previous", game_.previous_scene()},
                                {"space_from", game_.space_from()},
                                {"altitude", game_.altitude()},
                                {"ceiling", game_.ceiling()}}}}
            .dump();
    }
    std::vector<std::string> status() const override {
        // The player's stats (until the HUD shows them): "Health 87/100  Score 40".
        std::vector<std::string> lines;
        std::string line;
        for (const midgard::stats::StatDef& d : game_.stats().defs()) {
            const double v = game_.stats().get(d.id).value_or(0.0);
            char part[160];
            if (d.display())
                std::snprintf(part, sizeof(part), "%s %s", d.name.c_str(),
                              game_.stat_text(d).c_str());
            else if (d.capped())
                std::snprintf(part, sizeof(part), "%s %.0f/%.0f", d.name.c_str(), v, d.max);
            else
                std::snprintf(part, sizeof(part), "%s %.0f", d.name.c_str(), v);
            if (line.size() + std::strlen(part) > 70) {
                lines.push_back(line);
                line.clear();
            }
            line += (line.empty() ? "" : "  ") + std::string(part);
        }
        if (!line.empty()) lines.push_back(line);
        const auto* type = game_.weather_type();
        if (!type && !game_.weather_cell()) return lines;
        const auto& now = game_.weather_now();
        const auto& e = game_.exposure();
        const auto& g = game_.ground_cover();
        char a[160], b[160], c[160];
        std::snprintf(a, sizeof(a), "Weather: %s%s%s", type ? type->name.c_str() : "none",
                      now.forced ? " (forced)" : "", now.front >= 0 ? " (a front)" : "");
        std::snprintf(b, sizeof(b), "Air %.1f C, feels like %.1f C, wind %.1f m/s, heat +%.1f C",
                      e.air, e.feels_like, e.wind, e.heat);
        std::snprintf(c, sizeof(c),
                      "%s (roof %.0f%%, walls %.0f%%), wet %.0f%%; ground wet %.0f%%, snow %.0f%%",
                      e.sheltered ? "Sheltered" : "In the open", e.roof * 100.0, e.walls * 100.0,
                      e.wetness * 100.0, g.wetness * 100.0, g.snow * 100.0);
        lines.insert(lines.end(), {a, b, c});
        return lines;
    }
    std::optional<midgard::ui::HudData> hud() const override {
        midgard::ui::HudData d;
        const midgard::hud::HudState& h = game_.hud();
        d.visible = h.visible;
        // The crosshair as a script says, else in first person only.
        // (as the Project Page says: in first person, in every view, or never)
        const midgard::GameSettings& gs = game_.game_settings();
        const bool aiming = game_.camera_rig().active() &&
                            (gs.crosshair_views == "all" ||
                             (gs.crosshair_views == "first" &&
                              game_.camera_rig().view() == midgard::CameraView::First));
        d.crosshair = h.crosshair.value_or(aiming);
        d.crosshair_picture = !gs.crosshair.empty();
        d.crosshair_size = float(gs.crosshair_size);
        for (const midgard::stats::StatDef& s : game_.stats().defs()) {
            if (!s.hud) continue;
            std::string text = game_.stat_text(s);
            if (s.display() && text.empty()) continue;  // (a clock that shows nothing)
            d.stats.push_back({s.id, s.name, s.kind == midgard::stats::Kind::stat,
                               game_.stats().get(s.id).value_or(0.0), s.capped() ? s.max : 0.0,
                               std::move(text)});
        }
        for (const midgard::hud::Timer& t : h.timers())
            d.timers.push_back({t.id, t.label, t.value});
        for (const midgard::hud::Note& n : h.notes()) d.notes.push_back({n.text, n.opacity()});
        d.prompt = h.prompt();
        // The compass and minimap (step 4), true on the world's shape.
        const double heading = game_.heading();
        if (h.compass) d.heading = heading;
        d.map.shown = h.minimap;
        if (h.minimap) {
            const auto& ground = game_.minimap();
            d.map.radius = midgard::Game::kMinimapRadius;
            if (!ground.pixels.empty()) {
                d.map.ground = &ground.pixels;
                d.map.size = ground.size;
                d.map.ground_radius = ground.radius;
                d.map.version = ground.version;
                const glm::dvec2 off = game_.minimap_offset();
                d.map.east = off.x;
                d.map.north = off.y;
            }
            for (const auto& m : game_.map_marks(midgard::Game::kMinimapRadius * 1.5))
                d.map.marks.push_back({m.kind, m.label, m.east, m.north});
        }
        return d;
    }
    void hud_shown() override { game_.want_minimap(); }
    bool wants_mouse_look() const override { return game_.camera_rig().active(); }
    const std::vector<midgard::ScreenDef>* screen_defs() const override { return &game_.screens(); }
    const midgard::hud::HudState* hud_state() const override { return &game_.hud(); }
    bool ui_click(const std::string& screen, const std::string& element) override {
        return game_.ui_click(screen, element);
    }
    void ui_change(const std::string& screen, const std::string& element, double value) override {
        game_.ui_change(screen, element, value);
    }
    bool menu() override { return game_.menu(); }
    bool has_pause_menu() const override { return game_.screen_for("pause") != nullptr; }
    const midgard::GameSettings* game_settings() const override { return &game_.game_settings(); }
    void set_frozen(bool frozen) override { game_.set_frozen(frozen); }
    std::string rebinding() const override { return game_.rebinding(); }
    std::string key_chosen(const std::string& key) override { return game_.key_chosen(key); }
    std::map<std::string, std::string> player_keys() const override { return game_.player_keys(); }
    std::vector<std::pair<std::string, std::vector<uint8_t>>> ui_fonts(
        const std::string& folder) const override {
        auto found = midgard::package_fonts(
            game_.package().manifest.content, folder,
            [this](const std::string& id) { return game_.read_content(id); });
        const auto live = ui_font_folders_.find(folder);
        if (found.empty() && live != ui_font_folders_.end()) {
            std::error_code ec;
            for (const auto& f : std::filesystem::recursive_directory_iterator(
                     std::filesystem::u8path(live->second), ec)) {
                std::string ext = f.path().extension().string();
                for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
                if (ext != ".ttf" && ext != ".otf") continue;
                std::ifstream in(f.path(), std::ios::binary);
                found.emplace_back(f.path().filename().string(),
                                   std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                                        std::istreambuf_iterator<char>()));
            }
        }
        return found;
    }
    std::vector<uint8_t> ui_image(const std::string& content) const override {
        // From the package; under the editor, a picture chosen since the game was cooked comes
        // from the project's file (the live link's screens.put says where).
        try {
            return game_.read_content(content);
        } catch (const std::exception&) {
        }
        const auto it = ui_image_files_.find(content);
        if (it == ui_image_files_.end()) return {};
        std::ifstream in(std::filesystem::u8path(it->second), std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    }
    std::string input_context() const override { return game_.input_context(); }
    midgard::TouchLayout touch_layout() const override { return game_.game_settings().touch; }
    bool flying() const override {
        const entt::entity p = game_.camera_rig().player();
        const auto* player =
            p == entt::null ? nullptr : game_.scene().registry.try_get<midgard::Player>(p);
        return player && player->fly;
    }
    std::string title() const override { return game_.package().manifest.game_name; }
    std::optional<std::filesystem::path> icon() const override {
        const auto file = game_.package().root / "icon.png";
        if (std::filesystem::is_regular_file(file)) return file;
        return std::nullopt;
    }

    void apply(const nlohmann::json& message, render::Renderer* renderer) override {
        const std::string type = message.value("type", "");
        // Edits to a scene the game isn't in (the player went through a scene exit) wait for
        // the next Play.
        if (message.contains("scene") && message["scene"].is_string() &&
            message["scene"].get<std::string>() != game_.scene().id && type != "scene.dump") {
            acknowledge("skipped " + type + ": the game is in scene " + game_.scene().id);
            return;
        }
        if (type == "scene.dump") {  // what the game has, for the editor's tests and tools
            if (g_link) g_link->send({{"type", "scene"}, {"scene", nlohmann::json::parse(dump())}});
            return;
        }
        if (type == "environment.put") {  // the Environment editor
            const nlohmann::json& e = message.at("environment");
            const std::string id = e.value("id", "");
            try {
                game_.scene().environments[id] = midgard::environment_from_json(
                    id, e.value("name", id), e.value("curves", nlohmann::json::object()).dump(),
                    e.value("settings", nlohmann::json::object()).dump());
            } catch (const std::exception& ex) {
                warn("live link: environment %s: %s\n", id.c_str(), ex.what());
                return;
            }
            acknowledge("applied environment.put " + id);
            return;
        }
        if (type == "terrain.stamps") {  // sculpting and painting in the editor
            std::vector<midgard::TerrainStamp> stamps;
            try {
                for (const nlohmann::json& s : message.at("stamps"))
                    stamps.push_back(midgard::stamp_from_json(s));
            } catch (const std::exception& ex) {
                warn("live link: terrain stamps: %s\n", ex.what());
                return;
            }
            const size_t count = stamps.size();
            game_.set_terrain_stamps(std::move(stamps));
            acknowledge("applied terrain.stamps " + std::to_string(count));
            return;
        }
        if (type == "audio.put") {  // the Audio panel
            try {
                game_.set_audio_data(message.at("audio"));
            } catch (const std::exception& ex) {
                warn("live link: %s\n", ex.what());
                return;
            }
            acknowledge("applied audio.put");
            return;
        }
        if (type == "stats.put") {  // the Stats panel
            try {
                game_.set_stat_data(message.at("stats"));
            } catch (const std::exception& ex) {
                warn("live link: %s\n", ex.what());
                return;
            }
            acknowledge("applied stats.put");
            return;
        }
        if (type == "settings.put") {  // the Project Page's game settings
            for (const auto& [content, file] :
                 message.value("images", nlohmann::json::object()).items())
                if (file.is_string()) ui_image_files_[content] = file.get<std::string>();
            game_.set_game_settings(message.value("settings", nlohmann::json::object()));
            acknowledge("applied settings.put");
            return;
        }
        if (type == "screens.put") {  // the Menu editor
            try {
                for (const auto& [content, file] :
                     message.value("images", nlohmann::json::object()).items())
                    if (file.is_string()) ui_image_files_[content] = file.get<std::string>();
                for (const auto& [font, folder] :
                     message.value("fonts", nlohmann::json::object()).items())
                    if (folder.is_string()) ui_font_folders_[font] = folder.get<std::string>();
                game_.set_screen_data(message.at("screens"));
            } catch (const std::exception& ex) {
                warn("live link: %s\n", ex.what());
                return;
            }
            acknowledge("applied screens.put");
            return;
        }
        if (type == "screen.show" || type == "screen.hide") {  // the Menu editor's Show in Game
            const std::string id = message.value("screen", "");
            if (type == "screen.show" && game_.screen(id))
                game_.hud().show_screen(id);
            else
                game_.hud().hide_screen(id);
            acknowledge("applied " + type);
            return;
        }
        if (type == "weather.put") {  // the Calendar dock's Weather tab
            try {
                game_.set_weather_data(message.at("weather"));
            } catch (const std::exception& ex) {
                warn("live link: %s\n", ex.what());
                return;
            }
            acknowledge("applied weather.put");
            return;
        }
        if (type == "time.set") {  // the Calendar dock's scrubber
            game_.set_time(message.value("time", std::int64_t(0)));
            return;
        }
        if (type == "script.changed") {
            const std::string module = message.value("module", "");
#ifdef MIDGARD_HAS_SCRIPTING
            if (scripts_) scripts_->reload(module);
#endif
            acknowledge("applied script.changed " + module);
            return;
        }
        try {
            if (type == "entity.put") {
                game_.put_entity(message.at("entity"));
            } else if (type == "entity.set") {
                game_.set_component(message.at("entity").get<std::string>(),
                                    message.at("component").get<std::string>(),
                                    message.at("value"));
            } else if (type == "entity.delete") {
                game_.remove_entity(message.at("entity").get<std::string>());
            } else if (type == "row.changed") {
                const std::string table = message.at("table").get<std::string>();
                const std::string key = message.at("key").get<std::string>();
                if (table == "scene" && key == game_.scene().id)
                    game_.reload_scene_settings();
                else if (table == "entity")
                    game_.reload_entity(key);
                acknowledge("applied row.changed " + table + " " + key);
                return;
            } else if (type == "object.put") {
                const std::string id = message.at("object").get<std::string>();
                game_.set_object(id, message.at("parts").dump());
                if (renderer)
                    for (const std::string& error : game_.upload_new_meshes(*renderer))
                        warn("%s\n", error.c_str());
                acknowledge("applied object.put " + id);
                return;
            } else if (type == "asset.changed") {
                const std::string id = message.at("id").get<std::string>();
                if (renderer) game_.reload_asset(id, *renderer);
                for (const std::string& problem : game_.take_reload_problems())
                    warn("%s\n", problem.c_str());
                acknowledge("applied asset.changed " + id);
                return;
            } else {
                warn("live link: '%s' is not supported yet\n", type.c_str());
                return;
            }
        } catch (const std::exception& e) {  // bad data from the editor must not end the game
            warn("live link: %s: %s\n", type.c_str(), e.what());
            return;
        }
        if (renderer)
            for (const std::string& error : game_.upload_new_meshes(*renderer))
                warn("%s\n", error.c_str());
        const nlohmann::json& entity = message.at("entity");
        acknowledge("applied " + type + " " +
                    (entity.is_object() ? entity.value("id", "") : entity.get<std::string>()));
    }

private:
    bool scripts_needed() const {
        for (entt::entity e : game_.scene().registry.view<const midgard::ScriptRef>()) {
            (void)e;
            return true;
        }
        return false;
    }

    // Scripts start when the scene first has one (perhaps added from the editor while playing).
    void start_scripts() {
        scripts_tried_ = true;
#ifdef MIDGARD_HAS_SCRIPTING
        try {
#ifdef __ANDROID__
            const std::filesystem::path home = android_python_home();
#else
            const char* base = SDL_GetBasePath();
            const std::filesystem::path home =
                midgard::script::python_home_beside(base ? base : ".");
#endif
            python_ = std::make_unique<midgard::script::Interpreter>(
                home, std::vector<std::filesystem::path>{game_.package().root / "scripts"});
            scripts_ = std::make_unique<midgard::script::ScriptSystem>(game_, report_script);
        } catch (const std::exception& e) {
            scripts_.reset();
            python_.reset();
            warn("Scripts can't run: %s\n", e.what());
        }
#else
        warn("This runtime has no scripting yet, so the game's scripts don't run.\n");
#endif
    }

#ifdef MIDGARD_HAS_SCRIPTING
    static void report_script(const midgard::script::Report& r) {
        if (r.level == "info") {
            say("%s\n", r.text.c_str());
        } else if (r.level == "warn") {
            warn("%s\n", r.text.c_str());
        } else if (g_link && g_link->connected()) {
            send_log("error", r.text);
            g_link->send({{"type", "script.error"},
                          {"module", r.module},
                          {"file", r.file},
                          {"line", r.line},
                          { "message",
                            r.text }});
        } else {
            std::fprintf(stderr, "%s\n", r.text.c_str());
        }
    }
#endif

    // Lets the editor (and its tests) see when an edit took effect.
    static void acknowledge(const std::string& text) {
        if (g_link) g_link->send({{"type", "log"}, {"level", "debug"}, {"text", text}});
    }

public:
private:
    midgard::Game game_;
    // Pictures chosen in the Menu editor since the game was cooked: their files.
    std::map<std::string, std::string> ui_image_files_;
    std::map<std::string, std::string> ui_font_folders_;  // (and fonts: their folders)
    bool scripts_tried_ = false;
#ifdef MIDGARD_HAS_SCRIPTING
    std::unique_ptr<midgard::script::Interpreter> python_;  // before scripts_: outlives it
    std::unique_ptr<midgard::script::ScriptSystem> scripts_;
#endif
};

constexpr float kStickDeadZone = 0.15f;

// SDL events -> control names (schema/migrations/0003_input.sql). Keys use scancodes, so WASD
// stays in place on any keyboard layout.
void feed(midgard::Actions& actions, const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (!e.key.repeat)
                actions.set("keyboard", SDL_GetScancodeName(e.key.scancode),
                            e.type == SDL_EVENT_KEY_DOWN ? 1.0f : 0.0f);
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (e.motion.which == SDL_TOUCH_MOUSEID) break;  // (a finger: the touch controls')
            actions.add_delta("mouse", "delta_x", e.motion.xrel);
            actions.add_delta("mouse", "delta_y", e.motion.yrel);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            actions.add_delta("mouse", "wheel", e.wheel.y);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            // (6.1) Mouse buttons: "left", "right", "middle", "x1", "x2"; only while the game
            // has the mouse, so the click that takes it doesn't also fire an action.
            if (e.button.which == SDL_TOUCH_MOUSEID) break;
            SDL_Window* focus = SDL_GetMouseFocus();
            const bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            if (down && !(focus && SDL_GetWindowRelativeMouseMode(focus))) break;
            static const char* const kButtons[] = {"", "left", "middle", "right", "x1", "x2"};
            if (e.button.button >= 1 && e.button.button <= 5)
                actions.set("mouse", kButtons[e.button.button], down ? 1.0f : 0.0f);
            break;
        }
        case SDL_EVENT_GAMEPAD_ADDED:
            SDL_OpenGamepad(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
            float v = float(e.gaxis.value) / 32767.0f;
            if (v > -kStickDeadZone && v < kStickDeadZone) v = 0.0f;
            if (const char* name = SDL_GetGamepadStringForAxis(SDL_GamepadAxis(e.gaxis.axis)))
                actions.set("gamepad", name, v);
            break;
        }
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            if (const char* name =
                    SDL_GetGamepadStringForButton(SDL_GamepadButton(e.gbutton.button)))
                actions.set("gamepad", name, e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? 1.0f : 0.0f);
            break;
        default:
            break;
    }
}

// The Windows runtime is a windowed program. When it is started from a terminal it has no
// standard output, so attach to the terminal; pipes (the editor, scripts) already work.
void attach_parent_console() {
#ifdef _WIN32
    if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_UNKNOWN) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
#endif
}

// An exported game: <Game>.exe next to data/manifest.json.
std::optional<std::string> package_beside_executable() {
    const char* base = SDL_GetBasePath();
    if (!base) return std::nullopt;
    const std::filesystem::path data = std::filesystem::path(base) / "data";
    if (std::filesystem::is_regular_file(data / "manifest.json")) return data.string();
    return std::nullopt;
}

#ifdef __ANDROID__
bool write_file(const std::filesystem::path& path, const void* data, size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(static_cast<const char*>(data), std::streamsize(size));
    return bool(out);
}

// Copies the APK's assets/data/ package to internal storage, unless this exact package is already
// there. Returns the folder, or nothing if the APK holds no package.
std::optional<std::string> package_from_assets() {
    size_t size = 0;
    void* manifest_text = SDL_LoadFile("data/manifest.json", &size);  // read from the APK assets
    if (!manifest_text) return std::nullopt;
    const std::string manifest(static_cast<const char*>(manifest_text), size);
    SDL_free(manifest_text);

    const char* storage = SDL_GetAndroidInternalStoragePath();
    if (!storage) throw std::runtime_error(std::string("No internal storage: ") + SDL_GetError());
    const std::filesystem::path dir = std::filesystem::path(storage) / "data";
    {
        std::ifstream in(dir / "manifest.json", std::ios::binary);
        const std::string installed((std::istreambuf_iterator<char>(in)), {});
        if (in && installed == manifest) return dir.string();
    }
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    // The exporter lists the package's files in data/files.txt (scripts/, icon.png and the rest).
    std::vector<std::string> files;
    if (void* list = SDL_LoadFile("data/files.txt", &size)) {
        std::string line;
        for (const char c : std::string(static_cast<const char*>(list), size)) {
            if (c != '\n') {
                line += c;
            } else if (!line.empty()) {
                files.push_back(line);
                line.clear();
            }
        }
        if (!line.empty()) files.push_back(line);
        SDL_free(list);
    } else {  // an APK from before the list existed
        const midgard::Manifest m = midgard::parse_manifest(manifest);
        files = {m.database, "game.pak", m.game_name + ".config"};
    }
    for (const std::string& name : files) {
        if (name == "manifest.json" || name == "files.txt") continue;
        void* bytes = SDL_LoadFile(("data/" + name).c_str(), &size);
        if (!bytes) continue;  // optional parts of a package
        std::filesystem::create_directories((dir / name).parent_path());
        const bool ok = write_file(dir / name, bytes, size);
        SDL_free(bytes);
        if (!ok) throw std::runtime_error("Could not write " + (dir / name).string());
    }
    // The manifest goes last: it marks a complete copy.
    if (!write_file(dir / "manifest.json", manifest.data(), manifest.size()))
        throw std::runtime_error("Could not write " + (dir / "manifest.json").string());
    say("Unpacked the game data to %s\n", dir.string().c_str());
    return dir.string();
}

#ifdef MIDGARD_HAS_SCRIPTING
// The game's Python on Android: the APK's python.zip asset (the standard library and its
// compiled modules) unpacked into the app's storage, again only after an update changes it.
std::filesystem::path android_python_home() {
    const char* storage = SDL_GetAndroidInternalStoragePath();
    if (!storage) throw std::runtime_error(std::string("No internal storage: ") + SDL_GetError());
    const std::filesystem::path home = std::filesystem::path(storage) / "python";
    size_t size = 0;
    void* archive = SDL_LoadFile("python.zip", &size);
    if (!archive) throw std::runtime_error("python.zip is missing from the game's APK");
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, archive, size, 0)) {
        SDL_free(archive);
        throw std::runtime_error("python.zip in the game's APK is damaged");
    }
    std::string version;
    size_t version_size = 0;
    if (void* v = mz_zip_reader_extract_file_to_heap(&zip, "VERSION", &version_size, 0)) {
        version.assign(static_cast<const char*>(v), version_size);
        mz_free(v);
    }
    std::ifstream installed_file(home / "VERSION", std::ios::binary);
    const std::string installed((std::istreambuf_iterator<char>(installed_file)), {});
    if (version.empty() || installed != version) {
        std::filesystem::remove_all(home);
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
            char name[512];
            mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
            if (mz_zip_reader_is_file_a_directory(&zip, i) || std::string(name) == "VERSION")
                continue;
            const std::filesystem::path target = home / name;
            std::filesystem::create_directories(target.parent_path());
            if (!mz_zip_reader_extract_to_file(&zip, i, target.string().c_str(), 0)) {
                mz_zip_reader_end(&zip);
                SDL_free(archive);
                throw std::runtime_error("Could not unpack Python: " + target.string());
            }
        }
        write_file(home / "VERSION", version.data(), version.size());  // last: a complete copy
        say("Unpacked Python %s\n", version.c_str());
    }
    mz_zip_reader_end(&zip);
    SDL_free(archive);
    return home;
}
#endif
#endif

bool want_validation() {
    return std::getenv("MIDGARD_VULKAN_VALIDATION") != nullptr;
}

// ---- the in-game UI (milestone 6.0) --------------------------------------------------------
// The HUD with made-up data (--hud-test): every part of it at once, for pictures and a look.
midgard::ui::HudData sample_hud() {
    midgard::ui::HudData d;
    d.crosshair = true;
    d.stats = {{"health", "Health", true, 72.0, 100.0},
               {"stamina", "Stamina", true, 35.0, 100.0},
               {"mana", "Mana", true, 50.0, 80.0},
               {"score", "Score", false, 1250.0, 0.0},
               {"keys", "Keys", false, 3.0, 5.0}};
    d.timers = {{"escape", "Escape", 65.0}};
    d.notes = {{"Picked up a key", 1.0f}, {"Found a secret", 1.0f}, {"Saved", 0.5f}};
    d.prompt = "Press E to open";
    d.heading = 30.0;
    static const std::vector<uint8_t> ground = [] {  // hills in greens and a lake
        const int n = 64;
        std::vector<uint8_t> px(size_t(n) * n * 4);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const double dx = x - 44.0, dy = y - 20.0;
                const bool lake = dx * dx + dy * dy < 90.0;
                const double hill = 0.5 + 0.25 * std::sin(x * 0.2) * std::cos(y * 0.15);
                uint8_t* p = &px[(size_t(y) * n + x) * 4];
                p[0] = lake ? 40 : uint8_t(70 + 60 * hill);
                p[1] = lake ? 90 : uint8_t(100 + 50 * hill);
                p[2] = lake ? 140 : uint8_t(40 + 20 * hill);
                p[3] = 255;
            }
        return px;
    }();
    d.map.shown = true;
    d.map.ground = &ground;
    d.map.size = 64;
    d.map.version = 1;
    d.map.marks = {{"exit", "", 40.0, 70.0}, {"mark", "Shrine", -60.0, -30.0}};
    return d;
}

// The UI's messages go to the game's log.
midgard::ui::Platform ui_log_only() {
    midgard::ui::Platform p;
    p.log = [](int level, const std::string& message) {
        if (level >= 1)
            warn("UI: %s\n", message.c_str());
        else
            say("UI: %s\n", message.c_str());
    };
    return p;
}

bool parse_insets(std::string_view text, midgard::ui::Insets& out) {
    float v[4] = {};
    size_t at = 0;
    for (int i = 0; i < 4; ++i) {
        const size_t comma = text.find(',', at);
        if ((i < 3) != (comma != std::string_view::npos)) return false;
        const std::string part(
            text.substr(at, comma == std::string_view::npos ? text.npos : comma - at));
        char* end = nullptr;
        v[i] = std::strtof(part.c_str(), &end);
        if (part.empty() || *end != '\0' || v[i] < 0.0f) return false;
        at = comma + 1;
    }
    out = {v[0], v[1], v[2], v[3]};
    return true;
}

// The game's menu pointer (the Project Page's picture): the mouse pointer over menus in place
// of the system's arrow and hand. Its tip is the picture's top left.
SDL_Cursor* g_menu_pointer = nullptr;

void set_menu_pointer(const std::vector<uint8_t>& encoded) {
    SDL_Cursor* old = g_menu_pointer;
    g_menu_pointer = nullptr;
    if (!encoded.empty()) {
        int w = 0, h = 0, channels = 0;
        if (stbi_uc* pixels =
                stbi_load_from_memory(encoded.data(), int(encoded.size()), &w, &h, &channels, 4)) {
            if (SDL_Surface* surface =
                    SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4)) {
                g_menu_pointer = SDL_CreateColorCursor(surface, 0, 0);
                SDL_DestroySurface(surface);
            }
            stbi_image_free(pixels);
        }
    }
    SDL_SetCursor(g_menu_pointer ? g_menu_pointer : SDL_GetDefaultCursor());
    if (old) SDL_DestroyCursor(old);
}

// What the UI needs from the window: SDL's cursors, clipboard and on-screen keyboard.
midgard::ui::Platform ui_platform(SDL_Window* window) {
    midgard::ui::Platform p = ui_log_only();
    p.set_cursor = [](const std::string& name) {
        static std::map<SDL_SystemCursor, SDL_Cursor*> cursors;
        SDL_SystemCursor which = SDL_SYSTEM_CURSOR_DEFAULT;
        if (name == "pointer" || name == "hand")
            which = SDL_SYSTEM_CURSOR_POINTER;
        else if (name == "text")
            which = SDL_SYSTEM_CURSOR_TEXT;
        else if (name == "move")
            which = SDL_SYSTEM_CURSOR_MOVE;
        else if (name == "resize")
            which = SDL_SYSTEM_CURSOR_NWSE_RESIZE;
        else if (name == "cross")
            which = SDL_SYSTEM_CURSOR_CROSSHAIR;
        if (g_menu_pointer &&
            (which == SDL_SYSTEM_CURSOR_DEFAULT || which == SDL_SYSTEM_CURSOR_POINTER)) {
            SDL_SetCursor(g_menu_pointer);  // (the game's own pointer)
            return;
        }
        SDL_Cursor*& c = cursors[which];
        if (!c) c = SDL_CreateSystemCursor(which);
        if (c) SDL_SetCursor(c);
    };
    p.set_clipboard = [](const std::string& text) { SDL_SetClipboardText(text.c_str()); };
    p.get_clipboard = [] {
        char* text = SDL_GetClipboardText();
        std::string out = text ? text : "";
        SDL_free(text);
        return out;
    };
    p.keyboard = [window](bool on, float x, float y) {
        if (on) {
            const float to_points = 1.0f / std::max(SDL_GetWindowPixelDensity(window), 0.01f);
            const SDL_Rect caret{int(x * to_points), int(y * to_points), 1, 24};
            SDL_SetTextInputArea(window, &caret, 0);
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    };
    return p;
}

// Where the window keeps clear of the notch and rounded corners, in pixels.
midgard::ui::Insets window_safe_area(SDL_Window* window) {
    SDL_Rect safe{};
    int w = 0, h = 0;
    if (!SDL_GetWindowSafeArea(window, &safe) || !SDL_GetWindowSize(window, &w, &h)) return {};
    const float px = SDL_GetWindowPixelDensity(window);
    return {float(safe.x) * px, float(safe.y) * px, float(w - safe.x - safe.w) * px,
            float(h - safe.y - safe.h) * px};
}

// The mouse as a finger (--touch-controls): the left button down, moving with it held, and
// up, as finger -1 (false: not one of those).
bool mouse_finger(SDL_Window* window, const SDL_Event& e, midgard::ui::Input& in) {
    using T = midgard::ui::Input::Type;
    const float px = SDL_GetWindowPixelDensity(window);
    in.touch = -1;
    switch (e.type) {
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.which == SDL_TOUCH_MOUSEID || e.button.button != SDL_BUTTON_LEFT)
                return false;
            in.type = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? T::TouchDown : T::TouchUp;
            in.x = e.button.x * px;
            in.y = e.button.y * px;
            return true;
        case SDL_EVENT_MOUSE_MOTION:
            if (e.motion.which == SDL_TOUCH_MOUSEID || !(e.motion.state & SDL_BUTTON_LMASK))
                return false;
            in.type = T::TouchMove;
            in.x = e.motion.x * px;
            in.y = e.motion.y * px;
            return true;
        default:
            return false;
    }
}

// One SDL event as the UI's input (false: not one the UI takes).
bool ui_input(SDL_Window* window, const SDL_Event& e, midgard::ui::Input& in) {
    using T = midgard::ui::Input::Type;
    const float px = SDL_GetWindowPixelDensity(window);
    const SDL_Keymod mods = SDL_GetModState();
    in.shift = mods & SDL_KMOD_SHIFT;
    in.ctrl = mods & SDL_KMOD_CTRL;
    in.alt = mods & SDL_KMOD_ALT;
    in.meta = mods & SDL_KMOD_GUI;
    switch (e.type) {
        case SDL_EVENT_MOUSE_MOTION:
            if (e.motion.which == SDL_TOUCH_MOUSEID) return false;  // (touch comes as touch)
            in.type = T::PointerMove;
            in.x = e.motion.x * px;
            in.y = e.motion.y * px;
            return true;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.which == SDL_TOUCH_MOUSEID) return false;
            in.type = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? T::PointerDown : T::PointerUp;
            in.x = e.button.x * px;
            in.y = e.button.y * px;
            in.button = e.button.button == SDL_BUTTON_LEFT    ? 0
                        : e.button.button == SDL_BUTTON_RIGHT ? 1
                                                              : 2;
            return true;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            in.type = T::PointerLeave;
            return true;
        case SDL_EVENT_MOUSE_WHEEL:
            in.type = T::Wheel;
            in.x = e.wheel.x;
            in.y = e.wheel.y;
            return true;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            in.type = e.type == SDL_EVENT_KEY_DOWN ? T::KeyDown : T::KeyUp;
            in.key = SDL_GetKeyName(e.key.key);
            return true;
        case SDL_EVENT_TEXT_INPUT:
            in.type = T::Text;
            in.text = e.text.text ? e.text.text : "";
            return true;
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED: {
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(window, &pw, &ph);
            in.type = e.type == SDL_EVENT_FINGER_DOWN     ? T::TouchDown
                      : e.type == SDL_EVENT_FINGER_MOTION ? T::TouchMove
                      : e.type == SDL_EVENT_FINGER_UP     ? T::TouchUp
                                                          : T::TouchCancel;
            in.touch = int64_t(e.tfinger.fingerID);
            in.x = e.tfinger.x * float(pw);
            in.y = e.tfinger.y * float(ph);
            return true;
        }
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP: {
            const char* name = SDL_GetGamepadStringForButton(SDL_GamepadButton(e.gbutton.button));
            if (!name) return false;
            in.type = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? T::PadDown : T::PadUp;
            in.key = name;
            return true;
        }
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
            const char* name = SDL_GetGamepadStringForAxis(SDL_GamepadAxis(e.gaxis.axis));
            if (!name) return false;
            in.type = T::PadAxis;
            in.key = name;
            in.x = float(e.gaxis.value) / 32767.0f;
            return true;
        }
        default:
            return false;
    }
}

int run_screenshot(const Options& opt, Source& source) {
    render::RendererDesc desc;
    desc.offscreen = true;
    desc.width = opt.width;
    desc.height = opt.height;
    desc.validation = want_validation();
    desc.quality = render::quality_settings(opt.quality.value_or(render::default_quality()));
    render::Renderer renderer(desc);
    source.upload(renderer);
    if (opt.overlay_text) renderer.set_overlay(overlay_lines(*opt.overlay_text));
    renderer.set_exposure(opt.exposure);
    const render::Camera camera = source.camera(float(opt.width) / float(opt.height));
    source.stream(renderer, camera.origin, true);
    renderer.set_environment(source.environment(camera.origin));
    const auto draws = source.draws(0.6f, camera.origin);
    renderer.set_terrain_look(source.terrain_look());  // (after draws: the season is set)
    if (opt.debug_physics) renderer.set_debug_lines(source.debug_lines(camera.origin));
    // The UI test screen (6.0), checked as screen tests check every screen: a gamepad reaches
    // every control, text fits, and (--ui-touch) each control is big enough for a finger.
    std::unique_ptr<midgard::ui::Ui> ui;
    std::unique_ptr<midgard::ui::Hud> hud;  // (after the UI: it goes first)
    const auto hud_data = opt.hud_test   ? std::optional(sample_hud())
                          : opt.show_hud ? source.hud()
                                         : std::nullopt;
    if (opt.ui_test || hud_data || !opt.screen_test.empty()) {
        midgard::ui::Options uo;
        uo.width = opt.width;
        uo.height = opt.height;
        uo.scale = opt.ui_scale;
        uo.safe_area = opt.safe_area.value_or(midgard::ui::Insets{});
        uo.platform = ui_log_only();
        ui = std::make_unique<midgard::ui::Ui>(renderer, uo);
    }
    std::unique_ptr<midgard::ui::TouchControls> touch;  // (after the UI: it goes first)
    if (hud_data) {
        hud = std::make_unique<midgard::ui::Hud>(*ui);
        if (!opt.hud_test) source.hud_shown();
        hud->show(opt.hud_test ? *hud_data : *source.hud());  // (with the minimap's ground)
        if (opt.touch_controls) {                             // (as a phone shows them, at rest)
            touch = std::make_unique<midgard::ui::TouchControls>(*ui, source.touch_layout());
            touch->set_shown(true);
            touch->update();
            hud->set_touch(true);
        }
    }
    if (opt.ui_test) {
        Rml::ElementDocument* screen = midgard::ui::open_test_screen(*ui);
        if (!screen) {
            std::fprintf(stderr, "error: the UI test screen didn't open\n");
            return 1;
        }
        ui->update(0.0);
        const auto problems = ui->check(screen, opt.ui_touch);
        for (const auto& p : problems)
            std::fprintf(stderr, "UI %s: %s: %s\n", p.kind.c_str(), p.element.c_str(),
                         p.message.c_str());
        if (!problems.empty()) {
            std::fprintf(stderr, "error: the UI test screen has %zu problem(s)\n", problems.size());
            return 1;
        }
        // (the checks pressed its controls: the picture is of it as it opens)
        ui->close(screen);
        ui->update(0.0);
        midgard::ui::open_test_screen(*ui);
    }
    if (!opt.screen_test.empty()) {
        midgard::ScreenDef def;
        try {
            std::ifstream in(opt.screen_test, std::ios::binary);
            const std::string json((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            def = midgard::screen_from_json("screen", "Screen test", json);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: %s: %s\n", opt.screen_test.c_str(), e.what());
            return 1;
        }
        // (its pictures: "ui:NAME" is the file NAME beside the screen's)
        for (const std::string& image : midgard::screen_images(def)) {
            const std::filesystem::path file =
                std::filesystem::path(opt.screen_test).parent_path() / image.substr(3);
            std::ifstream in(file, std::ios::binary);
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                             std::istreambuf_iterator<char>());
            if (!ui->set_image_file(midgard::screen_image_name(image), bytes)) {
                std::fprintf(stderr, "error: can't read the picture %s\n", file.string().c_str());
                return 1;
            }
        }
        const std::string rml = midgard::screen_rml(def);
        Rml::ElementDocument* screen = ui->open_text(rml, "midgard://screen/test", def.modal);
        if (!screen) {
            std::fprintf(stderr, "error: the screen didn't open\n");
            return 1;
        }
        ui->update(0.0);
        const auto problems = ui->check(screen, opt.ui_touch);
        for (const auto& p : problems)
            std::fprintf(stderr, "UI %s: %s: %s\n", p.kind.c_str(), p.element.c_str(),
                         p.message.c_str());
        if (!problems.empty()) {
            std::fprintf(stderr, "error: the screen has %zu problem(s)\n", problems.size());
            return 1;
        }
        ui->close(screen);  // (the checks pressed its controls: draw it as it opens)
        ui->update(0.0);
        ui->open_text(rml, "midgard://screen/test", def.modal);
    }
    if (ui) {
        ui->update(0.0);
        ui->render();
    }
    renderer.render_frame(camera, source.light(camera.origin), draws, source.sky());
    // With MIDGARD_VULKAN_VALIDATION set, anything the validation layer reports fails the run
    // (CI's image tests run this way).
    if (!renderer.validation_messages().empty()) {
        for (const std::string& m : renderer.validation_messages())
            std::fprintf(stderr, "validation %s\n", m.c_str());
        std::fprintf(stderr, "error: the Vulkan validation layer reported %zu problem(s)\n",
                     renderer.validation_messages().size());
        return 1;
    }
    if (!render::write_png(*opt.screenshot, opt.width, opt.height, renderer.read_pixels())) {
        std::fprintf(stderr, "error: could not write %s\n", opt.screenshot->c_str());
        return 1;
    }
    // With --frames N: the same view drawn N more times, the median frame time said (the
    // World Tour's speed budgets, milestone 5.18).
    if (opt.frames > 0) {
        std::vector<double> times;
        for (long i = 0; i < opt.frames; ++i) {
            const auto start = std::chrono::steady_clock::now();
            renderer.render_frame(camera, source.light(camera.origin), draws, source.sky());
            times.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count());
        }
        std::sort(times.begin(), times.end());
        say("Frame time: %.3f ms median over %ld frames%s\n", times[times.size() / 2], opt.frames,
            renderer.software() ? " (software renderer)" : "");
    }
    say("Rendered %ux%u on %s to %s\n", opt.width, opt.height, renderer.device_name().c_str(),
        opt.screenshot->c_str());
    return 0;
}

// The game loop under the editor's live link, at 60 frames a second, without a window or GPU:
// edits, scripts, pause and stop work as in a window; nothing is drawn.
int run_headless(const Options& opt, Source& source) {
    if (!opt.editor_link) {
        fail("--headless needs --editor-link");
        return 2;
    }
    std::string host;
    uint16_t port = 0;
    if (!midgard::link::parse_endpoint(*opt.editor_link, host, port)) {
        fail("--editor-link needs host:port, not " + *opt.editor_link);
        return 2;
    }
    int result = 0;
    try {
        midgard::link::Client link(host, port);
        g_link = &link;
        link.send({{"type", "hello"},
                   {"role", "runtime"},
                   {"version", midgard::kVersion},
                   {"protocol", midgard::link::kProtocol}});
        midgard::Actions actions(source.input_map());
        const auto frame_time = std::chrono::milliseconds(16);
        auto next = std::chrono::steady_clock::now();
        auto stats_due = next + std::chrono::seconds(1);
        bool paused = false;
        long frames = 0;
        while (link.connected()) {
            bool stop = false;
            for (const nlohmann::json& message : link.receive()) {
                const std::string type = message.value("type", "");
                if (type == "stop")
                    stop = true;
                else if (type == "pause" || type == "resume")
                    paused = type == "pause";
                else if (type != "hello" && type != "play")
                    source.apply(message, nullptr);
            }
            if (stop) break;
            actions.begin_frame();
            actions.set_context(source.input_context());
            if (!paused) source.update(actions, 1.0f / 60.0f);
            ++frames;
            const auto now = std::chrono::steady_clock::now();
            if (now >= stats_due) {
                link.send(
                    {{"type", "stats"}, {"fps", frames}, {"draw_calls", 0}, {"paused", paused}});
                frames = 0;
                stats_due = now + std::chrono::seconds(1);
            }
            next += frame_time;
            std::this_thread::sleep_until(next);
        }
    } catch (const std::exception& e) {
        fail(e.what());
        result = 1;
    }
    g_link = nullptr;
    return result;
}

// Shows the game's icon in the window's title bar and the taskbar (the Windows executable also
// carries it as a resource, for Explorer).
void set_window_icon(SDL_Window* window, const std::filesystem::path& file) {
    const std::u8string u8 = file.u8string();
    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels =
        stbi_load(std::string(u8.begin(), u8.end()).c_str(), &width, &height, &channels, 4);
    if (!pixels) {
        warn("The game's icon %s could not be read\n", file.string().c_str());
        return;
    }
    if (SDL_Surface* surface =
            SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, pixels, width * 4)) {
        SDL_SetWindowIcon(window, surface);
        SDL_DestroySurface(surface);
    }
    stbi_image_free(pixels);
}

// The splash (milestone 6.0): while the game loads, its splash screen (the Menu editor's,
// used as the splash), else its name over black;
// drawn again and again (the window answering the system) until `ready` says the game has
// loaded and it has shown as long as the Project Page says. Its UI goes before the game's.
void show_splash(SDL_Window* window, render::Renderer& renderer, const Options& opt,
                 const std::filesystem::path& package_dir, const std::string& title,
                 const std::function<bool()>& ready) {
    std::optional<midgard::ScreenDef> screen;
    double least = 0.0;  // seconds it shows, however soon the game is ready (the screen's)
    try {
        const midgard::Manifest m = midgard::load_manifest(package_dir / "manifest.json");
        screen = midgard::load_splash_screen(package_dir / m.database);
        if (screen) least = screen->seconds;
    } catch (const std::exception&) {
    }
    std::unique_ptr<midgard::ui::Ui> ui;
    try {
        midgard::ui::Options uo;
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        uo.width = uint32_t(pw);
        uo.height = uint32_t(ph);
        uo.density = SDL_GetWindowDisplayScale(window);
        uo.scale = opt.ui_scale;
        uo.platform = ui_log_only();
        ui = std::make_unique<midgard::ui::Ui>(renderer, uo);
        if (screen) {  // (its pictures and fonts, from the package)
            for (const std::string& image : midgard::screen_images(*screen))
                ui->set_image_file(midgard::screen_image_name(image),
                                   midgard::read_package_content(package_dir, image));
            std::set<std::string> tried;
            midgard::ui::load_fonts(
                *ui, *screen,
                [&](const std::string& folder) {
                    return midgard::read_package_fonts(package_dir, folder);
                },
                tried);
        }
        const std::string rml =
            screen ? midgard::screen_rml(*screen)
                   : "<rml><head><link type=\"text/rcss\" href=\"midgard://base.rcss\"/><style>"
                     "body { background-color: #000000; }"
                     "#title { position: absolute; left: 0; right: 0; top: 45%; text-align: center;"
                     " font-size: 36dp; color: #e8ecf4; }"
                     "</style></head><body><div id=\"title\">" +
                         midgard::rml_escape(title) + "</div></body></rml>";
        ui->open_text(rml, "midgard://splash.rml", false);
    } catch (const std::exception& e) {
        warn("No splash: %s\n", e.what());
        ui.reset();
    }
    double t = 0.0;
    const auto start = std::chrono::steady_clock::now();
    const auto done = [&] {
        return ready() &&
               std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >=
                   least;
    };
    do {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                renderer.resize(uint32_t(e.window.data1), uint32_t(e.window.data2));
                if (ui) ui->resize(uint32_t(e.window.data1), uint32_t(e.window.data2));
            }
        }
        if (ui) {
            ui->update(t);
            ui->render();
        }
        renderer.render_frame(render::Camera{}, render::Light{}, {},
                              glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        t += 1.0 / 60.0;
        if (!done()) SDL_Delay(16);
    } while (!done());
    ui.reset();
    renderer.set_ui({});  // (the game's UI draws from now on)
}

int run_window(const Options& opt, std::unique_ptr<Source>& owned,
               const std::function<std::unique_ptr<Source>()>& load) {
    // (Without the game yet: its name from its manifest, for the window.)
    std::string title = owned ? owned->title() : "Midgard";
    if (!owned && opt.package_dir) {
        try {
            title =
                midgard::load_manifest(std::filesystem::path(*opt.package_dir) / "manifest.json")
                    .game_name;
        } catch (const std::exception&) {
        }
    }
    SDL_WindowFlags flags =
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#ifdef __ANDROID__
    flags |= SDL_WINDOW_FULLSCREEN;
#endif
    SDL_Window* window = SDL_CreateWindow(title.c_str(), int(opt.width), int(opt.height), flags);
    if (!window) {
        fail(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
        return 1;
    }
    if (owned)
        if (const auto icon = owned->icon()) set_window_icon(window, *icon);
    int result = 0;
    std::unique_ptr<midgard::link::Client> link;
    try {
        if (opt.editor_link) {
            std::string host;
            uint16_t port = 0;
            if (!midgard::link::parse_endpoint(*opt.editor_link, host, port))
                throw std::runtime_error("--editor-link needs host:port, not " + *opt.editor_link);
            link = std::make_unique<midgard::link::Client>(host, port);
            link->send({{"type", "hello"},
                        {"role", "runtime"},
                        {"version", midgard::kVersion},
                        {"protocol", midgard::link::kProtocol}});
            g_link = link.get();
        }
        render::RendererDesc desc;
        Uint32 count = 0;
        const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&count);
        desc.instance_extensions.assign(exts, exts + count);
        desc.create_surface = [window](VkInstance_T* instance) -> VkSurfaceKHR_T* {
            VkSurfaceKHR surface = nullptr;
            if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) return nullptr;
            return surface;
        };
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        desc.width = uint32_t(pw);
        desc.height = uint32_t(ph);
        desc.validation = want_validation();
        desc.quality = render::quality_settings(opt.quality.value_or(render::default_quality()));
        render::Renderer renderer(desc);
        say("Rendering on %s\n", renderer.device_name().c_str());
        // The game loads (in the background) while its splash shows.
        if (!owned) {
            auto loading = std::async(std::launch::async, load);
            show_splash(
                window, renderer, opt, opt.package_dir.value_or(std::string(".")), title, [&] {
                    return loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                });
            owned = loading.get();  // (a game that can't load throws here, as before)
            if (const auto icon = owned->icon()) set_window_icon(window, *icon);
        }
        Source& source = *owned;
        // Sound (milestone 5.14): the system's audio device, or silence without one.
        std::unique_ptr<midgard::audio::Mixer> sound;
        try {
            sound = std::make_unique<midgard::audio::Mixer>();
            if (sound->has_device())
                say("Sound on %s, %u Hz\n", sound->backend().c_str(), sound->sample_rate());
            else
                say("No sound device: the game is silent\n");
        } catch (const midgard::audio::AudioError& e) {
            say("No sound: %s\n", e.what());
        }
        source.set_audio(sound.get());
        // (The game lets go of the mixer before it goes, however this ends.)
        struct LetGo {
            Source& source;
            ~LetGo() { source.set_audio(nullptr); }
        } let_go{source};
        // (The ground's textures made ready on worker threads: the window carries on meanwhile.)
        renderer.set_ground_async(true);
        source.upload(renderer);
        // The in-game UI (6.0): sized by the display's density and the player's scale, clear of
        // the notch. Without its fonts the game still runs, with no screens.
        std::unique_ptr<midgard::ui::Ui> ui;
        std::unique_ptr<midgard::ui::Hud> hud;  // (after the UI: it goes first)
        std::unique_ptr<midgard::ui::TouchControls> touch;
        std::unique_ptr<midgard::ui::Screens> screens;  // the Menu editor's, as the game shows
        // The touch controls: on a phone without a controller, or asked for (with the mouse as
        // a finger). A finger shows them; a key, a mouse click or a controller hides them.
        bool touch_wanted = opt.touch_controls;
        bool touch_menu_held = false;  // (the Menu button opens the pause menu as it goes down)
        std::string shown_crosshair, shown_pointer;  // (the game settings' pictures, as loaded)
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
        touch_wanted = touch_wanted || !SDL_HasGamepad();
#endif
        try {
            midgard::ui::Options uo;
            uo.width = desc.width;
            uo.height = desc.height;
            uo.density = SDL_GetWindowDisplayScale(window);
            uo.scale = opt.ui_scale;
            uo.safe_area = opt.safe_area.value_or(window_safe_area(window));
            uo.platform = ui_platform(window);
            ui = std::make_unique<midgard::ui::Ui>(renderer, uo);
            // The HUD, for a game (or --hud-test), under any menu.
            if (opt.hud_test || source.hud()) {
                hud = std::make_unique<midgard::ui::Hud>(*ui);
                if (!opt.hud_test) source.hud_shown();
                touch = std::make_unique<midgard::ui::TouchControls>(*ui, source.touch_layout());
            }
            if (source.screen_defs())
                screens = std::make_unique<midgard::ui::Screens>(
                    *ui, [&source](const std::string& c) { return source.ui_image(c); },
                    [&source](const std::string& f) { return source.ui_fonts(f); });
            if (opt.ui_test && !midgard::ui::open_test_screen(*ui))
                warn("The UI test screen didn't open\n");
        } catch (const std::exception& e) {
            warn("No in-game UI: %s\n", e.what());
            screens.reset();
            touch.reset();
            hud.reset();
        }
        midgard::Actions actions(source.input_map());
        // The player's own keys (a settings screen's key buttons; kept in the profile).
        for (const auto& [action, key] : source.player_keys())
            actions.rebind(action, "keyboard", key);
        // Mouse-look captures the mouse. Under the editor the game starts with the mouse free
        // (so the editor's panels stay reachable): a click in the game captures it, and Esc,
        // Shift+F1 or switching away frees it. An exported game captures it from the start.
        bool captured = false;
        const auto capture = [&](bool on) {
            // (with the touch controls asked for, the mouse is a finger: never captured)
            on = on && source.wants_mouse_look() && !opt.touch_controls;
            if (on == captured) return;
            captured = on;
            SDL_SetWindowRelativeMouseMode(window, on);
            if (link) {
                const std::string hint = on ? " (Esc frees the mouse)" : " (click to look around)";
                SDL_SetWindowTitle(window, (source.title() + hint).c_str());
            }
        };
        if (link && source.wants_mouse_look())
            SDL_SetWindowTitle(window, (source.title() + " (click to look around)").c_str());
        capture(!link && !(ui && ui->modal()));
        bool was_modal = ui && ui->modal();

        const auto start = std::chrono::steady_clock::now();
        auto last = start;
        bool running = true;
        bool background = false;  // Android: the app is not visible and has no surface
        bool paused = false;      // the editor's Pause: drawn, but no gameplay
        bool show_stats = opt.debug_overlay && opt.fixed_overlay.empty();
        bool show_physics = opt.debug_physics;
        if (!opt.fixed_overlay.empty()) renderer.set_overlay(opt.fixed_overlay);
        long frame = 0;
        long frames_this_second = 0;
        double frame_seconds = 0.0, worst_frame = 0.0;  // over the last second
        size_t draw_count = 0;
        auto stats_due = start + std::chrono::seconds(1);
        while (running && (opt.frames < 0 || frame < opt.frames)) {
            if (link) {
                if (!link->connected()) {
                    say("The editor closed the live link.\n");
                    break;
                }
                for (const nlohmann::json& message : link->receive()) {
                    const std::string type = message.value("type", "");
                    if (type == "stop")
                        running = false;
                    else if (type == "pause" || type == "resume")
                        paused = type == "pause";
                    else if (type != "hello" && type != "play")
                        source.apply(message, &renderer);
                }
            }
            actions.begin_frame();
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                bool consumed = false;  // handled here; the game's actions don't see it
                // The UI first: a menu takes everything, the HUD what is aimed at it. (Android's
                // Back button is a menu's Escape.)
                // A settings screen's key button waits for a key: this one (Esc gives up).
                if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && !source.rebinding().empty()) {
                    const std::string key = e.key.scancode == SDL_SCANCODE_ESCAPE
                                                ? std::string()
                                                : SDL_GetScancodeName(e.key.scancode);
                    const std::string action = source.key_chosen(key);
                    if (!action.empty()) actions.rebind(action, "keyboard", key);
                    continue;
                }
                // The menu action's keys and buttons (Esc, a phone's Back, Start): back from a
                // menu, or the game's pause menu. (Without one, as before: under the editor Esc
                // frees the mouse; an exported game quits.)
                const bool menu_key = (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                                       (e.key.scancode == SDL_SCANCODE_ESCAPE ||
                                        e.key.scancode == SDL_SCANCODE_AC_BACK)) ||
                                      (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                                       e.gbutton.button == SDL_GAMEPAD_BUTTON_START);
                if (menu_key && screens && source.menu()) {
                    if (link) capture(false);
                    continue;
                }
                if (ui) {
                    midgard::ui::Input in;
                    if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_AC_BACK &&
                        ui->modal()) {
                        in.type = midgard::ui::Input::Type::KeyDown;
                        in.key = "Escape";
                        ui->handle(in);
                        in.type = midgard::ui::Input::Type::KeyUp;
                        ui->handle(in);
                        continue;
                    }
                    const bool editor_key =
                        e.type == SDL_EVENT_KEY_DOWN &&
                        (e.key.scancode == SDL_SCANCODE_F3 || e.key.scancode == SDL_SCANCODE_F4 ||
                         e.key.scancode == SDL_SCANCODE_F5 || e.key.scancode == SDL_SCANCODE_F6);
                    // The touch controls take the fingers (and, asked for, the mouse as one)
                    // while no menu is up. A finger shows them; anything else hides them.
                    if (touch && !ui->modal()) {
                        const bool finger =
                            e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_FINGER_MOTION ||
                            e.type == SDL_EVENT_FINGER_UP || e.type == SDL_EVENT_FINGER_CANCELED;
                        if (finger && ui_input(window, e, in)) {
                            if (in.type == midgard::ui::Input::Type::TouchDown && !touch_wanted) {
                                touch_wanted = true;
                                touch->set_shown(true);
                            }
                            if (touch->handle(in)) continue;
                        }
                        if (opt.touch_controls && mouse_finger(window, e, in)) {
                            touch->handle(in);
                            continue;
                        }
                        const bool other_device =
                            (e.type == SDL_EVENT_KEY_DOWN && !editor_key && !e.key.repeat &&
                             e.key.scancode != SDL_SCANCODE_AC_BACK &&  // (a phone's own keys)
                             e.key.scancode != SDL_SCANCODE_VOLUMEUP &&
                             e.key.scancode != SDL_SCANCODE_VOLUMEDOWN) ||
                            e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
                            (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                             e.button.which != SDL_TOUCH_MOUSEID);
                        if (other_device && touch_wanted && !opt.touch_controls) {
                            touch_wanted = false;
                            touch->set_shown(false);
                        }
                        // (a finger's pretend mouse: the touch controls had the finger)
                        if ((e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                             e.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
                            e.button.which == SDL_TOUCH_MOUSEID)
                            continue;
                    }
                    if (!editor_key && ui_input(window, e, in) && ui->handle(in)) {
                        if (e.type == SDL_EVENT_KEY_UP || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP)
                            feed(actions, e);  // (a key let go reaches the game: none stick)
                        continue;
                    }
                }
                switch (e.type) {
                    case SDL_EVENT_QUIT:
                    case SDL_EVENT_TERMINATING:
                        running = false;
                        break;
                    case SDL_EVENT_WILL_ENTER_BACKGROUND:
                    case SDL_EVENT_DID_ENTER_BACKGROUND:
                        background = true;
                        break;
                    case SDL_EVENT_DID_ENTER_FOREGROUND:
                        if (background) renderer.recreate_surface();
                        background = false;
                        last = std::chrono::steady_clock::now();
                        break;
                    case SDL_EVENT_KEY_DOWN:
                        // Android's Back button quits until in-game menus exist (Phase 6).
                        if (e.key.scancode == SDL_SCANCODE_AC_BACK) running = false;
                        if (e.key.scancode == SDL_SCANCODE_ESCAPE && !link &&
                            !opt.fixed_overlay.empty())
                            running = false;  // closes the self-test's results
                        if (e.key.scancode == SDL_SCANCODE_F3 && !e.key.repeat &&
                            opt.fixed_overlay.empty()) {
                            show_stats = !show_stats;
                            if (!show_stats) renderer.set_overlay({});
                            consumed = true;
                        }
                        if (e.key.scancode == SDL_SCANCODE_F4 && !e.key.repeat &&
                            opt.fixed_overlay.empty()) {
                            show_physics = !show_physics;
                            consumed = true;
                        }
                        // Under the editor, its Pause (F6) and Stop (Shift+F5) keys also work
                        // while the game window has the keyboard.
                        if (link && !e.key.repeat) {
                            const bool shift = e.key.mod & SDL_KMOD_SHIFT;
                            if (e.key.scancode == SDL_SCANCODE_F5 && shift) {
                                running = false;
                            } else if (e.key.scancode == SDL_SCANCODE_F6) {
                                paused = !paused;
                                link->send(
                                    {{"type", "state"}, {"state", paused ? "paused" : "playing"}});
                            } else if (e.key.scancode == SDL_SCANCODE_ESCAPE ||
                                       (e.key.scancode == SDL_SCANCODE_F1 && shift)) {
                                // Frees the mouse. Under the editor Esc never quits the game
                                // (Stop does), so pressing it twice is harmless.
                                capture(false);
                                consumed = true;
                            }
                        }
                        break;
                    case SDL_EVENT_MOUSE_BUTTON_DOWN:
                        if (link && !captured) {
                            capture(true);
                            consumed = true;  // the click that captures isn't a game click
                        }
                        break;
                    case SDL_EVENT_WINDOW_FOCUS_LOST:
                        if (link) capture(false);
                        break;
                    case SDL_EVENT_MOUSE_MOTION:
                        // A free mouse moving over the window doesn't turn the camera.
                        consumed = !captured && source.wants_mouse_look();
                        break;
                    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                        renderer.resize(uint32_t(e.window.data1), uint32_t(e.window.data2));
                        if (ui) {
                            ui->resize(uint32_t(e.window.data1), uint32_t(e.window.data2));
                            if (!opt.safe_area) ui->set_safe_area(window_safe_area(window));
                        }
                        break;
                    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
                        if (ui) ui->set_density(SDL_GetWindowDisplayScale(window));
                        break;
                    case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED:
                        if (ui && !opt.safe_area) ui->set_safe_area(window_safe_area(window));
                        break;
                    default:
                        break;
                }
                if (!consumed) feed(actions, e);
            }
            if (background || (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) {
                SDL_Delay(50);
                continue;
            }
            // The screens: what the player did on them reaches the game (a button that quits,
            // quits); then they show as the game has them now.
            if (screens) {
                for (const midgard::ui::Screens::Event& ev : screens->take_events()) {
                    if (ev.kind == "click" && source.ui_click(ev.screen, ev.element))
                        running = false;
                    if (ev.kind == "change") source.ui_change(ev.screen, ev.element, ev.value);
                }
                if (const midgard::hud::HudState* h = source.hud_state())
                    screens->sync(*source.screen_defs(), h->screens(), h->texts, h->values,
                                  h->enabled);
            }
            // A menu (the UI test screen) frees the mouse while it is up; closing it gives the
            // mouse back to the game as it was.
            const bool is_modal = ui && ui->modal();
            if (is_modal != was_modal) {
                capture(!is_modal && !link);
                was_modal = is_modal;
            }
            // The game's crosshair picture and menu pointer (the Project Page's), loaded
            // when they change.
            if (const midgard::GameSettings* gs = source.game_settings(); gs && ui) {
                if (gs->crosshair != shown_crosshair) {
                    shown_crosshair = gs->crosshair;
                    if (!shown_crosshair.empty() &&
                        !ui->set_image_file("hud_crosshair", source.ui_image(shown_crosshair)))
                        warn("No crosshair picture %s\n", shown_crosshair.c_str());
                }
                if (gs->pointer != shown_pointer) {
                    shown_pointer = gs->pointer;
                    set_menu_pointer(shown_pointer.empty() ? std::vector<uint8_t>{}
                                                           : source.ui_image(shown_pointer));
                }
            }
            // The touch controls: hidden under a menu; what they do reaches the game as the
            // "touch" device (schema/migrations/0020_touch.sql binds it).
            if (touch) {
                touch->set_shown(touch_wanted && !is_modal);
                touch->set_layout(source.touch_layout());  // (the editor's edits, live)
                touch->set_context(source.input_context());
                touch->set_menu(source.has_pause_menu());
                if (hud) hud->set_touch(touch->shown());
                const midgard::ui::TouchControls::Output o = touch->take();
                actions.set("touch", "stick_x", o.stick_x);
                actions.set("touch", "stick_y", o.stick_y);
                actions.add_delta("touch", "drag_x", o.drag_x);
                actions.add_delta("touch", "drag_y", o.drag_y);
                for (const auto& [name, value] : o.buttons) {
                    // (Menu: the pause menu, as Esc opens it)
                    if (name == "menu") {
                        if (value != 0.0f && !touch_menu_held) source.menu();
                        touch_menu_held = value != 0.0f;
                        continue;
                    }
                    actions.set("touch", name, value);
                }
            }
            // The menu action quits until in-game menus exist (Phase 6).
            if (actions.pressed("menu") && !is_modal) running = false;
            MIDGARD_PROFILE_SCOPE("frame");
            const auto now = std::chrono::steady_clock::now();
            const double real_dt = std::chrono::duration<double>(now - last).count();
            const float dt = std::min(float(real_dt), 0.1f);
            last = now;
            if (!paused) {
                MIDGARD_PROFILE_SCOPE("update");
                // (A screen that pauses the game: everything stands still, but its music
                // plays on and scripts still hear its buttons.)
                source.set_frozen(screens && screens->pauses());
                actions.set_context(source.input_context());
                source.update(actions, dt);
            }
            const float t = std::chrono::duration<float>(now - start).count();
            const float aspect = float(renderer.width()) / float(renderer.height());
            const render::Camera camera = source.camera(aspect);
            {
                MIDGARD_PROFILE_SCOPE("terrain");
                source.stream(renderer, camera.origin, frame == 0);
            }
            const auto draws = source.draws(t, camera.origin);
            bool drawn = false;
            {
                MIDGARD_PROFILE_SCOPE("render");
                renderer.set_environment(source.environment(camera.origin));
                renderer.set_terrain_look(source.terrain_look());
                renderer.set_exposure(opt.exposure * (1.0f - source.fade()));
                renderer.set_debug_lines(show_physics ? source.debug_lines(camera.origin)
                                                      : std::vector<render::DebugLine>{});
                if (ui) {
                    if (touch) touch->update();
                    if (hud) {
                        if (const auto data =
                                opt.hud_test ? std::optional(sample_hud()) : source.hud())
                            hud->show(*data);
                    }
                    ui->update(t);
                    ui->render();
                }
                drawn =
                    renderer.render_frame(camera, source.light(camera.origin), draws, source.sky());
            }
            if (drawn) {
                ++frame;
                ++frames_this_second;
                frame_seconds += real_dt;
                worst_frame = std::max(worst_frame, real_dt);
                draw_count = draws.size();
            }
            if (now >= stats_due) {
                if (link)
                    link->send({{"type", "stats"},
                                {"fps", frames_this_second},
                                {"draw_calls", draw_count},
                                {"paused", paused}});
                if (show_stats) {
                    const double average =
                        frames_this_second ? frame_seconds / double(frames_this_second) : 0.0;
                    char timing[96];
                    std::snprintf(timing, sizeof(timing), "%ld fps  %.1f ms  (worst %.1f ms)",
                                  frames_this_second, average * 1000.0, worst_frame * 1000.0);
                    std::vector<render::OverlayLine> lines = {
                        {std::string("Midgard ") + midgard::kVersion},
                        {timing, {1.0f, 1.0f, 0.6f, 1.0f}},
                        {std::to_string(draw_count) + " draws"},
                        {renderer.device_name()}};
                    for (const std::string& line : source.status())
                        lines.push_back({line, {0.7f, 0.9f, 1.0f, 1.0f}});
                    lines.push_back(
                        {"F3 hides this, F4 outlines physics", {0.7f, 0.7f, 0.7f, 1.0f}});
                    renderer.set_overlay(lines);
                }
                frames_this_second = 0;
                frame_seconds = worst_frame = 0.0;
                stats_due = now + std::chrono::seconds(1);
            }
        }
    } catch (const std::exception& e) {
        fail(e.what());
        result = 1;
    }
    g_link = nullptr;
    SDL_DestroyWindow(window);
    return result;
}

}  // namespace

namespace {

// The folder holding the running executable.
std::filesystem::path executable_dir() {
    const char* base = SDL_GetBasePath();
    return base ? std::filesystem::path(base) : std::filesystem::current_path();
}

// The game's folder in the user's data folder (SDL_GetPrefPath): %APPDATA%\Midgard\<game> on
// Windows, ~/.local/share/Midgard/<game> on Linux, the app's internal storage on Android.
std::optional<std::filesystem::path> user_data_dir(const std::string& game);

// Where the player's profile lives: --player-profile, or for an exported game in a window,
// profiles/default/ in the user's data folder. Under the editor it's the one Play passes (for
// Continue); in tests there is none, so play starts the way the designer set it up.
std::optional<std::filesystem::path> player_profile_dir(const Options& opt, bool windowed,
                                                        const std::string& game) {
    if (opt.player_profile) return std::filesystem::path(*opt.player_profile);
    if (!windowed || opt.editor_link || opt.frames >= 0) return std::nullopt;
    if (const auto dir = user_data_dir(game)) return *dir / "profiles" / "default";
    return std::nullopt;
}

// Where the world is kept: --world, or for an exported game in a window, worlds/default in the
// user's data folder (until Phase 6's world list chooses one). Under the editor it's the one
// Play passes (for Continue); in tests there is none, so play starts at the calendar's start.
std::optional<std::filesystem::path> world_dir(const Options& opt, bool windowed,
                                               const std::string& game) {
    if (opt.world) return std::filesystem::path(*opt.world);
    if (!windowed || opt.editor_link || opt.frames >= 0) return std::nullopt;
    if (const auto dir = user_data_dir(game)) return *dir / "worlds" / "default";
    return std::nullopt;
}

std::optional<std::filesystem::path> user_data_dir(const std::string& game) {
    std::string folder;  // the game's name, made safe for a folder name
    for (const char c : game)
        folder += (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_')
                      ? c
                      : '_';
    if (folder.find_first_not_of(" _") == std::string::npos) folder = "Game";
    char* base = SDL_GetPrefPath("Midgard", folder.c_str());
    if (!base) return std::nullopt;
    const std::filesystem::path dir(reinterpret_cast<const char8_t*>(base));
    SDL_free(base);
    return dir;
}

int dump_api() {
#ifdef MIDGARD_HAS_SCRIPTING
    try {
        midgard::script::Interpreter python(midgard::script::python_home_beside(executable_dir()));
        std::printf("%s\n", midgard::script::api_names().c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
#else
    std::fprintf(stderr, "error: this runtime was built without scripting\n");
    return 1;
#endif
}

// Gameplay and scripts for a number of frames at 60 per second, with no window: its only input
// the keys --hold holds. Its sound is mixed offline as it goes (each frame's 1/60 s), and
// written to --record-audio's WAV.
int run_simulation(const Options& opt, Source& source) {
    midgard::Actions actions(source.input_map());
    midgard::audio::MixerOptions offline;
    offline.device = false;
    midgard::audio::Mixer mixer(offline);
    source.set_audio(&mixer);
    const std::uint32_t per_frame = mixer.sample_rate() / 60;
    std::vector<float> block(size_t(per_frame) * mixer.channels());
    std::vector<float> recorded;
    for (long frame = 0; frame < opt.simulate; ++frame) {
        actions.begin_frame();
        // (A key held more than once, at different times, is down while any of them is.)
        std::map<std::string, float> keys;
        for (const Options::Hold& h : opt.holds)
            keys[h.key] = std::max(keys[h.key], frame >= h.from && frame < h.to ? 1.0f : 0.0f);
        for (const auto& [key, value] : keys) actions.set("keyboard", key, value);
        actions.set_context(source.input_context());
        source.update(actions, 1.0f / 60.0f);
        source.settle();
        mixer.mix(block);
        if (opt.record_audio) recorded.insert(recorded.end(), block.begin(), block.end());
    }
    if (opt.dump_scene) std::printf("%s\n", source.dump().c_str());
    source.set_audio(nullptr);
    if (opt.record_audio) {
        const auto wav = midgard::audio::wav_bytes(recorded, mixer.channels(), mixer.sample_rate());
        std::ofstream out(*opt.record_audio, std::ios::binary);
        out.write(reinterpret_cast<const char*>(wav.data()), std::streamsize(wav.size()));
        if (!out) {
            fail("can't write " + *opt.record_audio);
            return 1;
        }
    }
    return 0;
}

int check_python() {
#ifdef MIDGARD_HAS_SCRIPTING
    try {
        midgard::script::Interpreter python(midgard::script::python_home_beside(executable_dir()));
        python.exec(
            "import asyncio, collections, dataclasses, datetime, enum, functools, itertools, "
            "json, math, random, re, statistics, string, textwrap, typing\n"
            "assert math.isclose(sum(range(4)) * 0.5, 3.0)");
        std::printf("Python %s: ok\n", python.version().c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
#else
    std::fprintf(stderr, "error: this runtime was built without scripting\n");
    return 1;
#endif
}

}  // namespace

namespace {

int run_selftest(Options& opt) {
    midgard::selftest::Options options;
    if (opt.package_dir) options.package = std::filesystem::path(*opt.package_dir);
    options.quality = opt.quality;
#ifdef __ANDROID__
    options.vulkan_required = true;
#ifdef MIDGARD_HAS_SCRIPTING
    options.python_home = [] { return android_python_home(); };
#endif
#else
#ifdef MIDGARD_HAS_SCRIPTING
    options.python_home = [] { return midgard::script::python_home_beside(executable_dir()); };
#endif
#endif
    const auto checks = midgard::selftest::run(options);
    say("%s", midgard::selftest::report(checks).c_str());
    if (std::getenv("GITHUB_ACTIONS"))  // in CI, failures show on the run's page as well
        for (const auto& c : checks)
            if (c.status == midgard::selftest::Status::Fail)
                std::printf("::error title=Self-test %s::%s\n", c.name.c_str(), c.detail.c_str());
    const int result = midgard::selftest::passed(checks) ? 0 : 1;
    if (!opt.show) return result;
#ifdef __ANDROID__
    opt.fixed_overlay = midgard::selftest::overlay(checks, true);
#else
    opt.fixed_overlay = midgard::selftest::overlay(checks, false);
#endif
    // (the results over a spinning cube, which shows rendering keeps going)
    std::unique_ptr<Source> demo = std::make_unique<DemoSource>();
    const int shown = run_window(opt, demo, {});
    return result != 0 ? result : shown;
}

int finish(const Options& opt, bool windowed, int result) {
    if (opt.profile) {
        midgard::profile::set_enabled(false);
        const auto zones = midgard::profile::take();
        if (midgard::profile::write_chrome_trace(*opt.profile, zones))
            say("Wrote %zu profiling zones to %s\n", zones.size(), opt.profile->c_str());
        else
            warn("Could not write the profile to %s\n", opt.profile->c_str());
    }
    if (windowed) SDL_Quit();
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    attach_parent_console();
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--version") {
            std::printf("Midgard runtime %s (engine ABI %d, package format %d.x, schema %d-%d)\n",
                        midgard::kVersion, midgard::kEngineAbi, midgard::kFormatMajor,
                        midgard::kSchemaMin, midgard::kSchemaMax);
            return 0;
        } else if (arg == "--check-python") {
            return check_python();
        } else if (arg == "--dump-api") {
            return dump_api();
        } else if (arg == "--simulate" && has_value) {
            opt.simulate = std::atol(argv[++i]);
        } else if (arg == "--dump-scene") {
            opt.dump_scene = true;
        } else if (arg == "--hold" && has_value) {  // KEY:FROM:TO (the key may have spaces)
            const std::string v = argv[++i];
            const size_t b = v.rfind(':'), a = b == std::string::npos ? b : v.rfind(':', b - 1);
            if (a == std::string::npos || a == 0) {
                fail("--hold wants KEY:FROM:TO, such as W:60:120");
                return 2;
            }
            opt.holds.push_back(
                {v.substr(0, a), std::atol(v.c_str() + a + 1), std::atol(v.c_str() + b + 1)});
        } else if (arg == "--record-audio" && has_value) {
            opt.record_audio = argv[++i];
        } else if (arg == "--headless") {
            opt.headless = true;
        } else if (arg == "--package" && has_value) {
            opt.package_dir = argv[++i];
        } else if (arg == "--screenshot" && has_value) {
            opt.screenshot = argv[++i];
        } else if (arg == "--editor-link" && has_value) {
            opt.editor_link = argv[++i];
        } else if (arg == "--frames" && has_value) {
            opt.frames = std::atol(argv[++i]);
        } else if (arg == "--size" && has_value) {
            if (!parse_size(argv[++i], opt.width, opt.height)) return usage();
        } else if (arg == "--debug-overlay") {
            opt.debug_overlay = true;
        } else if (arg == "--debug-physics") {
            opt.debug_physics = true;
        } else if (arg == "--overlay-text" && has_value) {
            opt.overlay_text = argv[++i];
        } else if (arg == "--profile" && has_value) {
            opt.profile = argv[++i];
        } else if (arg == "--player-profile" && has_value) {
            opt.player_profile = argv[++i];
        } else if (arg == "--world" && has_value) {
            opt.world = argv[++i];
        } else if (arg == "--scene" && has_value) {
            opt.scene = argv[++i];
        } else if (arg == "--quality" && has_value) {
            opt.quality = render::parse_quality(argv[++i]);
            if (!opt.quality) return usage();
        } else if (arg == "--exposure" && has_value) {
            opt.exposure = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--origin-offset" && has_value) {
            double x = 0, y = 0, z = 0;
            if (std::sscanf(argv[++i], "%lf,%lf,%lf", &x, &y, &z) != 3) return usage();
            opt.origin_offset = {x, y, z};
        } else if (arg == "--ui-test") {
            opt.ui_test = true;
        } else if (arg == "--hud-test") {
            opt.hud_test = true;
        } else if (arg == "--show-hud") {
            opt.show_hud = true;
        } else if (arg == "--touch-controls") {
            opt.touch_controls = true;
        } else if (arg == "--screen-test" && has_value) {
            opt.screen_test = argv[++i];
        } else if (arg == "--ui-touch") {
            opt.ui_touch = true;
        } else if (arg == "--ui-scale" && has_value) {
            opt.ui_scale = std::strtof(argv[++i], nullptr);
            if (!(opt.ui_scale >= 0.25f && opt.ui_scale <= 4.0f)) return usage();
        } else if (arg == "--safe-area" && has_value) {
            midgard::ui::Insets insets;
            if (!parse_insets(argv[++i], insets)) return usage();
            opt.safe_area = insets;
        } else if (arg == "--selftest") {
            opt.selftest = true;
        } else if (arg == "--show") {
            opt.show = true;
        } else {
            return usage();
        }
    }
    // SDL starts first: on Android it also reads the package from the APK.
    const bool windowed =
        !opt.screenshot && opt.simulate < 0 && !opt.headless && (!opt.selftest || opt.show);
    if (windowed && !SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fail(std::string("SDL_Init failed: ") + SDL_GetError());
        return 1;
    }
    int result = 0;
    if (opt.profile) midgard::profile::set_enabled(true);
    try {
#ifdef __ANDROID__
        // The self-test APK is a game APK with this marker among its assets.
        size_t marker_size = 0;
        if (void* marker = SDL_LoadFile("selftest.txt", &marker_size)) {
            SDL_free(marker);
            opt.selftest = opt.show = true;
        }
        if (!opt.package_dir) opt.package_dir = package_from_assets();
#endif
        if (!opt.package_dir) opt.package_dir = package_beside_executable();
        if (opt.selftest) return finish(opt, windowed, run_selftest(opt));
        std::unique_ptr<Source> source;
        std::function<std::unique_ptr<Source>()> load;
        if (opt.scene) {
            auto scene = midgard::test_scenes::make(*opt.scene);
            if (!scene) {
                std::string known;
                for (const std::string& n : midgard::test_scenes::names()) known += " " + n;
                fail("no test scene called '" + *opt.scene + "' (there are:" + known + ")");
                return finish(opt, windowed, 2);
            }
            source = std::make_unique<TestSceneSource>(std::move(scene));
        } else if (opt.package_dir) {
            // (In a window the game loads behind its splash; otherwise now.)
            load = [&opt, windowed]() -> std::unique_ptr<Source> {
                auto game = std::make_unique<GameSource>(*opt.package_dir);
                if (const auto folder = player_profile_dir(opt, windowed, game->title()))
                    game->use_profile(*folder / "profile.json");
                if (const auto folder = world_dir(opt, windowed, game->title()))
                    game->use_world(*folder);
                return game;
            };
            const bool splash = opt.simulate < 0 && !opt.headless && !opt.screenshot;
            if (!splash) source = load();
        } else {
            source = std::make_unique<DemoSource>(opt.origin_offset);
        }
        if (opt.simulate >= 0)
            result = run_simulation(opt, *source);
        else if (opt.headless)
            result = run_headless(opt, *source);
        else
            result = opt.screenshot ? run_screenshot(opt, *source) : run_window(opt, source, load);
    } catch (const std::exception& e) {
        fail(e.what());
        result = 1;
    }
    return finish(opt, windowed, result);
}
