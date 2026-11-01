// SPDX-License-Identifier: Apache-2.0
// Terrain collision (midgard/terrain_collision.h): what the physics world collides with is the
// surface that is drawn (heightfields on flat shapes, meshes on curved ones), across a flat
// cylinder's join too, and bodies follow the player without piling up.
#include <doctest/doctest.h>

#include <cmath>
#include <optional>

#include <nlohmann/json.hpp>

#include "midgard/terrain_collision.h"

using json = nlohmann::json;

namespace {

midgard::Body body(const char* text) {
    return midgard::body_from_json(json::parse(text));
}

// Where a ray first meets a drawn chunk's triangles (Moller-Trumbore), as a distance.
std::optional<double> hit_drawn(const midgard::ChunkMesh& m, const glm::dvec3& origin,
                                const glm::dvec3& dir) {
    std::optional<double> best;
    for (size_t t = 0; t < m.indices.size(); t += 3) {
        const glm::dvec3 a = m.origin + glm::dvec3(m.positions[m.indices[t]]);
        const glm::dvec3 b = m.origin + glm::dvec3(m.positions[m.indices[t + 1]]);
        const glm::dvec3 c = m.origin + glm::dvec3(m.positions[m.indices[t + 2]]);
        const glm::dvec3 e1 = b - a, e2 = c - a, pv = glm::cross(dir, e2);
        const double det = glm::dot(e1, pv);
        if (std::abs(det) < 1e-12) continue;
        const glm::dvec3 tv = origin - a;
        const double u = glm::dot(tv, pv) / det;
        if (u < -1e-9 || u > 1 + 1e-9) continue;
        const glm::dvec3 qv = glm::cross(tv, e1);
        const double v = glm::dot(dir, qv) / det;
        if (v < -1e-9 || u + v > 1 + 1e-9) continue;
        const double d = glm::dot(e2, qv) / det;
        if (d > 0 && (!best || d < *best)) best = d;
    }
    return best;
}

// Rays straight down at points round `centre`: the physics ground and the drawn ground agree.
void check_matches(const midgard::Terrain& t, const glm::dvec3& centre) {
    midgard::physics::World world(t.body());
    midgard::TerrainCollision ground(t);
    ground.update(world, {centre});
    REQUIRE(ground.bodies() > 0);
    const int level = t.max_level();
    const glm::dvec2 size = t.body().shape.face_size();
    int checked = 0;
    for (int i = 0; i < 40; ++i) {
        // Points in a spiral out to 50 m.
        const double a = i * 2.39996, r = 1.25 * i;
        const glm::dvec3 up = t.body().up(centre);
        glm::dvec3 east = glm::normalize(glm::cross(up, glm::dvec3(0, 0, 1)));
        if (!std::isfinite(east.x)) east = glm::dvec3(1, 0, 0);
        const glm::dvec3 north = glm::cross(east, up);
        const glm::dvec3 above =
            t.ground(centre + (east * std::cos(a) + north * std::sin(a)) * r) + up * 30.0;
        const glm::dvec3 down = -t.body().up(above);
        const auto hit = world.raycast(above, glm::vec3(down), 100.0);
        REQUIRE(hit.has_value());
        CHECK(hit->user == midgard::kTerrainUser);
        // The drawn chunk under the point.
        const midgard::SurfacePoint s = t.body().surface(above);
        const midgard::ChunkKey key{s.face, level,
                                    int(std::floor(s.u * double(t.roots_u() << level))),
                                    int(std::floor(s.v * double(t.roots_v() << level)))};
        midgard::ChunkMesh m = t.mesh(key);
        if (t.body().shape.kind() == midgard::Shape::Kind::FlatCylinder)
            m.origin.x += std::round((above.x - m.origin.x) / size.x) * size.x;
        const auto drawn = hit_drawn(m, above, down);
        REQUIRE(drawn.has_value());
        CHECK(std::abs(hit->distance - *drawn) < 0.03);  // (heightfields keep 8 bits a block)
        ++checked;
    }
    CHECK(checked == 40);
}

}  // namespace

TEST_CASE("terrain collision: a bounded area's heightfield is the drawn ground") {
    const midgard::Terrain t(body(R"({"id": "a", "shape": "bounded_area", "width": 3000,
                                      "length": 2000, "seed": 7})"),
                             {});
    check_matches(t, t.ground({120.0, 0.0, -340.0}));
}

