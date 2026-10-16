// SPDX-License-Identifier: Apache-2.0
// midgard_bench: timings the engine must stay within (schema/budgets.json).
//
//   midgard_bench [--json results.json] [--filter text] [--list]
//
// Each benchmark runs a few times unmeasured, then is timed over many runs; the median and the
// 95th percentile go to the JSON file, which tools/budgets.py checks against the budgets.
// Benchmarks that need a GPU are skipped (and reported as skipped) when there's no Vulkan.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include "midgard/det.h"
#include "midgard/jobs.h"
#include "midgard/physics.h"
#include "midgard/render/ktx.h"
#include "midgard/render/overlay.h"
#include "midgard/render/renderer.h"
#include "midgard/shape.h"
#include "midgard/version.h"

namespace {

struct Benchmark {
    std::string name;
    std::string about;
    int iterations;
    // Prepares and returns the timed step, or nothing with a reason to skip it.
    std::function<std::function<void()>(std::string& skip_reason)> setup;
};

struct Result {
    std::string name;
    std::string about;
    bool skipped = false;
    std::string skip_reason;
    int iterations = 0;
    double median_ms = 0, p95_ms = 0, min_ms = 0;
};

Result measure(const Benchmark& b) {
    Result r{b.name, b.about};
    std::string reason;
    std::function<void()> step = b.setup(reason);
    if (!step) {
        r.skipped = true;
        r.skip_reason = reason;
        return r;
    }
    for (int i = 0; i < 3; ++i) step();  // warm caches and drivers up
    std::vector<double> ms;
    ms.reserve(size_t(b.iterations));
    for (int i = 0; i < b.iterations; ++i) {
        const auto start = std::chrono::steady_clock::now();
        step();
        ms.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count());
    }
    std::sort(ms.begin(), ms.end());
    r.iterations = b.iterations;
    r.median_ms = ms[ms.size() / 2];
    r.p95_ms = ms[std::min(ms.size() - 1, ms.size() * 95 / 100)];
    r.min_ms = ms.front();
    return r;
}

// Keeps the optimiser from removing work whose result is otherwise unused.
volatile std::uint64_t g_sink = 0;

std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return x;
}

