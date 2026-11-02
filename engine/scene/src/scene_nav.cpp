// SPDX-License-Identifier: Apache-2.0
#include "midgard/scene_nav.h"

#include <glm/gtc/matrix_transform.hpp>

namespace midgard {

namespace {

// A box's twelve triangles (counter-clockwise from outside), through `m`.
void add_box(NavGeometry& g, const glm::dmat4& m, const glm::dvec3& origin) {
    const auto corner = [&](int i) {
        const glm::dvec4 local(i & 1 ? 0.5 : -0.5, i & 2 ? 0.5 : -0.5, i & 4 ? 0.5 : -0.5, 1.0);
        return glm::vec3(glm::dvec3(m * local) - origin);
    };
    const int faces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1},
                             {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
    for (const auto& f : faces) {
        g.add_triangle(corner(f[0]), corner(f[2]), corner(f[1]));
        g.add_triangle(corner(f[0]), corner(f[3]), corner(f[2]));
    }
}

}  // namespace

bool flat_world(const Scene& scene) {
    if (!scene.settings.body) return true;
    const Shape::Kind k = scene.settings.body->shape.kind();
    return k == Shape::Kind::BoundedArea || k == Shape::Kind::FlatCylinder;
}

NavGeometry nav_geometry(const Scene& scene, const glm::dvec3& origin, const MeshLookup& mesh) {
    NavGeometry g;
    const entt::registry& reg = scene.registry;
    for (entt::entity e : reg.view<const ColliderShape, const Transform>()) {
        if (const auto* rb = reg.try_get<RigidBody>(e); rb && rb->motion != physics::Motion::Static)
            continue;  // (things that move aren't scenery)
        const ColliderShape& c = reg.get<ColliderShape>(e);
        const glm::dmat4 world =
            scene.world_matrix(e) * glm::translate(glm::dmat4(1.0), glm::dvec3(c.offset));
        using Kind = physics::Collider::Kind;
        switch (c.kind) {
            case Kind::Box:
                add_box(g, glm::scale(world, glm::dvec3(c.size)), origin);
                break;
            case Kind::Sphere:
                add_box(g, glm::scale(world, glm::dvec3(2.0 * c.radius)), origin);
                break;
            case Kind::Capsule:
                add_box(g, glm::scale(world, glm::dvec3(2.0 * c.radius, c.height, 2.0 * c.radius)),
                        origin);
                break;
            case Kind::Convex:
            case Kind::Mesh: {
                std::string asset = c.mesh;
                if (asset.empty())
                    if (const auto* own = reg.try_get<MeshRef>(e)) asset = own->asset;
                const render::MeshData* data = asset.empty() ? nullptr : mesh(asset);
                if (!data) break;
                const auto at = [&](std::uint32_t i) {
                    const float* p = data->vertices[i].position;
                    return glm::vec3(glm::dvec3(world * glm::dvec4(p[0], p[1], p[2], 1.0)) -
                                     origin);
                };
                for (size_t i = 0; i + 2 < data->indices.size(); i += 3)
                    g.add_triangle(at(data->indices[i]), at(data->indices[i + 1]),
                                   at(data->indices[i + 2]));
                break;
            }
            case Kind::Heightfield: {
                const std::uint32_t n = c.samples;
                if (n < 2 || c.heights.size() < size_t(n) * n) break;
                // Centred on the entity, as the physics heightfield is.
                const double half = 0.5 * double(n - 1) * c.spacing;
                const auto at = [&](std::uint32_t x, std::uint32_t z) {
                    const glm::dvec4 local(double(x) * c.spacing - half, c.heights[z * n + x],
                                           double(z) * c.spacing - half, 1.0);
                    return glm::vec3(glm::dvec3(world * local) - origin);
                };
                for (std::uint32_t z = 0; z + 1 < n; ++z)
                    for (std::uint32_t x = 0; x + 1 < n; ++x) {
                        g.add_triangle(at(x, z), at(x, z + 1), at(x + 1, z));
                        g.add_triangle(at(x + 1, z), at(x, z + 1), at(x + 1, z + 1));
                    }
                break;
            }
        }
    }
    return g;
}

}  // namespace midgard
