// SPDX-License-Identifier: Apache-2.0
// midgard_runtime: the game executable.
//
//   midgard_runtime --version
//   midgard_runtime [--package <dir>] [--frames N] [--size WxH] [--editor-link host:port]
//   midgard_runtime [--package <dir>] --screenshot out.png [--size WxH]   (offscreen, no window)
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
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include "midgard/game.h"
#include "midgard/input.h"
#include "midgard/link.h"
#include "midgard/manifest.h"
#include "midgard/render/renderer.h"
#include "midgard/version.h"

namespace render = midgard::render;

namespace {

struct Options {
    std::optional<std::string> package_dir;
    std::optional<std::string> screenshot;
    std::optional<std::string> editor_link;
    long frames = -1;  // run until closed
    uint32_t width = 1280, height = 720;
};

int usage() {
    std::puts(
        "usage: midgard_runtime [--version] [--package <dir>] [--frames N] [--size WxH]\n"
        "                       [--screenshot out.png] [--editor-link host:port]");
    return 2;
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
    virtual std::vector<render::DrawItem> draws(float seconds) const = 0;
    virtual render::Camera camera(float aspect) const = 0;
    virtual render::Light light() const = 0;
    virtual glm::vec4 sky() const = 0;
    virtual midgard::InputMap input_map() const { return midgard::InputMap::defaults(); }
    virtual void update(const midgard::Actions&, float) {}
    virtual bool wants_mouse_look() const { return false; }
    virtual std::string title() const { return "Midgard"; }
    // A message from the editor's live link (entity edits and the like).
    virtual void apply(const nlohmann::json& message, render::Renderer&) {
        warn("live link: '%s' needs a game package\n", message.value("type", "").c_str());
    }
};

class DemoSource : public Source {
public:
    void upload(render::Renderer& renderer) override {
        cube_ = renderer.upload_mesh(render::make_cube());
    }
    std::vector<render::DrawItem> draws(float seconds) const override {
        glm::mat4 spin = glm::rotate(glm::mat4(1.0f), seconds * 0.6f, glm::vec3(0, 1, 0));
        glm::mat4 cube = glm::translate(glm::mat4(1.0f), glm::vec3(0, 0.5f, 0)) * spin;
        glm::mat4 ground = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0, -0.05f, 0)),
                                      glm::vec3(6.0f, 0.1f, 6.0f));
        return {
            {cube_, cube, glm::vec4(0.79f, 0.64f, 0.15f, 1.0f)},    // knotwork gold
            {cube_, ground, glm::vec4(0.22f, 0.32f, 0.18f, 1.0f)},  // moss
        };
    }
    render::Camera camera(float aspect) const override {
        render::Camera c;
        c.view =
            glm::lookAt(glm::vec3(2.6f, 2.0f, 3.4f), glm::vec3(0, 0.4f, 0), glm::vec3(0, 1, 0));
        c.projection = render::perspective(glm::radians(55.0f), aspect, 0.05f, 200.0f);
        return c;
    }
    render::Light light() const override { return {}; }
    glm::vec4 sky() const override { return {0.08f, 0.11f, 0.18f, 1.0f}; }  // night blue

private:
    render::MeshId cube_ = 0;
};

class GameSource : public Source {
public:
    explicit GameSource(const std::string& dir) : game_(dir) {
        const midgard::Manifest& m = game_.package().manifest;
        say("%s %s: scene '%s', %zu entities, built by editor %s\n", m.game_name.c_str(),
            m.game_version.c_str(), m.entry_scene.c_str(),
            game_.scene().registry.view<midgard::EntityInfo>().size(), m.editor_build.c_str());
    }
    void upload(render::Renderer& renderer) override { game_.upload_meshes(renderer); }
    std::vector<render::DrawItem> draws(float) const override { return game_.draws(); }
    render::Camera camera(float aspect) const override { return game_.camera(aspect); }
    render::Light light() const override { return game_.scene().light(); }
    glm::vec4 sky() const override { return {game_.scene().settings.sky, 1.0f}; }
    midgard::InputMap input_map() const override { return game_.input_map(); }
    void update(const midgard::Actions& actions, float dt) override { game_.update(actions, dt); }
    bool wants_mouse_look() const override { return game_.third_person().active(); }
    std::string title() const override { return game_.package().manifest.game_name; }

