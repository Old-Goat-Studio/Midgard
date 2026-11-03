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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include "midgard/anim_graph.h"
#include "midgard/animation.h"
#include "midgard/audio.h"
#include "midgard/det.h"
#include "midgard/humanoid.h"
#include "midgard/ik.h"
#include "midgard/jobs.h"
#include "midgard/nav_world.h"
#include "midgard/navmesh.h"
#include "midgard/physics.h"
#include "midgard/render/ktx.h"
#include "midgard/render/overlay.h"
#include "midgard/render/renderer.h"
#include "midgard/shape.h"
#include "midgard/terrain.h"
#include "midgard/terrain_streamer.h"
#include "midgard/version.h"
#include "midgard/worldgen.h"

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

// An Unreal-named humanoid of 55 joints: spine, neck and head; arms with fingers; legs.
midgard::RawSkeleton bench_humanoid() {
    midgard::RawSkeleton raw;
    const auto add = [&](const std::string& name, int parent, glm::vec3 at) {
        raw.names.push_back(name);
        raw.parents.push_back(parent);
        raw.rest.push_back({at, {1, 0, 0, 0}, {1, 1, 1}});
        return int(raw.names.size()) - 1;
    };
    int spine = add("pelvis", -1, {0, 1, 0});
    for (const char* n : {"spine_01", "spine_02", "spine_03", "neck_01"})
        spine = add(n, spine, {0, 0.12f, 0});
    add("head", spine, {0, 0.12f, 0});
    for (const char* side : {"_l", "_r"}) {
        const float x = side[1] == 'l' ? 1.0f : -1.0f;
        int arm = add(std::string("clavicle") + side, 3, {x * 0.05f, 0, 0});
        for (const char* n : {"upperarm", "lowerarm", "hand"})
            arm = add(n + std::string(side), arm, {x * 0.25f, 0, 0});
        for (int f = 0; f < 5; ++f) {
            int finger = arm;
            for (int i = 0; i < 3; ++i)
                finger = add("finger_" + std::to_string(f) + "_" + std::to_string(i) + side, finger,
                             {x * 0.03f, 0, 0});
        }
        int leg = add(std::string("thigh") + side, 0, {x * 0.1f, -0.05f, 0});
        for (const char* n : {"calf", "foot", "ball"})
            leg = add(n + std::string(side), leg, {0, -0.3f, 0});
    }
    return raw;
}

