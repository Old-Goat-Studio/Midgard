// SPDX-License-Identifier: Apache-2.0
#include "selftest.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <exception>
#include <numeric>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include "midgard/anim_graph.h"
#include "midgard/animation.h"
#include "midgard/audio.h"
#include "midgard/body.h"
#include "midgard/det.h"
#include "midgard/hash.h"
#include "midgard/ik.h"
#include "midgard/jobs.h"
#include "midgard/package.h"
#include "midgard/physics.h"
#include "midgard/render/mesh_data.h"
#include "midgard/render/renderer.h"
#include "midgard/shape.h"
#include "midgard/terrain.h"
#include "midgard/version.h"
#include "midgard/world_type.h"
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

// The terrain on this device: chunk hashes as on every other target (the cube-sphere's too,
// sculpted across a face edge), and a chunk made within
// its budget (schema/budgets.json's terrain.chunk_mesh, 3 ms, plus the budgets' 25% margin).
constexpr double kChunkBudgetMs = 3.0 * 1.25;

Check terrain_check() {
    const std::uint64_t got = terrain_fingerprint();
    if (got != kTerrainFingerprint)
        return {
            "Terrain", Status::Fail,
            format("fingerprint %016" PRIx64 ", expected %016" PRIx64, got, kTerrainFingerprint)};
    const std::uint64_t graphs = worldgen_fingerprint();
    if (graphs != kWorldGraphFingerprint)
        return {"Terrain", Status::Fail,
                format("world graphs' fingerprint %016" PRIx64 ", expected %016" PRIx64, graphs,
                       kWorldGraphFingerprint)};
    const std::uint64_t placed = placement_fingerprint();
    if (placed != kPlacementFingerprint)
        return {"Terrain", Status::Fail,
                format("placements' fingerprint %016" PRIx64 ", expected %016" PRIx64, placed,
                       kPlacementFingerprint)};
    const std::uint64_t spheres = sphere_terrain_fingerprint();
    if (spheres != kSphereTerrainFingerprint)
        return {"Terrain", Status::Fail,
                format("cube-sphere fingerprint %016" PRIx64 ", expected %016" PRIx64, spheres,
                       kSphereTerrainFingerprint)};
    const std::uint64_t samples = sample_world_types_fingerprint();
    if (samples != kSampleWorldTypesFingerprint)
        return {"Terrain", Status::Fail,
                format("sample world types' fingerprint %016" PRIx64 ", expected %016" PRIx64,
                       samples, kSampleWorldTypesFingerprint)};
    const Terrain terrain(body_from_json(nlohmann::json::parse(
                              R"({"id": "a", "shape": "bounded_area", "width": 3000,
                                  "length": 2000, "seed": 7})")),
                          TerrainSettings{});
    std::vector<double> times;
    volatile std::size_t sink = 0;  // (so the work isn't optimised away)
    for (int i = 0; i < 25; ++i) {
        const auto start = std::chrono::steady_clock::now();
        sink = sink + terrain.mesh({0, 2, i % 8, 3}).indices.size();
        times.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count());
    }
    std::sort(times.begin(), times.end());
    const double median = times[times.size() / 2];
    const std::string detail =
        format("chunk hashes on four shapes: %016" PRIx64
               ", on planets and hollow worlds: %016" PRIx64 ", world graphs: %016" PRIx64
               ", sample world types: %016" PRIx64 "; a chunk in %.2f ms (budget %.2f)",
               got, spheres, graphs, samples, median, kChunkBudgetMs);
    return {"Terrain", median <= kChunkBudgetMs ? Status::Pass : Status::Fail, detail};
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

