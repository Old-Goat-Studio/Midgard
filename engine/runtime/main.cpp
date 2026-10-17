// SPDX-License-Identifier: Apache-2.0
// midgard_runtime: the game executable.
//
//   midgard_runtime --version
//   midgard_runtime --check-python      start the game's Python and import the standard library
//   midgard_runtime --dump-api          the midgard script API's names, as JSON
//   midgard_runtime --package <dir> --simulate N [--dump-scene]
//                                       N frames of gameplay and scripts, no window (tests)
//   midgard_runtime --package <dir> --headless --editor-link host:port
//                                       play under the editor without a window or GPU (tests, CI)
//   midgard_runtime [--package <dir>] [--frames N] [--size WxH] [--editor-link host:port]
//   midgard_runtime [--package <dir>] --screenshot out.png [--size WxH] [--overlay-text "a|b"]
//                                       offscreen, no window (image tests); --origin-offset x,y,z
//                                       places the demo scene far from the world's origin;
//                                       --exposure brightens it (highlights roll off);
//                                       --scene <name> draws a built-in test scene instead
//                                       (runtime/test_scenes.h lists them)
//   --quality desktop|phone             the renderer's quality tier (by default phone on phones,
//                                       desktop elsewhere), for a window, a screenshot or the
//                                       self-test
//   midgard_runtime --selftest [--show]  checks this device can run games (jobs, identical data,
//                                       rendering and its frame time, the package, Python);
//                                       --show puts the results
//                                       on screen. On Android, an APK with a selftest.txt asset
//                                       starts this way (the self-test APK built in CI).
//   any of these with --profile trace.json   timed zones, for ui.perfetto.dev or chrome://tracing
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
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
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

#include "midgard/game.h"
#include "midgard/input.h"
#include "midgard/link.h"
#include "midgard/manifest.h"
#include "midgard/profile.h"
#include "midgard/render/renderer.h"
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
    bool headless = false;  // the game loop and live link, without a window or GPU
    long frames = -1;       // run until closed
    uint32_t width = 1280, height = 720;
    bool debug_overlay = false;
    bool debug_physics = false;               // collider outlines (F4)
    std::optional<std::string> overlay_text;  // fixed overlay lines, separated by '|'
    std::optional<std::string> profile;       // where to write the profiling trace
    bool selftest = false;
    bool show = false;              // the self-test's results in a window
    glm::dvec3 origin_offset{0.0};  // where the demo scene is placed (precision tests)
    float exposure = 1.0f;
    std::optional<std::string> scene;        // a built-in test scene
    std::optional<render::Quality> quality;  // the renderer's tier (default: the device's)
    // Overlay lines shown for the whole run (the self-test's results); F3 is off then.
    std::vector<render::OverlayLine> fixed_overlay;
};

