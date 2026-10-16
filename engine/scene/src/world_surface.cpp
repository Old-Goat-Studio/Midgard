// SPDX-License-Identifier: Apache-2.0
#include "midgard/world_surface.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace midgard {
namespace {

constexpr double kPi = 3.14159265358979323846;

int cells(double length, double cell, int lowest, int highest) {
    return std::clamp(int(std::ceil(length / cell)), lowest, highest);
}

}  // namespace

render::MeshData surface_mesh(const std::optional<Body>& body) {
    const Body ground =
        body ? *body : Body{"flat", Shape::bounded_area(1000.0, 1000.0, 9.81), 0, glm::dvec3(0.0)};
    const Shape& shape = ground.shape;
    // Cells small enough for the flat triangles to stay near the curve: the gap under a chord
    // c long on a curve of radius R is about c^2 / 8R (13 m chords on a 1 km planet: 2 cm, a
    // little more near the cube faces' corners).
    int across = 1, along = 1;
    switch (shape.kind()) {
        case Shape::Kind::BoundedArea:
        case Shape::Kind::FlatCylinder:
            across = cells(shape.face_size().x, 500.0, 1, 64);
            along = cells(shape.face_size().y, 500.0, 1, 64);
            break;
        case Shape::Kind::CurvedCylinder:  // straight along its length, curved around
            across = cells(2.0 * kPi * shape.radius(), 12.0, 64, 1024);
            along = cells(shape.face_size().y, 250.0, 1, 64);
            break;
        case Shape::Kind::SphereOutside:
        case Shape::Kind::SphereInside:
            across = along = cells(shape.face_size().x, 13.0, 8, 128);
            break;
    }

    render::MeshData mesh;
    for (int f = 0; f < shape.faces(); ++f) {
        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        const glm::dvec2 size = shape.face_size();
        for (int j = 0; j <= along; ++j)
            for (int i = 0; i <= across; ++i) {
                const double u = double(i) / across, v = double(j) / along;
                const glm::vec3 p(ground.position({f, u, v, 0.0}));
                const glm::vec3 n(ground.up(glm::dvec3(p)));
                render::Vertex vert{};
                for (int k = 0; k < 3; ++k) {
                    vert.position[k] = p[k];
                    vert.normal[k] = n[k];
                }
                vert.uv[0] = float(u * size.x / 10.0);  // a texture repeats every 10 m
                vert.uv[1] = float(v * size.y / 10.0);
                mesh.vertices.push_back(vert);
            }
        const auto at = [&](int i, int j) { return base + std::uint32_t(j * (across + 1) + i); };
        const auto point = [&](std::uint32_t k) {
            const float* p = mesh.vertices[k].position;
            return glm::vec3(p[0], p[1], p[2]);
        };
        for (int j = 0; j < along; ++j)
            for (int i = 0; i < across; ++i)
                for (const auto& [a, b, c] :
                     {std::array<std::uint32_t, 3>{at(i, j), at(i + 1, j), at(i + 1, j + 1)},
                      std::array<std::uint32_t, 3>{at(i, j), at(i + 1, j + 1), at(i, j + 1)}}) {
                    // Counter-clockwise seen from above: facing up, into the world.
                    const glm::vec3 facing = glm::cross(point(b) - point(a), point(c) - point(a));
                    const float* n = mesh.vertices[a].normal;
                    const bool up = glm::dot(facing, glm::vec3(n[0], n[1], n[2])) > 0.0f;
                    mesh.indices.insert(mesh.indices.end(), {a, up ? b : c, up ? c : b});
                }
    }
    render::generate_tangents(mesh);
    return mesh;
}

}  // namespace midgard
