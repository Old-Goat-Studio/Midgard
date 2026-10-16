// SPDX-License-Identifier: Apache-2.0
#include "test_scenes.h"

#include <cmath>
#include <cstdint>
#include <functional>

#include <glm/gtc/matrix_transform.hpp>

#include "midgard/render/mesh_data.h"

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
    void upload(render::Renderer& r) override {
        cube_ = r.upload_mesh(render::make_cube());
        sphere_ = r.upload_mesh(render::make_sphere());
        // Base colour: an 8 x 8 checker of two colours.
        const auto checker = r.upload_texture(texture(true, [](float u, float v) {
            const bool odd = (int(u * 8) + int(v * 8)) % 2;
            return odd ? glm::vec4(0.9f, 0.85f, 0.7f, 1) : glm::vec4(0.2f, 0.35f, 0.6f, 1);
        }));
        // Normal map: a 4 x 4 grid of round bumps.
        const auto bumps = r.upload_texture(texture(false, [](float u, float v) {
            const glm::vec2 c = glm::fract(glm::vec2(u, v) * 4.0f) - 0.5f;
            const float d = glm::length(c);
            glm::vec3 n(0, 0, 1);
            if (d < 0.4f) n = glm::normalize(glm::vec3(c * 2.0f, std::sqrt(1.0f - d * d * 4.0f)));
            return glm::vec4(n * 0.5f + 0.5f, 1.0f);
        }));
        // Metal/roughness: smooth metal stripes on a rough plastic (G roughness, B metal).
        const auto stripes = r.upload_texture(texture(false, [](float u, float) {
            const bool metal = int(u * 6) % 2;
            return metal ? glm::vec4(0, 0.15f, 1, 1) : glm::vec4(0, 0.9f, 0, 1);
        }));
        // Occlusion: dark towards the face's edges.
        const auto occlusion = r.upload_texture(texture(false, [](float u, float v) {
            const float e = std::min(std::min(u, 1 - u), std::min(v, 1 - v)) * 5.0f;
            return glm::vec4(glm::vec3(std::min(1.0f, e)), 1.0f);
        }));
        // Emissive: a glowing grid.
        const auto grid = r.upload_texture(texture(true, [](float u, float v) {
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

}  // namespace

std::vector<std::string> names() {
    return {"materials", "textures", "lights"};
}

std::unique_ptr<TestScene> make(const std::string& name) {
    if (name == "materials") return std::make_unique<Materials>();
    if (name == "textures") return std::make_unique<Textures>();
    if (name == "lights") return std::make_unique<Lights>();
    return nullptr;
}

}  // namespace midgard::test_scenes