int usage() {
    std::puts(
        "usage: midgard_runtime [--version] [--package <dir>] [--frames N] [--size WxH]\n"
        "                       [--screenshot out.png [--overlay-text \"a|b\"]]\n"
        "                       [--editor-link host:port] [--debug-overlay] [--debug-physics]\n"
        "                       [--profile out.json]\n"
        "                       [--quality desktop|phone]\n"
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
    // What to draw, relative to the camera's origin (camera-relative rendering).
    virtual std::vector<render::DrawItem> draws(float seconds, const glm::dvec3& origin) const = 0;
    virtual render::Camera camera(float aspect) const = 0;
    // The lighting, with local lights relative to the camera's origin.
    virtual render::Light light(const glm::dvec3& origin) const = 0;
    virtual glm::vec4 sky() const = 0;
    // The sky and fog (by default: a flat sky in sky()'s colour, no fog).
    virtual render::Environment environment() const { return {}; }
    virtual midgard::InputMap input_map() const { return midgard::InputMap::defaults(); }
    virtual void update(const midgard::Actions&, float) {}
    virtual bool wants_mouse_look() const { return false; }
    virtual std::string title() const { return "Midgard"; }
    // The game's icon (icon.png in its package), for the window and the taskbar.
    virtual std::optional<std::filesystem::path> icon() const { return std::nullopt; }
    // Collider outlines for debug drawing (F4), relative to the camera's origin.
    virtual std::vector<render::DebugLine> debug_lines(const glm::dvec3&) const { return {}; }
    // The scene as JSON (entities and their transforms and colours), for tests.
    virtual std::string dump() const { return "{}"; }
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
    std::vector<render::DrawItem> draws(float, const glm::dvec3& origin) const override {
        return scene_->draws(origin);
    }
    render::Camera camera(float aspect) const override { return scene_->camera(aspect); }
    render::Light light(const glm::dvec3& origin) const override { return scene_->light(origin); }
    glm::vec4 sky() const override { return scene_->sky(); }
    render::Environment environment() const override { return scene_->environment(); }
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
    }
    void upload(render::Renderer& renderer) override { game_.upload_meshes(renderer); }
    std::vector<render::DrawItem> draws(float, const glm::dvec3& origin) const override {
        return game_.draws(origin);
    }
    render::Camera camera(float aspect) const override { return game_.camera(aspect); }
    render::Light light(const glm::dvec3& origin) const override {
        return game_.scene().light(origin);
    }
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
    }

    std::string dump() const override {
        nlohmann::json entities = nlohmann::json::array();
        const auto& reg = game_.scene().registry;
        for (entt::entity e : reg.view<const midgard::EntityInfo>()) {
            const auto& t = reg.get<midgard::Transform>(e);
            const midgard::Material* m = reg.try_get<midgard::Material>(e);
            entities.push_back(
                {{"id", reg.get<midgard::EntityInfo>(e).id},
                 {"name", reg.get<midgard::EntityInfo>(e).name},
                 {"position", {t.position.x, t.position.y, t.position.z}},
                 {"rotation", {t.rotation.x, t.rotation.y, t.rotation.z}},
                 {"scale", {t.scale.x, t.scale.y, t.scale.z}},
                 {"color", m ? nlohmann::json{m->color.r, m->color.g, m->color.b, m->color.a}
                             : nlohmann::json(nullptr)}});
        }
        const glm::vec3 sky = game_.scene().settings.sky;
        return nlohmann::json{{"entities", entities}, {"sky", {sky.x, sky.y, sky.z}}}.dump();
    }
    bool wants_mouse_look() const override { return game_.third_person().active(); }
    std::string title() const override { return game_.package().manifest.game_name; }
    std::optional<std::filesystem::path> icon() const override {
        const auto file = game_.package().root / "icon.png";
        if (std::filesystem::is_regular_file(file)) return file;
        return std::nullopt;
    }

    void apply(const nlohmann::json& message, render::Renderer* renderer) override {
        const std::string type = message.value("type", "");
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
            actions.add_delta("mouse", "delta_x", e.motion.xrel);
            actions.add_delta("mouse", "delta_y", e.motion.yrel);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            actions.add_delta("mouse", "wheel", e.wheel.y);
            break;
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
    renderer.set_environment(source.environment());
    const render::Camera camera = source.camera(float(opt.width) / float(opt.height));
    const auto draws = source.draws(0.6f, camera.origin);
    if (opt.debug_physics) renderer.set_debug_lines(source.debug_lines(camera.origin));
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

int run_window(const Options& opt, Source& source) {
    SDL_WindowFlags flags =
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#ifdef __ANDROID__
    flags |= SDL_WINDOW_FULLSCREEN;
#endif
    SDL_Window* window =
        SDL_CreateWindow(source.title().c_str(), int(opt.width), int(opt.height), flags);
    if (!window) {
        fail(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
        return 1;
    }
    if (const auto icon = source.icon()) set_window_icon(window, *icon);
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
        source.upload(renderer);
        midgard::Actions actions(source.input_map());
        // Mouse-look captures the mouse. Under the editor the game starts with the mouse free
        // (so the editor's panels stay reachable): a click in the game captures it, and Esc,
        // Shift+F1 or switching away frees it. An exported game captures it from the start.
        bool captured = false;
        const auto capture = [&](bool on) {
            on = on && source.wants_mouse_look();
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
        capture(!link);

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
            // The menu action quits until in-game menus exist (Phase 6).
            if (actions.pressed("menu")) running = false;
            MIDGARD_PROFILE_SCOPE("frame");
            const auto now = std::chrono::steady_clock::now();
            const double real_dt = std::chrono::duration<double>(now - last).count();
            const float dt = std::min(float(real_dt), 0.1f);
            last = now;
            if (!paused) {
                MIDGARD_PROFILE_SCOPE("update");
                source.update(actions, dt);
            }
            const float t = std::chrono::duration<float>(now - start).count();
            const float aspect = float(renderer.width()) / float(renderer.height());
            const render::Camera camera = source.camera(aspect);
            const auto draws = source.draws(t, camera.origin);
            bool drawn = false;
            {
                MIDGARD_PROFILE_SCOPE("render");
                renderer.set_environment(source.environment());
                renderer.set_debug_lines(show_physics ? source.debug_lines(camera.origin)
                                                      : std::vector<render::DebugLine>{});
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
                    renderer.set_overlay(
                        {{std::string("Midgard ") + midgard::kVersion},
                         {timing, {1.0f, 1.0f, 0.6f, 1.0f}},
                         {std::to_string(draw_count) + " draws"},
                         {renderer.device_name()},
                         {"F3 hides this, F4 outlines physics", {0.7f, 0.7f, 0.7f, 1.0f}}});
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

// Gameplay and scripts for a number of frames at 60 per second, with no window or input.
int run_simulation(const Options& opt, Source& source) {
    midgard::Actions actions(source.input_map());
    for (long frame = 0; frame < opt.simulate; ++frame) {
        actions.begin_frame();
        source.update(actions, 1.0f / 60.0f);
    }
    if (opt.dump_scene) std::printf("%s\n", source.dump().c_str());
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
    DemoSource demo;  // the results over a spinning cube, which shows rendering keeps going
    const int shown = run_window(opt, demo);
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
        if (opt.scene) {
            auto scene = midgard::test_scenes::make(*opt.scene);
            if (!scene) {
                std::string known;
                for (const std::string& n : midgard::test_scenes::names()) known += " " + n;
                fail("no test scene called '" + *opt.scene + "' (there are:" + known + ")");
                return finish(opt, windowed, 2);
            }
            source = std::make_unique<TestSceneSource>(std::move(scene));
        } else if (opt.package_dir)
            source = std::make_unique<GameSource>(*opt.package_dir);
        else
            source = std::make_unique<DemoSource>(opt.origin_offset);
        if (opt.simulate >= 0)
            result = run_simulation(opt, *source);
        else if (opt.headless)
            result = run_headless(opt, *source);
        else
            result = opt.screenshot ? run_screenshot(opt, *source) : run_window(opt, *source);
    } catch (const std::exception& e) {
        fail(e.what());
        result = 1;
    }
    return finish(opt, windowed, result);
}
