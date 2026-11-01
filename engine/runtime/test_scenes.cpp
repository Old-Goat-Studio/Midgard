// SPDX-License-Identifier: Apache-2.0
#include "test_scenes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

#include <nlohmann/json.hpp>

#include "midgard/collider_outlines.h"
#include "midgard/environment.h"
#include "midgard/render/ktx.h"
#include "midgard/render/mesh_data.h"
#include "midgard/scene.h"
#include "midgard/sunlight.h"
#include "midgard/terrain_streamer.h"
#include "midgard/world_light.h"

namespace midgard::test_scenes {

namespace {

using render::DrawItem;

glm::mat4 at(glm::vec3 position, glm::vec3 scale = glm::vec3(1.0f)) {
    return glm::scale(glm::translate(glm::mat4(1.0f), position), scale);
}

// Draw items are written with world positions; the camera of every test scene is near the
// origin, so making them relative to it is a plain subtraction.
std::vector<DrawItem> relative(std::vector<DrawItem> items, const glm::dvec3& origin) {
    for (DrawItem& d : items) d.model[3] -= glm::vec4(glm::vec3(origin), 0.0f);
    return items;
}

render::Camera look(glm::vec3 eye, glm::vec3 target, float aspect, float fov = 40.0f) {
    render::Camera c;
    c.origin = glm::dvec3(eye);
    c.view = glm::lookAt(glm::vec3(0.0f), target - eye, glm::vec3(0, 1, 0));
    c.projection = render::perspective(glm::radians(fov), aspect, 0.05f, 200.0f);
    return c;
}

// A 32 x 32 texture from a function of (u, v) in 0..1 giving RGBA 0..1. Small on purpose: on
// screen the test objects magnify their textures, which every driver filters alike; shrinking
// one (mip selection, anisotropy) is approximated differently by each driver.
render::TextureData texture(bool srgb, const std::function<glm::vec4(float, float)>& f) {
    constexpr uint32_t size = 32;
    std::vector<uint8_t> px;
    px.reserve(size * size * 4);
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x) {
            const glm::vec4 c = glm::clamp(f((x + 0.5f) / size, (y + 0.5f) / size), 0.0f, 1.0f);
            for (int i = 0; i < 4; ++i) px.push_back(static_cast<uint8_t>(std::lround(c[i] * 255)));
        }
    return render::make_texture(size, size, srgb, std::move(px));
}

class Materials : public TestScene {
public:
    void upload(render::Renderer& r) override { sphere_ = r.upload_mesh(render::make_sphere()); }
    std::vector<DrawItem> draws(const glm::dvec3& origin) const override {
        std::vector<DrawItem> out;
        for (int row = 0; row < 5; ++row)
            for (int col = 0; col < 5; ++col) {
                DrawItem d;
                d.mesh = sphere_;
                d.model = at({(col - 2) * 1.1f, (2 - row) * 1.1f, 0.0f});
                d.color = glm::vec4(0.85f, 0.55f, 0.3f, 1.0f);
                d.metallic = row / 4.0f;
                d.roughness = 0.05f + col * 0.2375f;
                out.push_back(d);
            }
        return relative(out, origin);
    }
    render::Camera camera(float aspect) const override {
        return look({0, 0, 9}, {0, 0, 0}, aspect, 38.0f);
    }
    render::Light light(const glm::dvec3&) const override {
        render::Light l;
        l.direction = glm::normalize(glm::vec3(-0.5f, -0.6f, -1.0f));
        l.intensity = 1.5f;
        return l;
    }
    glm::vec4 sky() const override { return {0.12f, 0.13f, 0.16f, 1.0f}; }

private:
    render::MeshId sphere_ = 0;
};

class Textures : public TestScene {
public:
    // compressed: every texture goes through KTX2 (Basis UASTC) and the GPU's own format.
    explicit Textures(bool compressed = false) : compressed_(compressed) {}
    void upload(render::Renderer& r) override {
        cube_ = r.upload_mesh(render::make_cube());
        sphere_ = r.upload_mesh(render::make_sphere());
        const auto upload = [&](const render::TextureData& t) {
            return compressed_ ? r.upload_ktx2(render::encode_ktx2(t)) : r.upload_texture(t);
        };
        // Base colour: an 8 x 8 checker of two colours.
        const auto checker = upload(texture(true, [](float u, float v) {
            const bool odd = (int(u * 8) + int(v * 8)) % 2;
            return odd ? glm::vec4(0.9f, 0.85f, 0.7f, 1) : glm::vec4(0.2f, 0.35f, 0.6f, 1);
        }));
        // Normal map: a 4 x 4 grid of round bumps.
        const auto bumps = upload(texture(false, [](float u, float v) {
            const glm::vec2 c = glm::fract(glm::vec2(u, v) * 4.0f) - 0.5f;
            const float d = glm::length(c);
            glm::vec3 n(0, 0, 1);
            if (d < 0.4f) n = glm::normalize(glm::vec3(c * 2.0f, std::sqrt(1.0f - d * d * 4.0f)));
            return glm::vec4(n * 0.5f + 0.5f, 1.0f);
        }));
        // Metal/roughness: smooth metal stripes on a rough plastic (G roughness, B metal).
        const auto stripes = upload(texture(false, [](float u, float) {
            const bool metal = int(u * 6) % 2;
            return metal ? glm::vec4(0, 0.15f, 1, 1) : glm::vec4(0, 0.9f, 0, 1);
        }));
        // Occlusion: dark towards the face's edges.
        const auto occlusion = upload(texture(false, [](float u, float v) {
            const float e = std::min(std::min(u, 1 - u), std::min(v, 1 - v)) * 5.0f;
            return glm::vec4(glm::vec3(std::min(1.0f, e)), 1.0f);
        }));
        // Emissive: a glowing grid.
        const auto grid = upload(texture(true, [](float u, float v) {
            const bool line = glm::fract(u * 4) < 0.25f || glm::fract(v * 4) < 0.25f;
            return line ? glm::vec4(1.0f, 0.55f, 0.1f, 1) : glm::vec4(0, 0, 0, 1);
        }));
        render::MaterialTextures t;
        t.base_color = checker;
        base_ = r.create_material(t);
        t = {};
        t.normal = bumps;
        normal_ = r.create_material(t);
        t = {};
        t.metallic_roughness = stripes;
        stripes_ = r.create_material(t);
        t = {};
        t.occlusion = occlusion;
        occlusion_ = r.create_material(t);
        t = {};
        t.emissive = grid;
        emissive_ = r.create_material(t);
    }
    std::vector<DrawItem> draws(const glm::dvec3& origin) const override {
        const float x[5] = {-3.2f, -1.6f, 0.0f, 1.6f, 3.2f};
        std::vector<DrawItem> out(5);
        out[0] = {cube_, at({x[0], 0, 0})};
        out[0].material = base_;
        out[1] = {sphere_, at({x[1], 0, 0}, glm::vec3(1.2f)), glm::vec4(0.7f, 0.7f, 0.7f, 1)};
        out[1].material = normal_;
        out[1].roughness = 0.4f;
        out[2] = {sphere_, at({x[2], 0, 0}, glm::vec3(1.2f)), glm::vec4(0.9f, 0.75f, 0.4f, 1)};
        out[2].material = stripes_;
        out[2].metallic = 1.0f;
        out[2].roughness = 1.0f;
        out[3] = {cube_, at({x[3], 0, 0}), glm::vec4(0.8f, 0.8f, 0.8f, 1)};
        out[3].material = occlusion_;
        out[4] = {cube_, at({x[4], 0, 0}), glm::vec4(0.05f, 0.05f, 0.05f, 1)};
        out[4].material = emissive_;
        out[4].emissive = glm::vec3(1.0f);
        out[4].emissive_strength = 3.0f;
        // The cubes turn a little, so two faces show.
        for (int i : {0, 3, 4})
            out[size_t(i)].model =
                glm::rotate(out[size_t(i)].model, glm::radians(30.0f), glm::vec3(0, 1, 0));
        return relative(out, origin);
    }
    render::Camera camera(float aspect) const override {
        return look({0, 0.6f, 6.0f}, {0, 0, 0}, aspect, 50.0f);
    }
    render::Light light(const glm::dvec3&) const override {
        render::Light l;
        l.direction = glm::normalize(glm::vec3(-0.6f, -0.7f, -0.8f));
        return l;
    }
    glm::vec4 sky() const override { return {0.12f, 0.13f, 0.16f, 1.0f}; }

private:
    bool compressed_;
    render::MeshId cube_ = 0, sphere_ = 0;
    render::MaterialId base_ = 0, normal_ = 0, stripes_ = 0, occlusion_ = 0, emissive_ = 0;
};

class Lights : public TestScene {
public:
    void upload(render::Renderer& r) override {
        cube_ = r.upload_mesh(render::make_cube());
        sphere_ = r.upload_mesh(render::make_sphere());
    }
    std::vector<DrawItem> draws(const glm::dvec3& origin) const override {
        std::vector<DrawItem> out;
        out.push_back({cube_, at({0, -0.05f, 0}, {12, 0.1f, 12}), glm::vec4(0.6f, 0.6f, 0.6f, 1)});
        for (int i = 0; i < 3; ++i) {
            DrawItem d{sphere_, at({(i - 1) * 1.8f, 0.5f, 0.0f}), glm::vec4(0.9f, 0.9f, 0.9f, 1)};
            d.roughness = 0.3f + 0.3f * i;
            out.push_back(d);
        }
        return relative(out, origin);
    }
    render::Camera camera(float aspect) const override {
        return look({0, 4.5f, 7.0f}, {0, 0, 0}, aspect, 50.0f);
    }
    render::Light light(const glm::dvec3& origin) const override {
        render::Light l;
        l.intensity = 0.05f;  // dim: the local lights do the work
        l.ambient = glm::vec3(0.04f);
        l.ambient_ground = glm::vec3(0.02f);
        render::LocalLight red;
        red.position = glm::vec3(glm::dvec3(-1.8, 1.5, 1.2) - origin);
        red.color = {1.0f, 0.25f, 0.2f};
        red.intensity = 4.0f;
        red.range = 6.0f;
        render::LocalLight blue;
        blue.position = glm::vec3(glm::dvec3(2.2, 3.5, 0.5) - origin);
        blue.color = {0.3f, 0.5f, 1.0f};
        blue.intensity = 12.0f;
        blue.range = 9.0f;
        blue.spot = true;
        blue.direction = glm::normalize(glm::vec3(-0.2f, -1.0f, -0.1f));
        blue.inner_angle = 18.0f;
        blue.outer_angle = 28.0f;
        l.local = {red, blue};
        return l;
    }
    glm::vec4 sky() const override { return {0.02f, 0.02f, 0.03f, 1.0f}; }

private:
    render::MeshId cube_ = 0, sphere_ = 0;
};

// Sun shadows on flat ground: pillars from 2 to 45 metres away (through every cascade), a
// sphere, and a plank in the air whose shadow lies apart from it.
class Shadows : public TestScene {
public:
    void upload(render::Renderer& r) override {
        cube_ = r.upload_mesh(render::make_cube());
        sphere_ = r.upload_mesh(render::make_sphere());
    }
    std::vector<DrawItem> draws(const glm::dvec3& origin) const override {
        std::vector<DrawItem> out;
        out.push_back(
            {cube_, at({0, -0.05f, -20}, {120, 0.1f, 120}), glm::vec4(0.6f, 0.6f, 0.58f, 1)});
        const float depths[] = {-2, -6, -12, -20, -32, -45};
        for (int i = 0; i < 6; ++i)
            out.push_back({cube_, at({i % 2 ? 2.5f : -2.5f, 1.0f, depths[i]}, {0.5f, 2.0f, 0.5f}),
                           glm::vec4(0.75f, 0.45f, 0.3f, 1)});
        out.push_back({sphere_, at({0.6f, 0.5f, -1.0f}), glm::vec4(0.3f, 0.5f, 0.8f, 1)});
        out.push_back(
            {cube_, at({-1.0f, 1.4f, -3.5f}, {2.5f, 0.1f, 0.8f}), glm::vec4(0.4f, 0.7f, 0.4f, 1)});
        return relative(out, origin);
    }
    render::Camera camera(float aspect) const override {
        return look({0, 2.0f, 4.0f}, {0, 0, -12.0f}, aspect, 55.0f);
    }
    render::Light light(const glm::dvec3&) const override {
        render::Light l;
        l.direction = glm::normalize(glm::vec3(0.8f, -0.6f, 0.35f));  // a low sun from the left
        return l;
    }
    glm::vec4 sky() const override { return {0.35f, 0.45f, 0.6f, 1.0f}; }

private:
    render::MeshId cube_ = 0, sphere_ = 0;
};

// Sun shadows on a small planet (radius 60 m), standing where "up" leans 50 degrees from the
// world's y axis: objects stand along the local up, and their shadows fall across the curved
// ground just as they would at the top.
class ShadowsCurved : public TestScene {
public:
    void upload(render::Renderer& r) override {
        cube_ = r.upload_mesh(render::make_cube());
        sphere_ = r.upload_mesh(render::make_sphere());
        planet_ = r.upload_mesh(render::make_sphere(128, 256));
    }
    std::vector<DrawItem> draws(const glm::dvec3& origin) const override {
        std::vector<DrawItem> out;
        out.push_back(
            {planet_, at({0, 0, 0}, glm::vec3(2.0f * kRadius)), glm::vec4(0.55f, 0.6f, 0.45f, 1)});
        // (east, south) on the ground: pillars, a ball and a floating plank.
        const glm::vec2 pillars[] = {{-3, -2}, {0, -5}, {3, -8}, {-2, -12}};
        for (const glm::vec2& p : pillars)
            out.push_back(
                {cube_, stand(p, 1.25f, {0.6f, 2.5f, 0.6f}), glm::vec4(0.75f, 0.45f, 0.3f, 1)});
        out.push_back({sphere_, stand({1.5f, 0.0f}, 0.5f), glm::vec4(0.3f, 0.5f, 0.8f, 1)});
        out.push_back({cube_, stand({-1.0f, -3.0f}, 1.5f, {2.5f, 0.1f, 0.8f}),
                       glm::vec4(0.4f, 0.7f, 0.4f, 1)});
        return relative(out, origin);
    }
    render::Camera camera(float aspect) const override {
        const glm::vec3 ground = up() * kRadius;
        const glm::vec3 eye = ground + up() * 2.5f + south() * 7.0f;
        render::Camera c;
        c.origin = glm::dvec3(eye);
        c.view = glm::lookAt(glm::vec3(0.0f), ground - south() * 6.0f - eye, up());
        c.projection = render::perspective(glm::radians(55.0f), aspect, 0.05f, 300.0f);
        return c;
    }
    render::Light light(const glm::dvec3&) const override {
        // The sun 35 degrees above the local horizon, from the east and a little south.
        const float elevation = glm::radians(35.0f);
        const glm::vec3 to_sun = glm::normalize(
            std::cos(elevation) * (0.8f * east() + 0.6f * south()) + std::sin(elevation) * up());
        render::Light l;
        l.direction = -to_sun;
        l.up = up();
        return l;
    }
    glm::vec4 sky() const override { return {0.35f, 0.45f, 0.6f, 1.0f}; }

private:
    static constexpr float kRadius = 60.0f;
    // The camera's local frame: up, and east and south along the ground.
    static glm::vec3 up() {
        const float a = glm::radians(50.0f);
        return {std::sin(a), std::cos(a), 0.0f};
    }
    static glm::vec3 south() { return {0.0f, 0.0f, 1.0f}; }
    static glm::vec3 east() { return glm::cross(up(), south()); }