TEST_CASE("terrain collision: a flat-wrap cylinder's, at its join and a circumference over") {
    const midgard::Terrain t(body(R"({"id": "s", "shape": "flat_cylinder", "circumference": 6000,
                                      "length": 5000, "seed": 8})"),
                             {});
    check_matches(t, t.ground({2995.0, 0.0, 100.0}));  // the east edge: chunks from both ends
    check_matches(t, t.ground({6000.0 + 40.0, 0.0, 100.0}));  // walked once round
}

TEST_CASE("terrain collision: a curved cylinder's mesh is the drawn ground") {
    const midgard::Terrain t(body(R"({"id": "h", "shape": "curved_cylinder", "radius": 1000,
                                      "length": 5000, "seed": 9, "origin": [0, 1000, 0]})"),
                             {});
    check_matches(t, t.ground({0.0, 0.0, 0.0}));        // the bottom
    check_matches(t, t.ground({1000.0, 1000.0, 0.0}));  // a quarter round: up is -x
    check_matches(t, t.ground({-5.0, 0.2, 300.0}));     // across u's join (u = 0 and 1)
}

TEST_CASE("terrain collision: a sphere's meshes, across face edges and at a cube corner") {
    for (const char* text : {R"({"id": "w", "shape": "sphere_outside", "radius": 3000, "seed": 11,
              "origin": [0, -3000, 0]})",
                             R"({"id": "h", "shape": "sphere_inside", "radius": 2500, "seed": 12,
              "origin": [0, 2500, 0]})"}) {
        CAPTURE(text);
        const midgard::Terrain t(body(text), {});
        const glm::dvec3 c = t.body().origin;
        const double r = t.body().shape.radius();
        const auto on = [&](const glm::dvec3& d) { return t.ground(c + glm::normalize(d) * r); };
        check_matches(t, on({0.0, 1.0, 0.0}));  // a face's middle
        // (Near an edge and a corner, not exactly on them: a ray straight down an edge shared by
        // two bodies can slip past both in Jolt's own rounding. The spiral of points round each
        // place crosses the edges.)
        check_matches(t, on({1.0, 0.9995, 0.004}));   // by the edge between +X and +Y
        check_matches(t, on({1.0, 0.99, -0.995}));    // by the corner of +X, +Y and -Z
        check_matches(t, on({-1.0, -0.997, 1.002}));  // by another corner
        // By a corner, the ground round it comes from all three faces.
        midgard::physics::World world(t.body());
        midgard::TerrainCollision ground(t);
        const glm::dvec3 corner = on({1.0, 1.0, 1.0});
        ground.update(world, {corner + t.body().up(corner) * 1.5});
        for (const glm::dvec3 d : {glm::dvec3(1.0, 0.98, 0.98), glm::dvec3(0.98, 1.0, 0.98),
                                   glm::dvec3(0.98, 0.98, 1.0)}) {
            const glm::dvec3 above = on(d) + t.body().up(on(d)) * 20.0;
            CHECK(world.raycast(above, glm::vec3(-t.body().up(above)), 60.0).has_value());
        }
    }
}

TEST_CASE("terrain collision follows the player, and old ground goes") {
    const midgard::Terrain t(body(R"({"id": "a", "shape": "bounded_area", "width": 6000,
                                      "length": 6000, "seed": 7})"),
                             {});
    midgard::physics::World world(t.body());
    midgard::TerrainCollision ground(t);
    size_t most = 0;
    for (int i = 0; i <= 50; ++i) {
        ground.update(world, {glm::dvec3(-2500.0 + i * 100.0, 10.0, 0.0)});
        most = std::max(most, ground.bodies());
        CHECK(world.body_count() == ground.bodies());
    }
    CHECK(most <= 9 * 9);  // what's within twice the radius, at most
    // The ground under the last place is there.
    CHECK(world.raycast(t.ground({2500.0, 0.0, 0.0}) + glm::dvec3(0, 20, 0), {0, -1, 0}, 50.0)
              .has_value());
    CHECK_FALSE(
        world.raycast(t.ground({-2500.0, 0.0, 0.0}) + glm::dvec3(0, 20, 0), {0, -1, 0},
                      50.0)
            .has_value());  // long gone
    ground.clear(world);
    CHECK(world.body_count() == 0);
}
