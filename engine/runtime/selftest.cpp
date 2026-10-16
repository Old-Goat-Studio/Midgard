// SPDX-License-Identifier: Apache-2.0
#include "selftest.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <exception>
#include <numeric>

#include <glm/gtc/matrix_transform.hpp>

#include "midgard/body.h"
#include "midgard/det.h"
#include "midgard/hash.h"
#include "midgard/jobs.h"
#include "midgard/package.h"
#include "midgard/physics.h"
#include "midgard/render/mesh_data.h"
#include "midgard/render/renderer.h"
#include "midgard/shape.h"
#include "midgard/version.h"
#include "test_scenes.h"
#ifdef MIDGARD_HAS_SCRIPTING
#include "midgard/script.h"
#endif

namespace midgard::selftest {

namespace {

// The FNV-1a hash of the unit cube's mesh data. It's the same on every target, so a different
// value means data isn't identical here (engine/tests/test_hash.cpp checks the same value).
constexpr std::uint64_t kCubeHash = 0x48cc279e660d43e5ULL;

std::string format(const char* fmt, auto... args) {
    char text[256];
    std::snprintf(text, sizeof(text), fmt, args...);
    return text;
}

Check jobs_check() {
    JobSystem jobs;
    constexpr std::size_t count = 200'000;
    std::vector<std::uint64_t> values(count);
    const auto start = std::chrono::steady_clock::now();
    jobs.parallel_for(count, [&](std::size_t i) { values[i] = i * i; });
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const std::uint64_t sum = std::accumulate(values.begin(), values.end(), std::uint64_t{0});
    const std::uint64_t expected = (count - 1) * count * (2 * count - 1) / 6;
    if (sum != expected) return {"Job system", Status::Fail, "a parallel sum came out wrong"};
    return {"Job system", Status::Pass,
            format("%u workers, %.2f ms for %zu items", jobs.worker_count(), ms, count)};
}

Check data_check() {
    const render::MeshData cube = render::make_cube();
    Fnv1a64 hash;
    hash.add(cube.vertices.data(), cube.vertices.size() * sizeof(render::Vertex));
    hash.add(cube.indices.data(), cube.indices.size() * sizeof(std::uint32_t));
    const std::uint64_t got = hash.value();
    if (got != kCubeHash)
        return {"Identical data", Status::Fail,
                format("hash %016" PRIx64 ", expected %016" PRIx64, got, kCubeHash)};
    return {"Identical data", Status::Pass, format("hash %016" PRIx64, got)};
}

Check maths_check() {
    if (!det::strict_floating_point())
        return {"Identical maths", Status::Fail,
                "this build fuses multiply-adds (built without strict floating point)"};
    const std::uint64_t got = det::fingerprint();
    if (got != det::kFingerprint)
        return {"Identical maths", Status::Fail,
                format("fingerprint %016" PRIx64 ", expected %016" PRIx64, got, det::kFingerprint)};
    return {"Identical maths", Status::Pass, format("noise, sin, cos, random: %016" PRIx64, got)};
}

Check shapes_check() {
    const std::uint64_t got = shape_fingerprint();
    if (got != kShapeFingerprint)
        return {"World shapes", Status::Fail,
                format("fingerprint %016" PRIx64 ", expected %016" PRIx64, got, kShapeFingerprint)};
    return {"World shapes", Status::Pass,
            format("planets, hollow worlds, cylinders: %016" PRIx64, got)};
}

// A missing Vulkan driver: skipped on PCs (CI machines have none), a failure on phones.
Check no_vulkan(const char* name, const std::exception& e, bool vulkan_required) {
    const std::string what = e.what();
    const bool no_driver = what.find("Loading the Vulkan library") != std::string::npos ||
                           what.find("No Vulkan device") != std::string::npos;
    return {name, no_driver && !vulkan_required ? Status::Skip : Status::Fail, what};
}

Check render_check(bool vulkan_required) {
    try {
        render::RendererDesc desc;
        desc.offscreen = true;
        desc.width = 64;
        desc.height = 64;
        render::Renderer renderer(desc);
        const render::MeshId cube = renderer.upload_mesh(render::make_cube());
        render::Camera camera;
        camera.view = glm::lookAt(glm::vec3(0, 0, 3), glm::vec3(0), glm::vec3(0, 1, 0));
        camera.projection = render::perspective(glm::radians(50.0f), 1.0f, 0.1f, 10.0f);
        // Colours below the tone mapper's roll-off (0.76 linear), so they come out as given.
        const render::DrawItem item{cube, glm::mat4(1.0f), glm::vec4(0.8f, 0, 0, 1)};
        renderer.render_frame(camera, render::Light{}, {&item, 1}, glm::vec4(0, 0, 0.8f, 1));
        const std::vector<uint8_t> pixels = renderer.read_pixels();
        const uint8_t* centre = &pixels[(32 * 64 + 32) * 4];
        const uint8_t* corner = &pixels[0];
        // A red cube (with a little of the bluish ambient light) on a blue background.
        if (centre[0] < 100 || centre[2] > 60 || corner[2] < 180 || corner[0] > 30)
            return {"Rendering", Status::Fail,
                    format("wrong colours: centre %d,%d,%d corner %d,%d,%d", centre[0], centre[1],
                           centre[2], corner[0], corner[1], corner[2])};
        return {"Rendering", Status::Pass,
                renderer.device_name() + ", textures " +
                    render::gpu_format_name(renderer.texture_format())};
    } catch (const std::exception& e) {
        return no_vulkan("Rendering", e, vulkan_required);
    }
}

// Frames must keep up with 60 a second at this device's quality tier: the sunset test scene
// (shadows, sky and fog) at 1280 x 720, the median of 30 frames after 5 to warm up. Software
// renderers (CI's lavapipe) are far slower than any GPU, so they're timed but not held to it.
constexpr double kFrameBudgetMs = 1000.0 / 60.0;

Check frame_check(const Options& options) {
    const render::Quality tier = options.quality.value_or(render::default_quality());
    try {
        render::RendererDesc desc;
        desc.offscreen = true;
        desc.width = 1280;
        desc.height = 720;
        desc.quality = render::quality_settings(tier);
        render::Renderer renderer(desc);
        const auto scene = test_scenes::make("sky_sunset");
        scene->upload(renderer);
        renderer.set_environment(scene->environment());
        const render::Camera camera = scene->camera(1280.0f / 720.0f);
        const auto draws = scene->draws(camera.origin);
        const render::Light light = scene->light(camera.origin);
        std::vector<double> times;
        for (int i = 0; i < 35; ++i) {
            const auto start = std::chrono::steady_clock::now();
            renderer.render_frame(camera, light, draws, scene->sky());  // waits for the GPU
            const auto end = std::chrono::steady_clock::now();
            if (i >= 5)
                times.push_back(std::chrono::duration<double, std::milli>(end - start).count());
        }
        std::sort(times.begin(), times.end());
        const double median = times[times.size() / 2];
        const std::string detail = format("%.1f ms a frame (budget %.1f), %s quality, 1280 x 720",
                                          median, kFrameBudgetMs, render::quality_name(tier));
        if (renderer.software())
            return {"Frame time", Status::Skip, detail + ", software renderer: not checked"};
        return {"Frame time", median <= kFrameBudgetMs ? Status::Pass : Status::Fail, detail};
    } catch (const std::exception& e) {
        return no_vulkan("Frame time", e, options.vulkan_required);
    }
}

// Jolt on this device: a box dropped on a 1 km planet lands on its surface, and a character
// walks 20 m round it without falling or sinking.
Check physics_check() {
    try {
        const auto start = std::chrono::steady_clock::now();
        Body planet{"planet", Shape::sphere_outside(1000.0, 9.81), 0, glm::dvec3(0.0)};
        physics::World world(planet);
        physics::BodyDesc ground;
        ground.collider.kind = physics::Collider::Kind::Sphere;
        ground.collider.radius = 1000.0f;
        world.add(ground);
        physics::BodyDesc box;
        box.motion = physics::Motion::Dynamic;
        box.position = {0.0, 0.0, 1003.0};
        const physics::BodyId crate = world.add(box);
        physics::CharacterDesc walker;
        walker.position = {1000.5, 0.0, 0.0};
        physics::Character character(world, walker);
        for (int i = 0; i < 180; ++i)  // three seconds
            world.step(physics::kStep, [&] {
                const glm::dvec3 up = planet.up(character.position());
                const glm::vec3 east(glm::normalize(glm::cross(up, glm::dvec3(0, 0, 1))));
                character.step(i < 60 ? glm::vec3(0.0f) : east * 10.0f);
            });
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        const double landed = glm::length(world.state(crate).position) - 1000.0;
        const double standing = glm::length(character.position()) - 1000.0;
        const double walked = glm::length(character.position() - glm::dvec3(1000.5, 0, 0));
        if (std::abs(landed - 0.5) > 0.05 || std::abs(standing - 0.5) > 0.05 ||
            std::abs(walked - 20.0) > 1.0 || !character.on_ground())
            return {"Physics", Status::Fail,
                    format("box at %.2f m, character at %.2f m after %.1f m", landed, standing,
                           walked)};
        return {"Physics", Status::Pass,
                format("a box lands and a character walks on a planet, %.0f ms", ms)};
    } catch (const std::exception& e) {
        return {"Physics", Status::Fail, e.what()};
    }
}

Check package_check(const std::optional<std::filesystem::path>& package) {
    if (!package) return {"Game package", Status::Skip, "none (the demo scene)"};
    try {
        const PackageInfo info = open_package(*package);
        return {"Game package", Status::Pass,
                info.manifest.game_name + " " + info.manifest.game_version + ", schema " +
                    std::to_string(info.database_schema_version)};
    } catch (const std::exception& e) {
        return {"Game package", Status::Fail, e.what()};
    }
}

Check python_check([[maybe_unused]] const Options& options) {
#ifdef MIDGARD_HAS_SCRIPTING
    try {
        if (!options.python_home) return {"Python", Status::Skip, "no Python home"};
        script::Interpreter python(options.python_home());
        python.exec(
            "import json, math, random, collections, dataclasses\n"
            "assert json.loads(json.dumps({'a': [1, 2]})) == {'a': [1, 2]}\n"
            "assert math.isclose(math.fsum([0.1] * 10), 1.0)\n");
        return {"Python", Status::Pass, "Python " + python.version()};
    } catch (const std::exception& e) {
        return {"Python", Status::Fail, e.what()};
    }
#else
    return {"Python", Status::Skip, "this runtime has no scripting"};
#endif
}

const char* label(Status s) {
    return s == Status::Pass ? "ok  " : s == Status::Fail ? "FAIL" : "skip";
}

}  // namespace

std::vector<Check> run(const Options& options) {
    std::vector<Check> checks;
    checks.push_back(
        {"Engine", Status::Pass,
         std::string("Midgard ") + kVersion + ", engine ABI " + std::to_string(kEngineAbi)});
    checks.push_back(jobs_check());
    checks.push_back(data_check());
    checks.push_back(maths_check());
    checks.push_back(shapes_check());
    checks.push_back(physics_check());
    checks.push_back(render_check(options.vulkan_required));
    checks.push_back(frame_check(options));
    checks.push_back(package_check(options.package));
    checks.push_back(python_check(options));
    return checks;
}

bool passed(const std::vector<Check>& checks) {
    return std::none_of(checks.begin(), checks.end(),
                        [](const Check& c) { return c.status == Status::Fail; });
}

std::vector<render::OverlayLine> overlay(const std::vector<Check>& checks, bool on_phone) {
    const auto failures = std::count_if(checks.begin(), checks.end(),
                                        [](const Check& c) { return c.status == Status::Fail; });
    // A phone's screen is small and dense: larger text there.
    const float size = on_phone ? 1.5f : 1.0f;
    std::vector<render::OverlayLine> lines;
    if (failures == 0)
        lines.push_back({"SELF-TEST PASSED", {0.3f, 1.0f, 0.3f, 1.0f}, 3.0f * size});
    else
        lines.push_back({format("SELF-TEST FAILED (%d)", int(failures)),
                         {1.0f, 0.3f, 0.3f, 1.0f},
                         3.0f * size});
    for (const Check& c : checks) {
        const glm::vec4 colour = c.status == Status::Pass   ? glm::vec4(0.85f, 1.0f, 0.85f, 1)
                                 : c.status == Status::Fail ? glm::vec4(1.0f, 0.5f, 0.5f, 1)
                                                            : glm::vec4(0.7f, 0.7f, 0.7f, 1);
        lines.push_back(
            {std::string(label(c.status)) + " " + c.name + ": " + c.detail, colour, size});
    }
    lines.push_back(
        {on_phone ? "The Back button closes this." : "Esc or closing the window ends it.",
         {0.7f, 0.7f, 0.7f, 1.0f},
         size});
    return lines;
}

std::string report(const std::vector<Check>& checks) {
    std::string out;
    for (const Check& c : checks)
        out += std::string(label(c.status)) + " " + c.name + ": " + c.detail + "\n";
    out += passed(checks) ? "Self-test passed\n" : "Self-test FAILED\n";
    return out;
}

}  // namespace midgard::selftest