    // An object standing on the ground at (east, south) metres from the camera's spot, its
    // centre `height` above the ground, upright along the up there.
    static glm::mat4 stand(glm::vec2 at_ground, float height, glm::vec3 scale = glm::vec3(1.0f)) {
        const glm::vec3 local_up =
            glm::normalize(up() * kRadius + east() * at_ground.x + south() * at_ground.y);
        const glm::vec3 local_east = glm::normalize(glm::cross(local_up, south()));
        const glm::vec3 local_south = glm::cross(local_east, local_up);
        glm::mat4 m(1.0f);
        m[0] = glm::vec4(local_east * scale.x, 0.0f);
        m[1] = glm::vec4(local_up * scale.y, 0.0f);
        m[2] = glm::vec4(local_south * scale.z, 0.0f);
        m[3] = glm::vec4(local_up * (kRadius + height), 1.0f);
        return m;
    }

    render::MeshId cube_ = 0, sphere_ = 0, planet_ = 0;
};

// The analytic sky over a wide plain with pillars out to 300 m. Sky, shadows and haze use the
// same sun, given by its height and its direction along the ground.
class Sky : public TestScene {
public:
    // elevation and azimuth (from -z towards +x) of the sun, in degrees; the fog's density at
    // the ground and how fast it thins upwards; where the camera stands and looks.
    struct Setup {
        float elevation, azimuth, density, falloff;
        glm::vec3 eye, target;
    };
    explicit Sky(const Setup& s)
        : setup_(s), sky_(render::analytic_sky(glm::radians(s.elevation))) {}
    void upload(render::Renderer& r) override {
        cube_ = r.upload_mesh(render::make_cube());
        sphere_ = r.upload_mesh(render::make_sphere());
    }
    std::vector<DrawItem> draws(const glm::dvec3& origin) const override {
        std::vector<DrawItem> out;
        out.push_back(
            {cube_, at({0, -0.05f, 0}, {4000, 0.1f, 4000}), glm::vec4(0.45f, 0.5f, 0.35f, 1)});
        const float distances[] = {8, 20, 50, 120, 300};
        for (int i = 0; i < 5; ++i) {
            const float d = distances[i];
            const float h = 2.0f + d * 0.04f;
            out.push_back({cube_,
                           at({(i % 2 ? 1.0f : -1.0f) * (2.0f + d * 0.2f), h * 0.5f, -d},
                              {0.8f + d * 0.01f, h, 0.8f + d * 0.01f}),
                           glm::vec4(0.75f, 0.45f, 0.3f, 1)});
        }
        out.push_back({sphere_, at({1.2f, 0.5f, -4.0f}), glm::vec4(0.8f, 0.8f, 0.8f, 1)});
        return relative(out, origin);
    }
    render::Camera camera(float aspect) const override {
        render::Camera c;
        c.origin = glm::dvec3(setup_.eye);
        c.view = glm::lookAt(glm::vec3(0.0f), setup_.target - setup_.eye, glm::vec3(0, 1, 0));
        c.projection = render::perspective(glm::radians(60.0f), aspect, 0.1f, 2000.0f);
        return c;
    }
    render::Light light(const glm::dvec3&) const override {
        render::Light l;
        const float e = glm::radians(setup_.elevation), a = glm::radians(setup_.azimuth);
        const glm::vec3 to_sun(std::cos(e) * std::sin(a), std::sin(e), -std::cos(e) * std::cos(a));
        l.direction = -to_sun;
        l.color = sky_.sunlight;
        l.intensity = sky_.sunlight_intensity;
        // Ambient light from the sky: dimmer as the sun goes down.
        const float day = std::clamp(sky_.colors.brightness, 0.05f, 1.0f);
        l.ambient *= day;
        l.ambient_ground *= day;
        return l;
    }
    glm::vec4 sky() const override { return {0.0f, 0.0f, 0.0f, 1.0f}; }
    render::Environment environment() const override {
        render::Environment env;
        env.sky = true;
        env.sky_colors = sky_.colors;
        env.fog.density = setup_.density;
        env.fog.height_falloff = setup_.falloff;
        env.fog.camera_height = setup_.eye.y;
        return env;
    }

private:
    Setup setup_;
    render::AnalyticSky sky_;
    render::MeshId cube_ = 0, sphere_ = 0;
};

// Collider outlines (drawn with --debug-physics): a scene built from components like a game's,
// with one of each kind of body, a trigger and the player.
class Colliders : public TestScene {
public:
    Colliders() {
        const auto put = [&](const char* id, const char* json) {
            scene_.put_entity(id, id, "", nlohmann::json::parse(json));
        };
        put("e1_ground", R"({"transform": {"position": [0, -0.1, 0], "scale": [12, 0.2, 8]},
            "mesh": {"asset": "cube"}, "collider": {}})");
        put("e2_crate", R"({"transform": {"position": [-3, 0.5, 0], "rotation": [0, 25, 0]},
            "mesh": {"asset": "cube"}, "collider": {}, "rigidbody": {}})");
        put("e3_ball", R"({"transform": {"position": [-1.2, 0.5, 1]}, "mesh": {"asset": "sphere"},
            "collider": {"shape": "sphere"}, "rigidbody": {}})");
        put("e4_lift", R"({"transform": {"position": [1, 0.6, -1]}, "collider":
            {"shape": "capsule", "radius": 0.4, "height": 1.2}, "rigidbody": {"motion": "kinematic"}})");
        put("e5_zone",
            R"({"transform": {"position": [3.2, 1, 0]}, "trigger": {"size": [1.6, 2, 1.6]}})");
        put("e6_rock", R"({"transform": {"position": [-0.6, 0.5, -2], "rotation": [20, 40, 10]},
            "mesh": {"asset": "cube"}, "collider": {"shape": "convex"}})");
        put("e7_player", R"({"transform": {"position": [1.2, 0.5, 1.6]}, "player": {}})");
    }
    void upload(render::Renderer& r) override {
        meshes_["cube"] = r.upload_mesh(cube_);
        meshes_["sphere"] = r.upload_mesh(render::make_sphere());
    }
    std::vector<DrawItem> draws(const glm::dvec3& origin) const override {
        std::vector<DrawItem> out;
        for (entt::entity e : scene_.registry.view<const MeshRef>())
            out.push_back(
                scene_.draw_item(e, meshes_.at(scene_.registry.get<MeshRef>(e).asset), origin));
        return out;
    }
    std::vector<render::DebugLine> debug_lines(const glm::dvec3& origin) const override {
        return collider_outlines(scene_, origin, [this](const std::string& asset) {
            return asset == "cube" ? &cube_ : nullptr;
        });
    }
    render::Camera camera(float aspect) const override {
        return look({4.5f, 4.0f, 7.0f}, {0, 0.3f, 0}, aspect, 50.0f);
    }
    render::Light light(const glm::dvec3&) const override { return {}; }
    glm::vec4 sky() const override { return {0.1f, 0.11f, 0.14f, 1.0f}; }