// A scene drawn 30 times at 1280 x 720 on this device's quality tier: its median frame time
// against the frame budget. "Frame time" draws the sunset; "Weather" the heaviest rain
// (milestone 5.15: the tier's most streaks, under cloud and fog); "Space" a whole planet from
// space against the stars (milestone 5.16: all its ground in view at once).
Check frame_check(const Options& options, const char* name = "Frame time",
                  const char* scene_name = "sky_sunset") {
    const render::Quality tier = options.quality.value_or(render::default_quality());
    try {
        render::RendererDesc desc;
        desc.offscreen = true;
        desc.width = 1280;
        desc.height = 720;
        desc.quality = render::quality_settings(tier);
        render::Renderer renderer(desc);
        const auto scene = test_scenes::make(scene_name);
        scene->upload(renderer);
        renderer.set_environment(scene->environment());
        const render::Camera camera = scene->camera(1280.0f / 720.0f);
        scene->stream(renderer, camera.origin);  // (its ground, made: none in most)
        const auto draws = scene->draws(camera.origin);
        renderer.set_terrain_look(scene->terrain_look());
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
            return {name, Status::Skip, detail + ", software renderer: not checked"};
        return {name, median <= kFrameBudgetMs ? Status::Pass : Status::Fail, detail};
    } catch (const std::exception& e) {
        return no_vulkan(name, e, options.vulkan_required);
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

// Characters on this device (milestone 5.13): 32 humanoids of 65 joints animated by a graph (a
// walk/run blend space under an upper-body layer), their feet put on the ground by IK, and drawn
// skinned with sun shadows, all within a frame's budget.
// Sound (milestone 5.14): the device opens (a phone always has one; a PC without one, or a CI
// machine, skips), and 32 voices mix in well under real time, to the levels expected.
Check sound_check(const Options& options) {
    constexpr double kBlockBudgetMs = 2.0;  // of each 10 ms block (the audio thread's job)
    std::string device;
    try {
        midgard::audio::Mixer live;
        device = live.has_device() ? live.backend() + format(", %u Hz", live.sample_rate()) : "";
    } catch (const std::exception& e) {
        return {"Sound", Status::Fail, std::string("the mixer didn't start: ") + e.what()};
    }
    if (device.empty() && options.vulkan_required)  // (on a phone)
        return {"Sound", Status::Fail, "no audio device opened"};
    try {
        midgard::audio::MixerOptions offline;
        offline.device = false;
        midgard::audio::Mixer mixer(offline);
        std::vector<float> wave(48000);
        for (size_t i = 0; i < wave.size(); ++i)
            wave[i] = 0.02f * float(std::sin(2.0 * 3.14159265358979 * 500.0 * double(i) / 48000));
        const auto tone = mixer.load("tone", midgard::audio::wav_bytes(wave, 1, 48000));
        for (int v = 0; v < 32; ++v) {
            midgard::audio::PlayOptions play;
            play.bus = midgard::audio::Bus(1 + v % 5);
            play.loop = true;
            mixer.play(tone, play);
        }
        std::vector<float> block(480 * 2);
        std::vector<double> ms;
        double sum = 0.0;
        for (int b = 0; b < 100; ++b) {  // a second
            const auto start = std::chrono::steady_clock::now();
            mixer.mix(block);
            ms.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count());
            for (const float x : block) sum += double(x) * x;
        }
        // 32 in-step tones of 0.02: 0.64 at the peak, its RMS 0.64 / sqrt 2.
        const double level = std::sqrt(sum / double(100 * block.size()));
        if (std::abs(level - 0.64 / std::sqrt(2.0)) > 0.01)
            return {"Sound", Status::Fail, format("mixed to the wrong level (%.3f)", level)};
        std::sort(ms.begin(), ms.end());
        const double median = ms[ms.size() / 2];
        const std::string detail =
            format("%s; 32 voices, 10 ms mixed in %.2f ms",
                   device.empty() ? "no device (skipped playing)" : device.c_str(), median);
        if (median > kBlockBudgetMs) return {"Sound", Status::Fail, detail};
        return {"Sound", device.empty() ? Status::Skip : Status::Pass, detail};
    } catch (const std::exception& e) {
        return {"Sound", Status::Fail, std::string("mixing: ") + e.what()};
    }
}

Check characters_check(const Options& options) {
    const render::Quality tier = options.quality.value_or(render::default_quality());
    try {
        // The humanoid, as the Quaternius Superhero is made: a root; the hips, spine, neck and
        // head; arms with five fingers of four; legs to the toes' tips (65 joints).
        RawSkeleton raw;
        const auto add = [&](const std::string& name, int parent, glm::vec3 at) {
            raw.names.push_back(name);
            raw.parents.push_back(parent);
            raw.rest.push_back({at, {1, 0, 0, 0}, {1, 1, 1}});
            return int(raw.names.size()) - 1;
        };
        const int root = add("root", -1, {0, 0, 0});
        const int pelvis = add("pelvis", root, {0, 1, 0});
        int spine = pelvis, chest = pelvis;
        for (const char* n : {"spine_01", "spine_02", "spine_03", "neck_01"}) {
            spine = add(n, spine, {0, 0.12f, 0});
            if (std::string(n) == "spine_03") chest = spine;
        }
        add("head", spine, {0, 0.12f, 0});
        for (const char* side : {"_l", "_r"}) {
            const float x = side[1] == 'l' ? 1.0f : -1.0f;
            int arm = add(std::string("clavicle") + side, chest, {x * 0.05f, 0, 0});
            for (const char* n : {"upperarm", "lowerarm", "hand"})
                arm = add(n + std::string(side), arm, {x * 0.25f, 0, 0});
            for (int f = 0; f < 5; ++f) {
                int finger = arm;
                for (int i = 0; i < 4; ++i)
                    finger = add("finger_" + std::to_string(f) + "_" + std::to_string(i) + side,
                                 finger, {x * 0.03f, 0, 0});
            }
            int leg = add(std::string("thigh") + side, pelvis, {x * 0.1f, -0.05f, 0});
            for (const char* n : {"calf", "foot"})
                leg = add(n + std::string(side), leg, {0, -0.4f, 0});
            leg = add(std::string("ball") + side, leg, {0, -0.05f, 0.1f});
            add(std::string("ball_leaf") + side, leg, {0, 0, 0.05f});
        }
        if (raw.names.size() != 65)  // (the tubes drawn below hang from 65 joints)
            return {"Characters", Status::Fail, "the test humanoid isn't 65 joints"};
        const Skeleton skeleton = Skeleton::build(raw);
        std::vector<Clip> clips;
        for (const auto& [name, swing] :
             {std::pair{"walk", 0.3f}, std::pair{"run", 0.6f}, std::pair{"wave", 0.9f}}) {
            RawClip clip{name, 4.0f / 3.0f, {}};
            for (size_t j = 0; j < raw.names.size(); ++j) {
                RawTrack t{raw.names[j], {}, {}, {}};
                for (int k = 0; k <= 40; ++k)
                    t.rotations.push_back(
                        {k / 30.0f, glm::angleAxis(swing * std::sin(k * 0.3f + float(j)),
                                                   glm::vec3(1, 0, 0))});
                clip.tracks.push_back(t);
            }
            clips.push_back(Clip::build(skeleton, clip));
        }
        const auto graph = std::make_shared<const AnimGraph>(AnimGraph::parse(std::string(
            R"({"layers": [{"name": "base", "states": {"move": {"blend": "speed",
                  "clips": [["walk", 1.4], ["run", 4.5]]}}},
                {"name": "upper", "mask": "spine_02", "weight": 0.5,
                 "states": {"wave": {"clip": "wave"}}}]})")));
        const auto find = [&](const std::string& name) -> const Clip* {
            for (const Clip& c : clips)
                if (c.name() == name) return &c;
            return nullptr;
        };
        std::vector<Pose> poses;
        std::vector<AnimPlayer> players;
        poses.reserve(32);
        players.reserve(32);
        for (int i = 0; i < 32; ++i) {
            poses.emplace_back(skeleton);
            players.emplace_back(graph, skeleton, find, skeleton.find("pelvis"));
            players.back().set("speed", 1.0f + 0.1f * float(i));
        }
        const int thigh[2] = {skeleton.find("thigh_l"), skeleton.find("thigh_r")};
        const int calf[2] = {skeleton.find("calf_l"), skeleton.find("calf_r")};
        const int foot[2] = {skeleton.find("foot_l"), skeleton.find("foot_r")};

        // Drawn as skinned tubes 1.8 m tall of 8,500 vertices on the 65 joints.
        render::RendererDesc desc;
        desc.offscreen = true;
        desc.width = 1280;
        desc.height = 720;
        desc.quality = render::quality_settings(tier);
        render::Renderer renderer(desc);
        render::MeshData tube;
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
        const render::MeshId mesh = renderer.upload_skinned_mesh(tube, joints, weights);
        std::vector<std::vector<glm::mat4>> palettes(32);
        std::vector<render::DrawItem> draws(32);
        for (int c = 0; c < 32; ++c) {
            draws[size_t(c)].mesh = mesh;
            draws[size_t(c)].model = glm::translate(
                glm::mat4(1.0f), glm::vec3((c % 8) * 1.5f - 5.25f, 0, (c / 8) * -2.0f));
            draws[size_t(c)].color = glm::vec4(0.7f, 0.5f, 0.4f, 1.0f);
        }
        render::Camera camera;
        camera.view = glm::lookAt(glm::vec3(0, 4, 8), glm::vec3(0, 0.8f, -3), glm::vec3(0, 1, 0));
        camera.projection =
            render::perspective(glm::radians(55.0f), 1280.0f / 720.0f, 0.1f, 100.0f);

        std::vector<double> times;
        for (int frame = 0; frame < 35; ++frame) {
            const auto start = std::chrono::steady_clock::now();
            for (size_t c = 0; c < 32; ++c) {
                players[c].update(1.0f / 60.0f, poses[c]);
                std::vector<JointTransform> local = poses[c].local();
                for (int side = 0; side < 2; ++side) {  // feet on uneven ground
                    glm::vec3 target(joint_model(local, skeleton.parents(), foot[side])[3]);
                    target.y += 0.05f * float((int(c) + side) % 3) - 0.05f;
                    two_bone_ik(local, skeleton.parents(), thigh[side], calf[side], foot[side],
                                target, target + glm::vec3(0, 0.5f, 0.5f));
                    orient(local, skeleton.parents(), foot[side], {0, 1, 0},
                           glm::normalize(glm::vec3(0, 1, -0.2f)), 0.6f);
                }
                poses[c].set_local(local);
                palettes[c].assign(poses[c].model().begin(), poses[c].model().end());
                draws[c].skin = palettes[c];
            }
            renderer.render_frame(camera, render::Light{}, draws, glm::vec4(0.1f, 0.1f, 0.2f, 1));
            const auto end = std::chrono::steady_clock::now();
            if (frame >= 5)
                times.push_back(std::chrono::duration<double, std::milli>(end - start).count());
        }
        std::sort(times.begin(), times.end());
        const double median = times[times.size() / 2];
        const std::string detail = format(
            "32 characters animated, IK and skinned: %.1f ms a frame (budget %.1f), %s "
            "quality",
            median, kFrameBudgetMs, render::quality_name(tier));
        if (renderer.software())
            return {"Characters", Status::Skip, detail + ", software renderer: not checked"};
        return {"Characters", median <= kFrameBudgetMs ? Status::Pass : Status::Fail, detail};
    } catch (const AnimationError& e) {
        return {"Characters", Status::Fail, e.what()};
    } catch (const std::exception& e) {
        return no_vulkan("Characters", e, options.vulkan_required);
    }
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
    checks.push_back(terrain_check());
    checks.push_back(physics_check());
    checks.push_back(render_check(options.vulkan_required));
    checks.push_back(frame_check(options));
    checks.push_back(frame_check(options, "Weather", "sky_rain"));
    checks.push_back(frame_check(options, "Space", "space"));
    checks.push_back(characters_check(options));
    checks.push_back(sound_check(options));
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
