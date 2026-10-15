// SPDX-License-Identifier: Apache-2.0
// midgard_runtime: the game executable.
//
//   midgard_runtime --version
//   midgard_runtime [--package <dir>] [--frames N] [--size WxH]
//   midgard_runtime [--package <dir>] --screenshot out.png [--size WxH]   (offscreen, no window)
//
// With --package, the runtime shows the package's entry scene; without it, a built-in demo.
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "midgard/game.h"
#include "midgard/input.h"
#include "midgard/render/renderer.h"
#include "midgard/version.h"

namespace render = midgard::render;

namespace {

struct Options {
    std::optional<std::string> package_dir;
    std::optional<std::string> screenshot;
    long frames = -1;  // run until closed
    uint32_t width = 1280, height = 720;
};

int usage() {
    std::puts(
        "usage: midgard_runtime [--version] [--package <dir>] [--frames N] [--size WxH]\n"
        "                       [--screenshot out.png]");
    return 2;
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
        std::printf("%s %s: scene '%s', %zu entities, built by editor %s\n", m.game_name.c_str(),
                    m.game_version.c_str(), m.entry_scene.c_str(),
                    game_.scene().registry.view<midgard::EntityInfo>().size(),
                    m.editor_build.c_str());
    }
    void upload(render::Renderer& renderer) override { game_.upload_meshes(renderer); }
    std::vector<render::DrawItem> draws(float) const override { return game_.draws(); }
    render::Camera camera(float aspect) const override { return game_.camera(aspect); }
    render::Light light() const override { return game_.scene().light(); }
    glm::vec4 sky() const override { return {game_.scene().settings.sky, 1.0f}; }
    midgard::InputMap input_map() const override { return game_.input_map(); }
    void update(const midgard::Actions& actions, float dt) override { game_.update(actions, dt); }
    bool wants_mouse_look() const override { return game_.third_person().active(); }

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
    std::printf("Rendered %ux%u on %s to %s\n", opt.width, opt.height,
                renderer.device_name().c_str(), opt.screenshot->c_str());
    return 0;
}

int run_window(const Options& opt, Source& source) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "error: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window =
        SDL_CreateWindow("Midgard", int(opt.width), int(opt.height),
                         SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        std::fprintf(stderr, "error: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    int result = 0;
    try {
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
        std::printf("Rendering on %s\n", renderer.device_name().c_str());
        source.upload(renderer);
        midgard::Actions actions(source.input_map());
        if (source.wants_mouse_look()) SDL_SetWindowRelativeMouseMode(window, true);

        const auto start = std::chrono::steady_clock::now();
        auto last = start;
        bool running = true;
        long frame = 0;
        while (running && (opt.frames < 0 || frame < opt.frames)) {
            actions.begin_frame();
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT) running = false;
                feed(actions, e);
                if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                    renderer.resize(uint32_t(e.window.data1), uint32_t(e.window.data2));
            }
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
                SDL_Delay(50);
                continue;
            }
            // The menu action quits until in-game menus exist (Phase 6).
            if (actions.pressed("menu")) running = false;
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
            last = now;
            source.update(actions, dt);
            const float t = std::chrono::duration<float>(now - start).count();
            const auto draws = source.draws(t);
            const float aspect = float(renderer.width()) / float(renderer.height());
            if (renderer.render_frame(source.camera(aspect), source.light(), draws, source.sky()))
                ++frame;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        result = 1;
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}

}  // namespace

int main(int argc, char** argv) {
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
        } else if (arg == "--frames" && has_value) {
            opt.frames = std::atol(argv[++i]);
        } else if (arg == "--size" && has_value) {
            if (!parse_size(argv[++i], opt.width, opt.height)) return usage();
        } else {
            return usage();
        }
    }
    try {
        std::unique_ptr<Source> source;
        if (opt.package_dir)
            source = std::make_unique<GameSource>(*opt.package_dir);
        else
            source = std::make_unique<DemoSource>();
        return opt.screenshot ? run_screenshot(opt, *source) : run_window(opt, *source);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