private:
    Scene scene_;
    render::MeshData cube_ = render::make_cube();
    std::unordered_map<std::string, render::MeshId> meshes_;
};

// A day on each world shape (milestone 5.8): a scene built from components like a game's, its
// Sky linked to an empty environment (the analytic sky), its sun moved by the world's light
// model to a time of day. The camera stands at the world's origin looking towards the dawn sun,
// a little up, so dawn shows the sun rising ahead, noon the sky lit from above, dusk the light
// going behind and midnight the night.
class Day : public TestScene {
public:
    Day(const std::string& shape, double phase) {
        nlohmann::json body;
        if (shape == "area")
            body = {{"shape", "bounded_area"}, {"width", 4000}, {"length", 4000}};
        else if (shape == "flat_cylinder")
            body = {{"shape", "flat_cylinder"}, {"circumference", 6000}, {"length", 6000}};
        else if (shape == "planet")
            body = {{"shape", "sphere_outside"}, {"radius", 1000}, {"origin", {0, -1000, 0}}};
        else if (shape == "hollow")
            body = {{"shape", "sphere_inside"}, {"radius", 1000}, {"origin", {0, 1000, 0}}};
        else
            body = {{"shape", "curved_cylinder"},
                    {"radius", 1000},
                    {"length", 5000},
                    {"origin", {0, 1000, 0}}};
        body["id"] = shape;
        planet_ = shape == "planet";
        scene_.settings.body = body_from_json(body);
        scene_.environments.emplace("env_earth",
                                    environment_from_json("env_earth", "Earth-like", "{}", "{}"));
        scene_.put_entity("e_sky", "Sky", "",
                          nlohmann::json::parse(R"({"sky": {"environment": "env_earth"}})"));
        // As placed, the sun stands 50 degrees up towards +z (a little to +x) at noon.
        scene_.put_entity("e_sun", "Sun", "", nlohmann::json::parse(R"({"transform":
            {"rotation": [-50, 20, 0]}, "light": {"type": "directional"}})"));

        // The camera: at eye height, looking towards where the sun rises, 8 degrees up.
        up_ = glm::vec3(scene_.settings.body->up(glm::dvec3(0.0)));
        eye_ = up_ * 1.7f;
        scene_.solar_phase = 0.03;
        const glm::vec3 to_dawn = glm::vec3(scene_.local_sun(glm::dvec3(eye_))->to_sun);
        glm::vec3 ahead = to_dawn - up_ * glm::dot(to_dawn, up_);
        ahead = glm::length(ahead) > 1e-4f ? glm::normalize(ahead) : glm::vec3(0, 0, -1);
        ahead_ = ahead;
        const float pitch = glm::radians(8.0f);
        forward_ = std::cos(pitch) * ahead + std::sin(pitch) * up_;
        scene_.solar_phase = phase;
    }
    void upload(render::Renderer& r) override {
        cube_ = r.upload_mesh(render::make_cube());
        sphere_ = r.upload_mesh(render::make_sphere());
        if (planet_) ground_ = r.upload_mesh(render::make_sphere(128, 256));
    }
    std::vector<DrawItem> draws(const glm::dvec3& origin) const override {
        std::vector<DrawItem> out;
        const glm::vec4 grass(0.45f, 0.5f, 0.35f, 1);
        if (planet_)
            out.push_back({ground_, at({0, -1000, 0}, glm::vec3(2000.0f)), grass});
        else
            out.push_back({cube_, at({0, -0.05f, 0}, {800, 0.1f, 800}), grass});
        // Pillars ahead and to the sides, and a pale ball, to catch the light and cast shadows.
        const glm::vec3 side = glm::normalize(glm::cross(ahead_, up_));
        const float distances[] = {6, 14, 30, 60};
        for (int i = 0; i < 4; ++i) {
            const float d = distances[i];
            const glm::vec3 p = ahead_ * d + side * ((i % 2 ? 1.0f : -1.0f) * (2.0f + d * 0.25f));
            out.push_back(
                {cube_, at(p + up_ * (1.0f + d * 0.02f), glm::vec3(0.8f, 2.0f + d * 0.04f, 0.8f)),
                 glm::vec4(0.75f, 0.45f, 0.3f, 1)});
        }
        out.push_back({sphere_, at(ahead_ * 4.0f + side * 0.8f + up_ * 0.5f),
                       glm::vec4(0.8f, 0.8f, 0.8f, 1)});
        return relative(out, origin);
    }
    render::Camera camera(float aspect) const override {
        render::Camera c;
        c.origin = glm::dvec3(eye_);
        c.view = glm::lookAt(glm::vec3(0.0f), forward_, up_);
        c.projection = render::perspective(glm::radians(70.0f), aspect, 0.1f, 3000.0f);
        return c;
    }
    render::Light light(const glm::dvec3& origin) const override { return scene_.light(origin); }
    glm::vec4 sky() const override { return {scene_.settings.sky, 1.0f}; }
    render::Environment environment() const override {
        return scene_.environment(glm::dvec3(eye_));
    }

private:
    Scene scene_;
    bool planet_ = false;
    glm::vec3 up_{0, 1, 0}, eye_{0, 1.7f, 0}, ahead_{0, 0, -1}, forward_{0, 0, -1};
    render::MeshId cube_ = 0, sphere_ = 0, ground_ = 0;
};