// A 1.3 s clip with 30 keys a second on every joint, swinging by `swing` radians.
midgard::RawClip bench_walk(const midgard::RawSkeleton& raw, float swing,
                            const std::string& name = "walk") {
    midgard::RawClip clip{name, 4.0f / 3.0f, {}};
    for (size_t j = 0; j < raw.names.size(); ++j) {
        midgard::RawTrack t{raw.names[j], {}, {}, {}};
        for (int k = 0; k <= 40; ++k)
            t.rotations.push_back({k / 30.0f, glm::angleAxis(swing * std::sin(k * 0.3f + float(j)),
                                                             glm::vec3(1, 0, 0))});
        clip.tracks.push_back(t);
    }
    return clip;
}

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
    list.push_back({"terrain.chunk_mesh",
                    "one terrain chunk's mesh: 35 x 35 heights of 6-octave noise, normals and "
                    "triangles (one thread)",
                    30, [](std::string&) -> std::function<void()> {
                        const auto terrain = std::make_shared<midgard::Terrain>(
                            midgard::body_from_json(nlohmann::json::parse(
                                R"({"id": "a", "shape": "bounded_area", "width": 3000,
                                    "length": 2000, "seed": 7})")),
                            midgard::TerrainSettings{});
                        return [terrain] {
                            const midgard::ChunkMesh m = terrain->mesh({0, 2, 5, 3});
                            g_sink = g_sink + m.indices.size();
                        };
                    }});
    // World generation graphs (milestone 5.11): a 35 x 35 chunk's worth of points, for the
    // 5.9 ground as a graph and for one using every node (two filters: smooth and erode).
    for (const auto& [name, which] : {std::pair{"worldgen.classic_35x35", "classic"},
                                      std::pair{"worldgen.everything_35x35", "everything"}}) {
        const std::string graph =
            which == std::string("classic")
                ? R"({"output": "height", "nodes": {"hills": {"op": "noise", "size": 500, "octaves": 6}, "scaled": {"op": "multiply", "in": [60, "hills"]}, "height": {"op": "add", "in": [8, "scaled"]}}})"
                : R"({"output": "out", "nodes": {"n": {"op": "noise", "size": 300, "octaves": 4, "salt": "base"}, "r": {"op": "ridged", "size": 900, "octaves": 3, "salt": "ridges"}, "lat": {"op": "input", "name": "latitude"}, "edge": {"op": "input", "name": "edge_distance"}, "end": {"op": "input", "name": "end_distance"}, "near_edge": {"op": "smoothstep", "in": ["edge"], "from": 0, "to": 400}, "near_end": {"op": "smoothstep", "in": ["end"], "from": 0, "to": 300}, "cold": {"op": "curve", "in": ["abs_lat"], "points": [[0, 0], [45, 10], [90, 40]]}, "abs_lat": {"op": "abs", "in": ["lat"]}, "land": {"op": "lerp", "in": ["n", "r", 0.4]}, "scaled": {"op": "multiply", "in": [50, "land", "near_edge", "near_end"]}, "stepped": {"op": "terrace", "in": ["scaled"], "step": 6, "sharpness": 0.7}, "smoothed": {"op": "smooth", "in": ["stepped"], "radius": 12}, "worn": {"op": "erode", "in": ["scaled"], "radius": 20, "strength": 0.6}, "blend": {"op": "lerp", "in": ["smoothed", "worn", 0.5]}, "sum": {"op": "add", "in": ["blend", "cold", 3]}, "low": {"op": "min", "in": ["sum", 70]}, "high": {"op": "max", "in": ["low", -25]}, "clamped": {"op": "clamp", "in": ["high"], "min": -20, "max": 65}, "diff": {"op": "subtract", "in": ["clamped", "n"]}, "out": {"op": "divide", "in": ["diff", 1.5]}}})";
        list.push_back(
            {name,
             std::string("a world graph (") + which +
                 ") at 35 x 35 points on a bounded area (one thread)",
             30, [graph](std::string&) -> std::function<void()> {
                 auto g = std::make_shared<midgard::WorldGraph>(
                     midgard::WorldGraph::from_json(nlohmann::json::parse(graph)));
                 return [g] {
                     const midgard::Shape area = midgard::Shape::bounded_area(3000.0, 2000.0, 9.81);
                     double sum = 0.0;
                     for (int j = 0; j < 35; ++j)
                         for (int i = 0; i < 35; ++i)
                             sum += g->evaluate(area, 7, {0, 0.4 + i * 1e-3, 0.4 + j * 1e-3});
                     g_sink = g_sink + std::size_t(sum);
                 };
             }});
    }
    list.push_back({"terrain.chunk_mesh_eroded",
                    "one terrain chunk's mesh from a world graph: 6-octave noise worn by erode "
                    "(five times the noise; one thread)",
                    30, [](std::string&) -> std::function<void()> {
                        midgard::TerrainSettings settings;
                        settings.graph = R"({"output": "h", "nodes": {
                            "n": {"op": "noise", "size": 500, "octaves": 6},
                            "s": {"op": "multiply", "in": [60, "n"]},
                            "w": {"op": "erode", "in": ["s"], "radius": 12, "strength": 0.6},
                            "h": {"op": "add", "in": [8, "w"]}}})";
                        const auto terrain = std::make_shared<midgard::Terrain>(
                            midgard::body_from_json(nlohmann::json::parse(
                                R"({"id": "a", "shape": "bounded_area", "width": 3000,
                                    "length": 2000, "seed": 7})")),
                            settings);
                        return [terrain] {
                            const midgard::ChunkMesh m = terrain->mesh({0, 2, 5, 3});
                            g_sink = g_sink + m.indices.size();
                        };
                    }});
    list.push_back({"terrain.placements_tile",
                    "objects placed in one 125 m tile by two rules (trees every 9 m, rocks every "
                    "20 m; one thread)",
                    30, [](std::string&) -> std::function<void()> {
                        const auto terrain = std::make_shared<midgard::Terrain>(
                            midgard::body_from_json(nlohmann::json::parse(
                                R"({"id": "a", "shape": "bounded_area", "width": 3000,
                                    "length": 2000, "seed": 7})")),
                            midgard::terrain_from_json(nlohmann::json::parse(R"({"placements": [
                                {"object": "t", "spacing": 9, "chance": 0.7, "slope": [0, 25]},
                                {"object": "r", "spacing": 20, "chance": 0.5, "slope": [5, 70],
                                 "align": 1}]})")));
                        return [terrain] {
                            const auto placed =
                                terrain->placements_in({0, terrain->placement_level(), 12, 9});
                            g_sink = g_sink + placed.size();
                        };
                    }});
    list.push_back({"nav.tile_32m",
                    "a 32 m navigation tile over rolling ground (0.5 m grid) with 12 boxes on it "
                    "(one thread)",
                    20, [](std::string&) -> std::function<void()> {
                        auto g = std::make_shared<midgard::NavGeometry>();
                        const auto h = [](float x, float z) {
                            return 0.6f * std::sin(x * 0.21f) * std::cos(z * 0.17f);
                        };
                        for (int j = 0; j < 80; ++j)
                            for (int i = 0; i < 80; ++i) {
                                const float x = i * 0.5f - 4.0f, z = j * 0.5f - 4.0f;
                                const glm::vec3 a(x, h(x, z), z), b(x + 0.5f, h(x + 0.5f, z), z),
                                    c(x, h(x, z + 0.5f), z + 0.5f),
                                    d(x + 0.5f, h(x + 0.5f, z + 0.5f), z + 0.5f);
                                g->add_triangle(a, c, b);
                                g->add_triangle(b, c, d);
                            }
                        for (int k = 0; k < 12; ++k) {  // boxes as walls (their tops and sides)
                            const float x = 2.0f + float(k % 4) * 8.0f,
                                        z = 3.0f + float(k / 4) * 9.0f;
                            const glm::vec3 lo(x, -1.0f, z), hi(x + 2.0f, 2.0f, z + 1.0f);
                            const auto at = [&](int c) {
                                return glm::vec3(c & 1 ? hi.x : lo.x, c & 2 ? hi.y : lo.y,
                                                 c & 4 ? hi.z : lo.z);
                            };
                            const int faces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1},
                                                     {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
                            for (const auto& f : faces) {
                                g->add_triangle(at(f[0]), at(f[2]), at(f[1]));
                                g->add_triangle(at(f[0]), at(f[3]), at(f[2]));
                            }
                        }
                        return [g] {
                            const midgard::NavTileData t = midgard::build_nav_tile(
                                midgard::NavSettings{}, glm::vec3(0.0f), {0, 0}, *g);
                            g_sink = g_sink + t.size();
                        };
                    }});
    list.push_back(
        {"anim.retarget_clip",
         "a 1.3 s clip (30 keys a second on every joint) retargeted between two "
         "55-joint humanoids of other proportions (one thread)",
         20, [](std::string&) -> std::function<void()> {
             const midgard::RawSkeleton raw = bench_humanoid();
             midgard::RawSkeleton tall = raw;
             for (auto& r : tall.rest) r.translation *= 1.3f;
             const midgard::RawClip clip = bench_walk(raw, 0.3f);
             auto data = std::make_shared<
                 std::tuple<midgard::RawSkeleton, midgard::RawSkeleton, midgard::RawClip>>(
                 raw, tall, clip);
             return [data] {
                 const auto& [from, to, c] = *data;
                 const midgard::RawClip made = midgard::retarget(
                     c, from, midgard::map_humanoid(from), to, midgard::map_humanoid(to));
                 g_sink = g_sink + made.tracks.size();
             };
         }});
    list.push_back(
        {"anim.graph_32_characters",
         "32 characters' animation graphs moved on a frame (55 joints: a walk/run blend space, "
         "an upper-body layer, root motion and events; one thread)",
         30, [](std::string&) -> std::function<void()> {
             struct Crowd {
                 midgard::Skeleton skeleton = midgard::Skeleton::build(bench_humanoid());
                 std::vector<midgard::Clip> clips;
                 std::vector<midgard::Pose> poses;
                 std::vector<midgard::AnimPlayer> players;
             };
             auto crowd = std::make_shared<Crowd>();
             const midgard::RawSkeleton raw = bench_humanoid();
             for (const auto& [name, swing] :
                  {std::pair{"walk", 0.3f}, std::pair{"run", 0.6f}, std::pair{"wave", 0.9f}})
                 crowd->clips.push_back(
                     midgard::Clip::build(crowd->skeleton, bench_walk(raw, swing, name)));
             auto graph = std::make_shared<const midgard::AnimGraph>(midgard::AnimGraph::parse(
                 nlohmann::json::parse(R"({"root_motion": true, "layers": [
                   {"name": "base", "states": {"move": {"blend": "speed",
                     "clips": [["walk", 1.4], ["run", 4.5]], "events": [[0, "step"]]}}},
                   {"name": "upper", "mask": "spine_02", "weight": 0.7,
                    "states": {"wave": {"clip": "wave"}}}]})")));
             crowd->poses.reserve(32);
             crowd->players.reserve(32);
             const auto find = [c = crowd.get()](const std::string& name) -> const midgard::Clip* {
                 for (const auto& clip : c->clips)
                     if (clip.name() == name) return &clip;
                 return nullptr;
             };
             for (int i = 0; i < 32; ++i) {
                 crowd->poses.emplace_back(crowd->skeleton);
                 crowd->players.emplace_back(graph, crowd->skeleton, find, 0);
                 crowd->players.back().set("speed", 1.0f + 0.1f * float(i));
             }
             return [crowd] {
                 for (size_t i = 0; i < crowd->players.size(); ++i) {
                     const auto step = crowd->players[i].update(1.0f / 60.0f, crowd->poses[i]);
                     g_sink = g_sink + step.events.size();
                 }
             };
         }});
    list.push_back(
        {"anim.ik_32_characters",
         "32 characters' IK on a frame (55 joints: both legs to the ground with their soles "
         "along it, the head looking, a hand reaching; one thread)",
         30, [](std::string&) -> std::function<void()> {
             struct Crowd {
                 midgard::RawSkeleton raw = bench_humanoid();
                 midgard::Skeleton skeleton = midgard::Skeleton::build(raw);
                 std::vector<midgard::JointTransform> rest;
                 int thigh[2], calf[2], foot[2], head, upper, lower, hand;
             };
             auto c = std::make_shared<Crowd>();
             midgard::Pose pose(c->skeleton);
             c->rest = pose.local();
             const auto j = [&](const char* name) { return c->skeleton.find(name); };
             for (int side = 0; side < 2; ++side) {
                 const std::string s = side ? "_r" : "_l";
                 c->thigh[side] = j(("thigh" + s).c_str());
                 c->calf[side] = j(("calf" + s).c_str());
                 c->foot[side] = j(("foot" + s).c_str());
             }
             c->head = j("head");
             c->upper = j("upperarm_r");
             c->lower = j("lowerarm_r");
             c->hand = j("hand_r");
             return [c] {
                 for (int i = 0; i < 32; ++i) {
                     std::vector<midgard::JointTransform> local = c->rest;
                     const std::span<const int> parents = c->skeleton.parents();
                     const float lift = 0.02f * float(i % 5);
                     for (int side = 0; side < 2; ++side) {
                         const auto model = midgard::model_of(local, parents);
                         glm::vec3 target(model[size_t(c->foot[side])][3]);
                         target.y += side ? lift : -lift;
                         midgard::two_bone_ik(local, parents, c->thigh[side], c->calf[side],
                                              c->foot[side], target,
                                              target + glm::vec3(0, 0.5f, 0.5f));
                         midgard::orient(local, parents, c->foot[side], {0, 1, 0},
                                         glm::normalize(glm::vec3(0, 1, -0.3f)), 0.6f);
                     }
                     midgard::aim(local, parents, c->head, {0, 0, 1}, {1, 1.6f, 1}, 1.2f);
                     midgard::two_bone_ik(local, parents, c->upper, c->lower, c->hand,
                                          {-0.3f, 1.1f, 0.3f}, {-0.3f, 0.8f, -0.5f});
                     g_sink = g_sink + size_t(local[size_t(c->hand)].rotation.w * 100.0f);
                 }
             };
         }});
    list.push_back({"audio.mix_32_voices",
                    "10 ms of sound mixed (48 kHz stereo) from 32 voices playing on the six "
                    "buses, each at its own volume, pitch and pan, half of them placed in 3D "
                    "(and muffled), with the reverb on, offline",
                    200, [](std::string&) -> std::function<void()> {
                        midgard::audio::MixerOptions options;
                        options.device = false;
                        auto mixer = std::make_shared<midgard::audio::Mixer>(options);
                        std::vector<float> wave(48000);
                        for (size_t i = 0; i < wave.size(); ++i)
                            wave[i] = 0.02f * float(std::sin(double(i) * 0.05));
                        const auto sound =
                            mixer->load("tone", midgard::audio::wav_bytes(wave, 1, 48000));
                        for (int v = 0; v < 32; ++v) {
                            midgard::audio::PlayOptions play;
                            play.bus = midgard::audio::Bus(1 + v % 5);
                            play.volume = 0.5f + 0.01f * float(v);
                            play.pitch = 0.8f + 0.02f * float(v);
                            play.pan = -1.0f + float(v) / 16.0f;
                            play.loop = true;
                            play.spatial = v % 2 == 0;
                            play.position = {float(v) - 16.0f, 0.0f, -5.0f};
                            const auto voice = mixer->play(sound, play);
                            if (play.spatial) mixer->set_voice_muffle(voice, 0.5f);
                        }
                        mixer->set_reverb(midgard::audio::reverb_preset("hall").value());
                        auto out = std::make_shared<std::vector<float>>(480 * 2);
                        return [mixer, out] { mixer->mix(*out); };
                    }});
    list.push_back({"anim.pose_65_joints",
                    "a 65-joint humanoid's pose sampled from a clip (30 keys a second on every "
                    "joint) and put in model space, 100 times (one thread)",
                    30, [](std::string&) -> std::function<void()> {
                        // A humanoid's shape, as many joints as the Quaternius Superhero: a spine
                        // of 6, a head, two arms of 5 with five fingers of 4 (a "leaf" at each
                        // tip), two legs of 4 (65 joints).
                        midgard::RawSkeleton raw;
                        const auto add = [&](int parent, float up) {
                            raw.names.push_back("j" + std::to_string(raw.names.size()));
                            raw.parents.push_back(parent);
                            raw.rest.push_back({{0.0f, up, 0.0f}, {1, 0, 0, 0}, {1, 1, 1}});
                            return int(raw.names.size()) - 1;
                        };
                        int spine = add(-1, 1.0f);
                        for (int i = 0; i < 5; ++i) spine = add(spine, 0.1f);
                        add(spine, 0.2f);
                        for (int side = 0; side < 2; ++side) {
                            int arm = spine;
                            for (int i = 0; i < 5; ++i) arm = add(arm, 0.15f);
                            for (int f = 0; f < 5; ++f) {
                                int finger = arm;
                                for (int i = 0; i < 4; ++i) finger = add(finger, 0.02f);
                            }
                        }
                        for (int side = 0; side < 2; ++side) {
                            int leg = 0;
                            for (int i = 0; i < 4; ++i) leg = add(leg, -0.25f);
                        }
                        if (raw.names.size() != 65) throw std::logic_error("not 65 joints");
                        midgard::RawClip clip{"walk", 4.0f / 3.0f, {}};
                        for (size_t j = 0; j < raw.names.size(); ++j) {
                            midgard::RawTrack t{raw.names[j], {}, {}, {}};
                            for (int k = 0; k <= 40; ++k)
                                t.rotations.push_back(
                                    {k / 30.0f, glm::angleAxis(0.3f * std::sin(k * 0.3f + float(j)),
                                                               glm::vec3(1, 0, 0))});
                            clip.tracks.push_back(t);
                        }
                        auto skeleton =
                            std::make_shared<midgard::Skeleton>(midgard::Skeleton::build(raw));
                        auto c =
                            std::make_shared<midgard::Clip>(midgard::Clip::build(*skeleton, clip));
                        auto pose = std::make_shared<midgard::Pose>(*skeleton);
                        return [skeleton, c, pose] {
                            for (int i = 0; i < 100; ++i) pose->sample(*c, i * 0.013f);
                            g_sink = g_sink + size_t(pose->model().back()[3][1] * 1000.0f);
                        };
                    }});
    list.push_back({"nav.terrain_tile",
                    "a 32 m navigation tile over terrain (5.9's hills): the ground sampled and "
                    "Recast (one thread)",
                    20, [](std::string&) -> std::function<void()> {
                        auto terrain = std::make_shared<midgard::Terrain>(
                            midgard::body_from_json(nlohmann::json::parse(
                                R"({"id": "a", "shape": "bounded_area", "width": 3000,
                                    "length": 3000, "seed": 7})")),
                            midgard::terrain_from_json(nlohmann::json::parse(R"({"seed": 7})")));
                        auto nav = std::make_shared<midgard::NavWorld>(*terrain, nullptr);
                        nav->set_range(0.0);  // (just the tile under the point)
                        const glm::dvec3 at = terrain->body().position({0, 0.5, 0.5, 0.0});
                        return [terrain, nav, at] {
                            nav->ground_changed();  // (made again each time)
                            nav->update(at, true);
                            g_sink = g_sink + nav->stats().tiles;
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
    // Skinned characters (milestone 5.13): 32 of them, each about 8,500 vertices on 65 joints
    // (the Quaternius characters' size), with sun shadows, at the phone's quality.
    list.push_back(
        {"render.skinned_32_phone",
         "32 skinned characters (8,500 vertices, 65 joints each) with sun shadows drawn offscreen "
         "at 640x360, phone quality (GPU)",
         60, [](std::string& skip) -> std::function<void()> {
             using namespace midgard::render;
             std::shared_ptr<Renderer> renderer;
             try {
                 RendererDesc desc;
                 desc.offscreen = true;
                 desc.width = 640;
                 desc.height = 360;
                 desc.quality = quality_settings(Quality::Phone);
                 renderer = std::make_shared<Renderer>(desc);
             } catch (const std::exception& e) {
                 skip = std::string("no Vulkan: ") + e.what();
                 return {};
             }
             // A tube 1.8 m tall, 257 rings of 33 vertices, skinned to a chain of 65 joints.
             MeshData tube;
             std::vector<glm::u16vec4> joints;
             std::vector<glm::vec4> weights;
             const int rings = 257, segments = 32, chain = 65;
             for (int i = 0; i < rings; ++i) {
                 const float y = 1.8f * float(i) / float(rings - 1);
                 const float at = float(i) / float(rings - 1) * float(chain - 1);
                 const int j = std::min(int(at), chain - 2);
                 for (int k = 0; k <= segments; ++k) {
                     const float a = 6.2831853f * float(k) / float(segments);
                     tube.vertices.push_back({{0.2f * std::cos(a), y, 0.2f * std::sin(a)},
                                              {std::cos(a), 0, std::sin(a)},
                                              {0, 0},
                                              {1, 0, 0, 1}});
                     joints.emplace_back(j, j + 1, 0, 0);
                     weights.emplace_back(1.0f - (at - float(j)), at - float(j), 0.0f, 0.0f);
                 }
             }
             for (int i = 0; i + 1 < rings; ++i)
                 for (int k = 0; k < segments; ++k) {
                     const auto a = std::uint32_t(i * (segments + 1) + k), b = a + 1,
                                c = a + std::uint32_t(segments + 1), d = c + 1;
                     tube.indices.insert(tube.indices.end(), {a, c, b, b, c, d});
                 }
             const MeshId mesh = renderer->upload_skinned_mesh(tube, joints, weights);
             // Each a little bent: every joint turned a little about Z.
             auto palettes = std::make_shared<std::vector<std::vector<glm::mat4>>>(32);
             for (int c = 0; c < 32; ++c) {
                 glm::mat4 m(1.0f);
                 for (int j = 0; j < chain; ++j) {
                     const float y = 1.8f * float(j) / float(chain - 1);
                     (*palettes)[size_t(c)].push_back(
                         m * glm::translate(glm::mat4(1.0f), glm::vec3(0, -y, 0)));
                     m = m * glm::translate(glm::mat4(1.0f), glm::vec3(0, 1.8f / (chain - 1), 0)) *
                         glm::rotate(glm::mat4(1.0f), 0.01f * float(c % 5), glm::vec3(0, 0, 1));
                 }
             }
             auto draws = std::make_shared<std::vector<DrawItem>>();
             for (int c = 0; c < 32; ++c) {
                 DrawItem d{mesh,
                            glm::translate(glm::mat4(1.0f),
                                           glm::vec3((c % 8) * 1.5f - 5.25f, 0, (c / 8) * -2.0f)),
                            glm::vec4(0.7f, 0.5f, 0.4f, 1.0f)};
                 d.skin = (*palettes)[size_t(c)];
                 draws->push_back(d);
             }
             Camera camera;
             camera.view =
                 glm::lookAt(glm::vec3(0, 4, 8), glm::vec3(0, 0.8f, -3), glm::vec3(0, 1, 0));
             camera.projection = perspective(glm::radians(55.0f), 640.0f / 360.0f, 0.1f, 100.0f);
             return [renderer, draws, palettes, camera] {
                 renderer->render_frame(camera, Light{}, *draws, glm::vec4(0.1f, 0.1f, 0.2f, 1));
             };
         }});
    // Inside the largest hollow world (radius 20 km), standing on the ground looking up: all of
    // it is in view, the far side overhead in a few coarse chunks (milestone 5.10).
    list.push_back(
        {"render.terrain_hollow_phone",
         "a frame inside a 20 km hollow world, every chunk streamed, at 640x360, phone quality "
         "(GPU)",
         30, [](std::string& skip) -> std::function<void()> {
             using namespace midgard::render;
             std::shared_ptr<Renderer> renderer;
             try {
                 RendererDesc desc;
                 desc.offscreen = true;
                 desc.width = 640;
                 desc.height = 360;
                 desc.quality = quality_settings(Quality::Phone);
                 renderer = std::make_shared<Renderer>(desc);
             } catch (const std::exception& e) {
                 skip = std::string("no Vulkan: ") + e.what();
                 return {};
             }
             const midgard::Body body = midgard::body_from_json(nlohmann::json::parse(
                 R"({"id": "h", "shape": "sphere_inside", "radius": 20000, "seed": 12,
                     "origin": [0, 20000, 0]})"));
             auto streamer =
                 std::make_shared<midgard::TerrainStreamer>(body, midgard::TerrainSettings{});
             const glm::dvec3 eye =
                 streamer->terrain().ground(glm::dvec3(0.0)) + glm::dvec3(0.0, 2.0, 0.0);
             streamer->update(eye, renderer.get(), true);
             auto draws = std::make_shared<std::vector<DrawItem>>(streamer->draws(eye));
             Camera camera;
             camera.origin = eye;
             camera.view = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.5f, -1.0f),
                                       glm::vec3(0.0f, 1.0f, 0.0f));
             camera.projection = perspective(glm::radians(65.0f), 640.0f / 360.0f, 0.5f, 50000.0f);
             Light light;
             light.up = glm::vec3(0.0f, 1.0f, 0.0f);
             return [renderer, streamer, draws, camera, light] {
                 renderer->render_frame(camera, light, *draws, glm::vec4(0.5f, 0.6f, 0.7f, 1));
             };
         }});
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
        // (Said first, unbuffered: if one crashes, CI's log names it.)
        std::fprintf(stderr, "running %s\n", b.name.c_str());
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