    void apply(const nlohmann::json& message, render::Renderer& renderer) override {
        const std::string type = message.value("type", "");
        try {
            if (type == "entity.put") {
                game_.put_entity(message.at("entity"));
            } else if (type == "entity.set") {
                game_.set_component(message.at("entity").get<std::string>(),
                                    message.at("component").get<std::string>(),
                                    message.at("value"));
            } else if (type == "entity.delete") {
                game_.remove_entity(message.at("entity").get<std::string>());
            } else {
                warn("live link: '%s' is not supported yet\n", type.c_str());
                return;
            }
        } catch (const std::exception& e) {  // bad data from the editor must not end the game
            warn("live link: %s: %s\n", type.c_str(), e.what());
            return;
        }
        for (const std::string& error : game_.upload_new_meshes(renderer))
            warn("%s\n", error.c_str());
        if (g_link) {  // lets the editor (and its tests) see when an edit took effect
            const nlohmann::json& entity = message.at("entity");
            g_link->send({{"type", "log"},
                          {"level", "debug"},
                          {"text", "applied " + type + " " +
                                       (entity.is_object() ? entity.value("id", "")
                                                           : entity.get<std::string>())}});
        }
    }

private:
    midgard::Game game_;
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
    const midgard::Manifest m = midgard::parse_manifest(manifest);
    const std::vector<std::string> files = {m.database, "game.pak", m.game_name + ".config"};
    for (const std::string& name : files) {
        void* bytes = SDL_LoadFile(("data/" + name).c_str(), &size);
        if (!bytes) continue;  // optional parts of a package
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
    render::Renderer renderer(desc);
    source.upload(renderer);
    const auto draws = source.draws(0.6f);
    renderer.render_frame(source.camera(float(opt.width) / float(opt.height)), source.light(),
                          draws, source.sky());
    if (!render::write_png(*opt.screenshot, opt.width, opt.height, renderer.read_pixels())) {
        std::fprintf(stderr, "error: could not write %s\n", opt.screenshot->c_str());
        return 1;
    }
    say("Rendered %ux%u on %s to %s\n", opt.width, opt.height, renderer.device_name().c_str(),
        opt.screenshot->c_str());
    return 0;
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
        render::Renderer renderer(desc);
        say("Rendering on %s\n", renderer.device_name().c_str());
        source.upload(renderer);
        midgard::Actions actions(source.input_map());
        if (source.wants_mouse_look()) SDL_SetWindowRelativeMouseMode(window, true);

        const auto start = std::chrono::steady_clock::now();
        auto last = start;
        bool running = true;
        bool background = false;  // Android: the app is not visible and has no surface
        bool paused = false;      // the editor's Pause: drawn, but no gameplay
        long frame = 0;
        long frames_this_second = 0;
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
                        source.apply(message, renderer);
                }
            }
            actions.begin_frame();
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
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
                        // Under the editor, its Pause (F6) and Stop (Shift+F5) keys also work
                        // while the game window has the keyboard.
                        if (link && !e.key.repeat) {
                            if (e.key.scancode == SDL_SCANCODE_F5 && (e.key.mod & SDL_KMOD_SHIFT)) {
                                running = false;
                            } else if (e.key.scancode == SDL_SCANCODE_F6) {
                                paused = !paused;
                                link->send(
                                    {{"type", "state"}, {"state", paused ? "paused" : "playing"}});
                            }
                        }
                        break;
                    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                        renderer.resize(uint32_t(e.window.data1), uint32_t(e.window.data2));
                        break;
                    default:
                        break;
                }
                feed(actions, e);
            }
            if (background || (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) {
                SDL_Delay(50);
                continue;
            }
            // The menu action quits until in-game menus exist (Phase 6).
            if (actions.pressed("menu")) running = false;
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
            last = now;
            if (!paused) source.update(actions, dt);
            const float t = std::chrono::duration<float>(now - start).count();
            const auto draws = source.draws(t);
            const float aspect = float(renderer.width()) / float(renderer.height());
            if (renderer.render_frame(source.camera(aspect), source.light(), draws, source.sky())) {
                ++frame;
                ++frames_this_second;
                draw_count = draws.size();
            }
            if (link && now >= stats_due) {
                link->send({{"type", "stats"},
                            {"fps", frames_this_second},
                            {"draw_calls", draw_count},
                            {"paused", paused}});
                frames_this_second = 0;
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
        } else {
            return usage();
        }
    }
    // SDL starts first: on Android it also reads the package from the APK.
    if (!opt.screenshot && !SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fail(std::string("SDL_Init failed: ") + SDL_GetError());
        return 1;
    }
    int result = 0;
    try {
#ifdef __ANDROID__
        if (!opt.package_dir) opt.package_dir = package_from_assets();
#endif
        if (!opt.package_dir) opt.package_dir = package_beside_executable();
        std::unique_ptr<Source> source;
        if (opt.package_dir)
            source = std::make_unique<GameSource>(*opt.package_dir);
        else
            source = std::make_unique<DemoSource>();
        result = opt.screenshot ? run_screenshot(opt, *source) : run_window(opt, *source);
    } catch (const std::exception& e) {
        fail(e.what());
        result = 1;
    }
    if (!opt.screenshot) SDL_Quit();
    return result;
}