// Terrain (milestone 5.9) at noon on the Earth-like environment: a bounded area seen from a
// hill above it (land, then the ocean band at its edge), a flat-wrap cylinder at one of its end
// walls, and a habitat seen along its length with the far side overhead.
class TerrainScene : public TestScene {
public:
    explicit TerrainScene(const std::string& which) {
        nlohmann::json body, terrain = nlohmann::json::object();
        if (which == "winter") summer_ = -1.0;  // deep winter: snow down to the shore
        if (which == "autumn") autumn_ = 1.0;   // between summer and winter, the leaves turning
        if (which == "area" || which == "shore" || which == "winter" || which == "autumn") {
            body = {{"shape", "bounded_area"}, {"width", 3000}, {"length", 3000}, {"seed", 7}};
            eye_ = {-600.0, 160.0, -600.0};
            look_ = {-1000.0, 0.0, -1500.0};
            if (which == "shore") {  // low over the sea by the coast, the shallows below
                eye_ = {-1180.0, 4.0, -300.0};
                look_ = {-1000.0, 0.0, -260.0};
            }
        } else if (which == "planet_biomes") {
            // A 3 km planet with a climate and four biomes, from 5 km up: desert and savanna
            // round the equator, forest between, tundra towards the poles, ice where it is
            // below freezing (milestone 5.11).
            body = {{"shape", "sphere_outside"},
                    {"radius", 3000},
                    {"seed", 11},
                    {"origin", {0, -3000, 0}}};
            terrain = nlohmann::json::parse(R"({"height_scale": 90,
                "climate": {"temperature": 30, "latitude_cooling": 55, "lapse_rate": 30,
                            "variation": 3, "season_swing": 10},
                "biomes": [
                  {"name": "Desert", "temperature": [24, 80], "moisture": [0, 0.5],
                   "color": [0.80, 0.68, 0.42], "dry_color": [0.86, 0.74, 0.50]},
                  {"name": "Savanna", "temperature": [20, 80], "moisture": [0.5, 1],
                   "color": [0.50, 0.55, 0.22], "dry_color": [0.66, 0.60, 0.30]},
                  {"name": "Forest", "temperature": [6, 20], "moisture": [0, 1],
                   "color": [0.13, 0.30, 0.12], "dry_color": [0.30, 0.38, 0.17]},
                  {"name": "Tundra", "temperature": [-60, 6], "moisture": [0, 1],
                   "color": [0.45, 0.47, 0.40], "dry_color": [0.55, 0.52, 0.45]}]})");
        } else if (which == "planet" || which == "planet_low") {
            // A 3 km planet: from low orbit over a cube corner, where three faces meet; or
            // low over the ground by the edge between faces +X and +Y, looking across it.
            body = {{"shape", "sphere_outside"},
                    {"radius", 3000},
                    {"seed", 11},
                    {"origin", {0, -3000, 0}}};
        } else if (which == "hollow_far") {
            // Inside an 8 km hollow world, looking up: the far side overhead, 16 km away, in a
            // few coarse chunks, softened by the inside-world haze.
            body = {{"shape", "sphere_inside"},
                    {"radius", 8000},
                    {"seed", 12},
                    {"origin", {0, 8000, 0}}};
        } else if (which == "hollow_light") {
            // Inside a 1 km hollow world, looking up at its light sphere at the centre.
            body = {{"shape", "sphere_inside"},
                    {"radius", 1000},
                    {"seed", 12},
                    {"origin", {0, 1000, 0}}};
        } else if (which == "hollow") {
            // Inside a 2.5 km hollow world, by the edge between faces -Y and +Z.
            body = {{"shape", "sphere_inside"},
                    {"radius", 2500},
                    {"seed", 12},
                    {"origin", {0, 2500, 0}}};
        } else if (which == "strip") {
            body = {
                {"shape", "flat_cylinder"}, {"circumference", 6000}, {"length", 5000}, {"seed", 8}};
            terrain = {{"wall", "fortification"}};
            eye_ = {0.0, 150.0, -2000.0};
            look_ = {300.0, 120.0, -2500.0};
        } else {
            body = {{"shape", "curved_cylinder"},
                    {"radius", 1000},
                    {"length", 5000},
                    {"seed", 9},
                    {"origin", {0, 1000, 0}}};
            terrain = {{"wall", "barrier"}};
            eye_ = {0.0, 40.0, 0.0};
            look_ = {0.0, 250.0, -1500.0};
        }
        body["id"] = which;
        scene_.settings.body = body_from_json(body);
        scene_.settings.terrain = terrain_from_json(terrain);
        if (which == "habitat_rod") {  // on the floor, looking up at the light rod on the axis
            const Terrain t(*scene_.settings.body, *scene_.settings.terrain);
            eye_ = t.ground({0.0, 0.0, 0.0}) + glm::dvec3(0.0, 1.7, 0.0);
            look_ = {30.0, 1000.0, -200.0};
        }
        if (which == "hollow_light") {  // on the floor, looking up at the light sphere
            const Terrain t(*scene_.settings.body, *scene_.settings.terrain);
            eye_ = t.ground({0.0, 0.0, 0.0}) + glm::dvec3(0.0, 1.7, 0.0);
            look_ = t.body().origin + glm::dvec3(0.0, 0.0, -150.0);
        }
        if (which == "habitat_ground") {  // on the floor, looking ahead and a little down
            const Terrain t(*scene_.settings.body, *scene_.settings.terrain);
            eye_ = t.ground({0.0, 0.0, 0.0}) + glm::dvec3(0.0, 1.7, 0.0);
            look_ = t.ground({0.0, 0.0, -60.0});
        }
        if (which == "planet" || which == "planet_low" || which == "hollow" ||
            which == "hollow_far" || which == "planet_biomes") {
            const Terrain t(*scene_.settings.body, *scene_.settings.terrain);
            // A point on the ground (in a direction from the centre) and a height above it.
            const auto above = [&](const glm::dvec3& direction, double height) {
                const glm::dvec3 g =
                    t.ground(t.body().origin + glm::normalize(direction) * t.body().shape.radius());
                return g + glm::dvec3(t.body().up(g)) * height;
            };
            if (which == "planet_biomes") {
                eye_ = above({1.0, 0.35, -0.3}, 5000.0);
                look_ = t.body().origin + glm::dvec3(0.0, 400.0, 0.0);
            } else if (which == "planet") {
                eye_ = above({1.0, 1.25, -0.55}, 1400.0);
                look_ = above({1.0, 1.0, -1.0}, 0.0);
            } else if (which == "planet_low") {
                eye_ = above({1.0, 0.86, 0.12}, 30.0);
                look_ = above({1.0, 1.12, 0.12}, 0.0);
            } else if (which == "hollow_far") {
                eye_ = above({0.0, -1.0, 0.0}, 30.0);
                look_ = above({0.0, 0.6, 1.0}, 0.0);
            } else {
                eye_ = above({0.0, -1.0, 0.94}, 40.0);
                look_ = above({0.0, -0.2, 1.0}, 0.0);
            }
        }
        // Inside a habitat: the Environment editor's Inside-World Atmosphere preset, haze that
        // softens the far side overhead: 1.2 / radius per metre, as the editor's preset sets it
        // for the world's size (environments.inside_atmosphere).
        const bool inside = which == "habitat" || which == "habitat_ground" ||
                            which == "habitat_rod" || which == "hollow" || which == "hollow_far" ||
                            which == "hollow_light";
        nlohmann::json curves = nlohmann::json::object();
        if (inside) {
            curves = nlohmann::json::parse(R"({"fog_color": [[0.0, [0.55, 0.6, 0.66]],
                [0.25, [0.62, 0.7, 0.8]], [0.5, [0.55, 0.5, 0.5]], [0.75, [0.05, 0.06, 0.09]]]})");
            curves["fog_density"] = {{0.0, 1.2 / scene_.settings.body->shape.radius()}};
        }
        scene_.environments.emplace(
            "env_earth",
            environment_from_json(
                "env_earth", "Earth-like", curves.dump(),
                inside ? R"({"fog_height_falloff": 0.0, "fog_sun_scatter": 0.8})" : "{}"));
        scene_.put_entity("e_sky", "Sky", "",
                          nlohmann::json::parse(R"({"sky": {"environment": "env_earth"}})"));
        scene_.put_entity("e_sun", "Sun", "", nlohmann::json::parse(R"({"transform":
            {"rotation": [-50, 30, 0]}, "light": {"type": "directional"}})"));
        scene_.solar_phase = 0.2;
        streamer_ =
            std::make_unique<TerrainStreamer>(*scene_.settings.body, *scene_.settings.terrain);
        up_ = glm::vec3(scene_.settings.body->up(eye_));
        if (which == "hollow_far" || which == "planet_biomes") far_ = 20000.0f;
    }
    void upload(render::Renderer& renderer) override { light_.upload(renderer); }
    void stream(render::Renderer& renderer, const glm::dvec3& origin) override {
        streamer_->update(origin, &renderer, true);
    }
    std::vector<DrawItem> draws(const glm::dvec3& origin) const override {
        streamer_->set_season(summer_, autumn_);
        std::vector<DrawItem> out = streamer_->draws(origin);
        if (const auto light = light_.draw(scene_, origin)) out.push_back(*light);  // rod, sphere
        return out;
    }
    render::Camera camera(float aspect) const override {
        render::Camera c;
        c.origin = eye_;
        c.view = glm::lookAt(glm::vec3(0.0f), glm::vec3(look_ - eye_), up_);
        c.projection = render::perspective(glm::radians(65.0f), aspect, 0.5f, far_);
        return c;
    }
    render::Light light(const glm::dvec3& origin) const override { return scene_.light(origin); }
    glm::vec4 sky() const override { return {scene_.settings.sky, 1.0f}; }
    render::Environment environment() const override { return scene_.environment(eye_); }
    render::TerrainLook terrain_look() const override { return streamer_->look(); }

