// SPDX-License-Identifier: Apache-2.0
#include "selftest.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <exception>
#include <numeric>

#include <glm/gtc/matrix_transform.hpp>

#include "midgard/det.h"
#include "midgard/hash.h"
#include "midgard/jobs.h"
#include "midgard/package.h"
#include "midgard/render/mesh_data.h"
#include "midgard/render/renderer.h"
#include "midgard/shape.h"
#include "midgard/version.h"
#ifdef MIDGARD_HAS_SCRIPTING
#include "midgard/script.h"
#endif

namespace midgard::selftest {

namespace {

// The FNV-1a hash of the unit cube's mesh data. It's the same on every target, so a different
// value means data isn't identical here (engine/tests/test_hash.cpp checks the same value).
constexpr std::uint64_t kCubeHash = 0xce0a694172de14a5ULL;

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
        const render::DrawItem item{cube, glm::mat4(1.0f), glm::vec4(1, 0, 0, 1)};
        renderer.render_frame(camera, render::Light{}, {&item, 1}, glm::vec4(0, 0, 1, 1));
        const std::vector<uint8_t> pixels = renderer.read_pixels();
        const uint8_t* centre = &pixels[(32 * 64 + 32) * 4];
        const uint8_t* corner = &pixels[0];
        // A red cube in the middle of a blue background.
        if (centre[0] < 100 || centre[2] > 60 || corner[2] < 200 || corner[0] > 30)
            return {"Rendering", Status::Fail,
                    format("wrong colours: centre %d,%d,%d corner %d,%d,%d", centre[0], centre[1],
                           centre[2], corner[0], corner[1], corner[2])};
        return {"Rendering", Status::Pass, renderer.device_name()};
    } catch (const std::exception& e) {
        const std::string what = e.what();
        const bool no_driver = what.find("Loading the Vulkan library") != std::string::npos ||
                               what.find("No Vulkan device") != std::string::npos;
        return {"Rendering", no_driver && !vulkan_required ? Status::Skip : Status::Fail, what};
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
    checks.push_back(render_check(options.vulkan_required));
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
