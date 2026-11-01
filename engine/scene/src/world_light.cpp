// SPDX-License-Identifier: Apache-2.0
#include "midgard/world_light.h"

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace midgard {

namespace {

constexpr double kTwoPi = 6.283185307179586;

render::Vertex vertex(const glm::vec3& p, const glm::vec3& n, float u, float v) {
    render::Vertex out{};
    out.position[0] = p.x;
    out.position[1] = p.y;
    out.position[2] = p.z;
    out.normal[0] = n.x;
    out.normal[1] = n.y;
    out.normal[2] = n.z;
    out.uv[0] = u;
    out.uv[1] = v;
    out.tangent[0] = 1.0f;
    out.tangent[3] = 1.0f;
    return out;
}

}  // namespace

render::MeshData make_light_rod() {
    constexpr int segments = 32;
    render::MeshData m;
    for (int i = 0; i <= segments; ++i) {
        const double a = kTwoPi * i / segments;
        const glm::vec3 n(float(std::cos(a)), float(std::sin(a)), 0.0f);
        const float u = float(i) / segments;
        m.vertices.push_back(vertex(n * 0.5f + glm::vec3(0.0f, 0.0f, -0.5f), n, u, 0.0f));
        m.vertices.push_back(vertex(n * 0.5f + glm::vec3(0.0f, 0.0f, 0.5f), n, u, 1.0f));
    }
    for (std::uint32_t i = 0; i < segments; ++i) {
        const std::uint32_t a = 2 * i, b = a + 1, c = a + 2, d = a + 3;
        m.indices.insert(m.indices.end(), {a, c, b, b, c, d});  // outwards, counter-clockwise
    }
    return m;
}

double inside_view_distance(const std::optional<Body>& body) {
    if (!body) return 0.0;
    const Shape::Kind kind = body->shape.kind();
    if (kind != Shape::Kind::CurvedCylinder && kind != Shape::Kind::SphereInside) return 0.0;
    return 2.2 * body->shape.radius();
}

void WorldLight::upload(render::Renderer& renderer) {
    if (!rod_) rod_ = renderer.upload_mesh(make_light_rod());
    if (!sphere_) sphere_ = renderer.upload_mesh(render::make_sphere(16, 32));
}

void WorldLight::release(render::Renderer& renderer) {
    if (rod_) renderer.free_mesh(*rod_);
    if (sphere_) renderer.free_mesh(*sphere_);
    rod_.reset();
    sphere_.reset();
}

std::optional<render::DrawItem> WorldLight::draw(const Scene& scene,
                                                 const glm::dvec3& origin) const {
    if (!scene.settings.body) return std::nullopt;
    const Body& body = *scene.settings.body;
    const Shape::Kind kind = body.shape.kind();
    render::DrawItem d;
    const glm::vec3 at(body.origin - origin);
    if (kind == Shape::Kind::CurvedCylinder && rod_) {
        d.mesh = *rod_;
        const float width = float(kLightRodDiameter);
        d.model = glm::scale(glm::translate(glm::mat4(1.0f), at),
                             glm::vec3(width, width, float(body.shape.face_size().y)));
    } else if (kind == Shape::Kind::SphereInside && sphere_) {
        d.mesh = *sphere_;
        // (make_sphere's radius is 0.5.)
        d.model =
            glm::scale(glm::translate(glm::mat4(1.0f), at), glm::vec3(float(kLightSphereDiameter)));
    } else {
        return std::nullopt;
    }
    // As bright as the light where the camera is: by day it glows, by night (behind the shades
    // there) only faintly.
    const render::Light light = scene.light(origin);
    d.color = glm::vec4(0.05f, 0.05f, 0.05f, 1.0f);
    d.emissive = light.color;
    d.emissive_strength = 0.3f + 12.0f * std::max(light.intensity, 0.0f);
    d.roughness = 1.0f;
    d.casts_shadow = false;  // the light itself
    return d;
}

}  // namespace midgard