private:
    Scene scene_;
    std::unique_ptr<TerrainStreamer> streamer_;
    WorldLight light_;
    glm::dvec3 eye_{0.0}, look_{0.0, 0.0, -1.0};
    glm::vec3 up_{0, 1, 0};
    double summer_ = 0.0, autumn_ = 0.0;
    float far_ = 8000.0f;  // the far plane (inside a big hollow world, past its far side)
};

const char* const kDayShapes[] = {"area", "flat_cylinder", "planet", "hollow", "habitat"};
const std::pair<const char*, double> kDayTimes[] = {
    {"dawn", 0.02}, {"noon", 0.25}, {"dusk", 0.48}, {"midnight", 0.75}};

}  // namespace

std::vector<std::string> names() {
    std::vector<std::string> out = {"materials", "textures",   "compressed",
                                    "lights",    "shadows",    "shadows_curved",
                                    "sky_day",   "sky_sunset", "colliders"};
    for (const char* shape : kDayShapes)
        for (const auto& [time, phase] : kDayTimes)
            out.push_back(std::string("day_") + shape + "_" + time);
    for (const char* which :
         {"terrain_area", "terrain_strip", "terrain_habitat", "terrain_shore", "terrain_planet",
          "terrain_planet_low", "terrain_hollow", "terrain_hollow_far", "terrain_habitat_ground",
          "terrain_habitat_rod", "terrain_hollow_light", "terrain_planet_biomes", "terrain_winter",
          "terrain_autumn"})
        out.push_back(which);
    return out;
}