std::vector<Benchmark> benchmarks() {
    std::vector<Benchmark> list;
    auto jobs = std::make_shared<midgard::JobSystem>();

    list.push_back({"jobs.parallel_for_1m", "1,000,000 small items split across the workers", 30,
                    [jobs](std::string&) -> std::function<void()> {
                        auto out = std::make_shared<std::vector<std::uint64_t>>(1'000'000);
                        return [jobs, out] {
                            jobs->parallel_for(out->size(),
                                               [&](std::size_t i) { (*out)[i] = mix(i); });
                            g_sink = g_sink + (*out)[12345];
                        };
                    }});
    list.push_back({"jobs.submit_wait_10k", "10,000 tiny jobs submitted and waited for", 30,
                    [jobs](std::string&) -> std::function<void()> {
                        return [jobs] {
                            std::vector<midgard::JobHandle> handles;
                            handles.reserve(10'000);
                            for (int i = 0; i < 10'000; ++i)
                                handles.push_back(jobs->submit(
                                    [](const midgard::JobContext&) { g_sink = g_sink + 1; }));
                            jobs->wait_all(handles);
                        };
                    }});
    list.push_back({"det.fbm_64x64_6_octaves",
                    "a 64 x 64 field of 6-octave deterministic noise (one thread)", 30,
                    [](std::string&) -> std::function<void()> {
                        return [] {
                            std::int64_t sum = 0;
                            for (int j = 0; j < 64; ++j)
                                for (int i = 0; i < 64; ++i)
                                    sum += midgard::det::fbm(7, i * 0.173, j * 0.173, 2.5, 6);
                            g_sink = g_sink + static_cast<std::uint64_t>(sum);
                        };
                    }});
    list.push_back({"shape.sphere_64x64_positions",
                    "positions and up for a 64 x 64 grid on all 6 faces of a planet", 30,
                    [](std::string&) -> std::function<void()> {
                        return [] {
                            const midgard::Shape planet =
                                midgard::Shape::sphere_outside(5000.0, 9.81);
                            double sum = 0.0;
                            for (int f = 0; f < 6; ++f)
                                for (int j = 0; j < 64; ++j)
                                    for (int i = 0; i < 64; ++i) {
                                        const glm::dvec3 p =
                                            planet.position({f, i / 64.0, j / 64.0, 1.0});
                                        sum += p.x + planet.up(p).y;
                                    }
                            g_sink = g_sink + static_cast<std::uint64_t>(sum);
                        };
                    }});
    list.push_back({"overlay.layout_40_lines", "the debug overlay's layout of 40 lines of text",
                    200, [](std::string&) -> std::function<void()> {
                        std::vector<midgard::render::OverlayLine> lines(
                            40, {"Chunk 12,-7 face 3: 4.21 ms generate, 0.85 ms upload"});
                        return [lines] {
                            g_sink =
                                g_sink + midgard::render::layout_overlay(lines, 1920, 1080).size();
                        };
                    }});
    // A frame at each quality tier: the desktop's (the agreed look) and the phone's.
    for (const auto& [name, tier] :
         {std::pair{"render.cubes_400_offscreen", midgard::render::Quality::Desktop},
          std::pair{"render.cubes_400_phone", midgard::render::Quality::Phone}}) {
        list.push_back(
            {name,
             std::string("400 lit cubes with sun shadows drawn offscreen at 640x360, ") +
                 midgard::render::quality_name(tier) + " quality (GPU)",
             60, [tier](std::string& skip) -> std::function<void()> {
                 using namespace midgard::render;
                 std::shared_ptr<Renderer> renderer;
                 try {
                     RendererDesc desc;
                     desc.offscreen = true;
                     desc.width = 640;
                     desc.height = 360;
                     desc.quality = quality_settings(tier);
                     renderer = std::make_shared<Renderer>(desc);
                 } catch (const std::exception& e) {
                     skip = std::string("no Vulkan: ") + e.what();
                     return {};
                 }
                 const MeshId cube = renderer->upload_mesh(make_cube());
                 auto draws = std::make_shared<std::vector<DrawItem>>();
                 for (int x = 0; x < 20; ++x)
                     for (int z = 0; z < 20; ++z)
                         draws->push_back({cube,
                                           glm::translate(glm::mat4(1.0f),
                                                          glm::vec3(x * 1.5f - 15, 0, z * -1.5f)),
                                           glm::vec4(0.8f, 0.6f, 0.2f, 1.0f)});
                 Camera camera;
                 camera.view =
                     glm::lookAt(glm::vec3(0, 12, 12), glm::vec3(0, 0, -12), glm::vec3(0, 1, 0));
                 camera.projection =
                     perspective(glm::radians(55.0f), 640.0f / 360.0f, 0.1f, 200.0f);
                 return [renderer, draws, camera] {
                     // Offscreen frames wait for the GPU, so this is the whole frame's time.
                     renderer->render_frame(camera, Light{}, *draws,
                                            glm::vec4(0.1f, 0.1f, 0.2f, 1));
                 };
             }});
    }
    list.push_back({"physics.step_500_boxes",
                    "one physics step with 500 boxes tumbling onto the ground and each other", 60,
                    [](std::string&) -> std::function<void()> {
                        using namespace midgard::physics;
                        auto world = std::make_shared<World>();
                        BodyDesc ground;
                        ground.collider.size = {100, 1, 100};
                        ground.position = {0, -0.5, 0};
                        world->add(ground);
                        for (int i = 0; i < 500; ++i) {
                            BodyDesc box;
                            box.motion = Motion::Dynamic;
                            box.position = {(i % 10) * 1.1 - 5.0, 0.5 + (i / 100) * 1.1,
                                            ((i / 10) % 10) * 1.1 - 5.0};
                            box.rotation =
                                glm::angleAxis(0.1f * float(i), glm::normalize(glm::vec3(1, 2, 3)));
                            world->add(box);
                        }
                        for (int i = 0; i < 30; ++i) world->step(kStep);  // falling and colliding
                        return [world] { world->step(kStep); };
                    }});
    // Loading a texture: a 512 x 512 KTX2 file (all its mip levels) transcoded to the format a
    // PC (BC7) or a phone (ASTC) reads.
    for (const auto& [name, format] :
         {std::pair{"texture.transcode_512_bc7", midgard::render::GpuFormat::BC7},
          std::pair{"texture.transcode_512_astc", midgard::render::GpuFormat::ASTC4x4}}) {
        list.push_back(
            {name,
             std::string("a 512 x 512 KTX2 texture with mipmaps, transcoded to ") +
                 midgard::render::gpu_format_name(format),
             20, [format](std::string&) -> std::function<void()> {
                 std::vector<uint8_t> rgba(512 * 512 * 4);
                 for (uint32_t i = 0; i < rgba.size(); ++i)
                     rgba[i] = static_cast<uint8_t>((i * 2654435761u) >> 24);  // busy detail
                 auto ktx2 = std::make_shared<std::vector<uint8_t>>(midgard::render::encode_ktx2(
                     midgard::render::make_texture(512, 512, true, std::move(rgba))));
                 return [ktx2, format] {
                     g_sink = g_sink + midgard::render::transcode_ktx2(*ktx2, format).levels.size();
                 };
             }});
    }
    return list;
}

}  // namespace

int main(int argc, char** argv) {
    std::optional<std::string> json_path;
    std::string filter;
    bool list_only = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
            json_path = argv[++i];
        } else if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
            filter = argv[++i];
        } else if (std::strcmp(argv[i], "--list") == 0) {
            list_only = true;
        } else {
            std::puts("usage: midgard_bench [--json results.json] [--filter text] [--list]");
            return 2;
        }
    }
    nlohmann::json results = nlohmann::json::object();
    for (const Benchmark& b : benchmarks()) {
        if (!filter.empty() && b.name.find(filter) == std::string::npos) continue;
        if (list_only) {
            std::printf("%-30s %s\n", b.name.c_str(), b.about.c_str());
            continue;
        }
        const Result r = measure(b);
        if (r.skipped) {
            std::printf("%-30s skipped (%s)\n", r.name.c_str(), r.skip_reason.c_str());
            results[r.name] = {{"skipped", r.skip_reason}, {"about", r.about}};
        } else {
            std::printf("%-30s median %8.3f ms   p95 %8.3f ms   (%d runs)\n", r.name.c_str(),
                        r.median_ms, r.p95_ms, r.iterations);
            results[r.name] = {{"median_ms", r.median_ms},
                               {"p95_ms", r.p95_ms},
                               {"min_ms", r.min_ms},
                               {"iterations", r.iterations},
                               {"about", r.about}};
        }
        std::fflush(stdout);
    }
    if (json_path && !list_only) {
        std::ofstream out(*json_path, std::ios::trunc);
        out << nlohmann::json{{"midgard", midgard::kVersion}, {"results", results}}.dump(2) << "\n";
        if (!out) {
            std::fprintf(stderr, "error: could not write %s\n", json_path->c_str());
            return 1;
        }
    }
    return 0;
}
