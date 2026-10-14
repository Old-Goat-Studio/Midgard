// SPDX-License-Identifier: Apache-2.0
// midgard_runtime: the game executable.
//
//   midgard_runtime --version
//   midgard_runtime [--package <dir>] [--frames N] [--size WxH]
//   midgard_runtime [--package <dir>] --screenshot out.png [--size WxH]   (offscreen, no window)
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "midgard/package.h"
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

// The built-in demo scene, used until a package's own scene is loaded (Phase 2, task 2).
struct Demo {
    render::MeshId cube = 0;

    std::vector<render::DrawItem> draws(float seconds) const {
        glm::mat4 spin = glm::rotate(glm::mat4(1.0f), seconds * 0.6f, glm::vec3(0, 1, 0));
        glm::mat4 cube_model = glm::translate(glm::mat4(1.0f), glm::vec3(0, 0.5f, 0)) * spin;
        glm::mat4 ground = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0, -0.05f, 0)),
                                      glm::vec3(6.0f, 0.1f, 6.0f));
        return {
            {cube, cube_model, glm::vec4(0.79f, 0.64f, 0.15f, 1.0f)},  // knotwork gold
            {cube, ground, glm::vec4(0.22f, 0.32f, 0.18f, 1.0f)},      // moss
        };
    }
};

render::Camera demo_camera(uint32_t width, uint32_t height) {
    render::Camera camera;
    camera.view =
        glm::lookAt(glm::vec3(2.6f, 2.0f, 3.4f), glm::vec3(0, 0.4f, 0), glm::vec3(0, 1, 0));
    camera.projection =
        render::perspective(glm::radians(55.0f), float(width) / float(height), 0.05f, 200.0f);
    return camera;
}

const glm::vec4 kSky(0.08f, 0.11f, 0.18f, 1.0f);  // night blue

void describe_package(const std::string& dir) {
    const midgard::PackageInfo info = midgard::open_package(dir);
    const midgard::Manifest& m = info.manifest;
    std::printf("%s %s: scene '%s', schema %d, %zu content file(s), built by editor %s\n",
                m.game_name.c_str(), m.game_version.c_str(), m.entry_scene.c_str(),
                info.database_schema_version, m.content.size(), m.editor_build.c_str());
}

int run_screenshot(const Options& opt) {
    render::RendererDesc desc;
    desc.offscreen = true;
    desc.width = opt.width;
    desc.height = opt.height;
    desc.validation = std::getenv("MIDGARD_VULKAN_VALIDATION") != nullptr;
    render::Renderer renderer(desc);
    Demo demo{renderer.upload_mesh(render::make_cube())};
    const auto draws = demo.draws(0.6f);
    renderer.render_frame(demo_camera(opt.width, opt.height), render::Light{}, draws, kSky);
    if (!render::write_png(*opt.screenshot, opt.width, opt.height, renderer.read_pixels())) {
        std::fprintf(stderr, "error: could not write %s\n", opt.screenshot->c_str());
        return 1;
    }
    std::printf("Rendered %ux%u on %s to %s\n", opt.width, opt.height,
                renderer.device_name().c_str(), opt.screenshot->c_str());
    return 0;
}

int run_window(const Options& opt) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
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
        desc.validation = std::getenv("MIDGARD_VULKAN_VALIDATION") != nullptr;
        render::Renderer renderer(desc);
        std::printf("Rendering on %s\n", renderer.device_name().c_str());
        Demo demo{renderer.upload_mesh(render::make_cube())};

        const auto start = std::chrono::steady_clock::now();
        bool running = true;
        long frame = 0;
        while (running && (opt.frames < 0 || frame < opt.frames)) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT) running = false;
                // Esc to quit is temporary: the input actions layer (task 3) replaces raw keys.
                if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) running = false;
                if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                    renderer.resize(uint32_t(e.window.data1), uint32_t(e.window.data2));
            }
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
                SDL_Delay(50);
                continue;
            }
            const float t =
                std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
            const auto draws = demo.draws(t);
            if (renderer.render_frame(demo_camera(renderer.width(), renderer.height()),
                                      render::Light{}, draws, kSky))
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
        if (opt.package_dir) describe_package(*opt.package_dir);
        return opt.screenshot ? run_screenshot(opt) : run_window(opt);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