std::unique_ptr<TestScene> make(const std::string& name) {
    if (name == "materials") return std::make_unique<Materials>();
    if (name == "textures") return std::make_unique<Textures>();
    if (name == "compressed") return std::make_unique<Textures>(true);
    if (name == "lights") return std::make_unique<Lights>();
    if (name == "shadows") return std::make_unique<Shadows>();
    if (name == "colliders") return std::make_unique<Colliders>();
    if (name == "shadows_curved") return std::make_unique<ShadowsCurved>();
    // By day: the sun high behind the camera's right shoulder, a thin haze everywhere.
    if (name == "sky_day")
        return std::make_unique<Sky>(
            Sky::Setup{35.0f, 140.0f, 0.004f, 0.02f, {0, 1.7f, 0}, {0, 1.2f, -20}});
    // At sunset: the sun ahead just above the horizon, fog lying low over the plain.
    if (name == "sky_sunset")
        return std::make_unique<Sky>(
            Sky::Setup{3.0f, -15.0f, 0.02f, 0.6f, {0, 3.0f, 0}, {0, 2.2f, -20}});
    if (name == "terrain_area") return std::make_unique<TerrainScene>("area");
    if (name == "terrain_strip") return std::make_unique<TerrainScene>("strip");
    if (name == "terrain_habitat") return std::make_unique<TerrainScene>("habitat");
    if (name == "terrain_shore") return std::make_unique<TerrainScene>("shore");
    if (name == "terrain_planet") return std::make_unique<TerrainScene>("planet");
    if (name == "terrain_planet_low") return std::make_unique<TerrainScene>("planet_low");
    if (name == "terrain_hollow") return std::make_unique<TerrainScene>("hollow");
    if (name == "terrain_hollow_far") return std::make_unique<TerrainScene>("hollow_far");
    if (name == "terrain_habitat_ground") return std::make_unique<TerrainScene>("habitat_ground");
    if (name == "terrain_habitat_rod") return std::make_unique<TerrainScene>("habitat_rod");
    if (name == "terrain_hollow_light") return std::make_unique<TerrainScene>("hollow_light");
    if (name == "terrain_planet_biomes") return std::make_unique<TerrainScene>("planet_biomes");
    if (name == "terrain_winter") return std::make_unique<TerrainScene>("winter");
    if (name == "terrain_autumn") return std::make_unique<TerrainScene>("autumn");
    for (const char* shape : kDayShapes)
        for (const auto& [time, phase] : kDayTimes)
            if (name == std::string("day_") + shape + "_" + time)
                return std::make_unique<Day>(shape, phase);
    return nullptr;
}

}  // namespace midgard::test_scenes
