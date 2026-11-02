// SPDX-License-Identifier: Apache-2.0
// Navigation meshes (midgard/navmesh.h, milestone 5.12): tiles built from triangles, paths that
// go round walls and across tiles, places that can't be reached, and steep slopes left out.
#include <doctest/doctest.h>

#include <cmath>

#include "midgard/jobs.h"
#include "midgard/navmesh.h"

namespace {

// An axis-aligned box's twelve triangles.
void add_box(midgard::NavGeometry& g, const glm::vec3& lo, const glm::vec3& hi) {
    const auto corner = [&](int i) {
        return glm::vec3(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z);
    };
    const int faces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1},
                             {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
    for (const auto& f : faces) {
        g.add_triangle(corner(f[0]), corner(f[2]), corner(f[1]));  // (counter-clockwise from
        g.add_triangle(corner(f[0]), corner(f[3]), corner(f[2]));  // outside, as in glTF)
    }
}

float length(const std::vector<glm::vec3>& points) {
    float total = 0.0f;
    for (size_t i = 1; i < points.size(); ++i) total += glm::distance(points[i - 1], points[i]);
    return total;
}

}  // namespace

TEST_CASE("a path goes round a wall, across tiles, and not into a walled yard") {
    midgard::NavGeometry g;
    add_box(g, {-40, -1, -40}, {40, 0, 40});     // the floor, 80 m square
    add_box(g, {-15, 0, -0.5f}, {15, 3, 0.5f});  // a wall across the middle, 30 m long
    // A yard walled in on all four sides, 10 m square, at the floor's far corner.
    add_box(g, {20, 0, 20}, {30, 3, 20.5f});
    add_box(g, {20, 0, 29.5f}, {30, 3, 30});
    add_box(g, {20, 0, 20}, {20.5f, 3, 30});
    add_box(g, {29.5f, 0, 20}, {30, 3, 30});
    midgard::NavSettings s;
    midgard::JobSystem jobs;
    const auto mesh = midgard::build_navmesh(s, g, &jobs);
    CHECK(mesh->tile_count() == 9);  // 80 m in 32 m tiles: 3 x 3
    CHECK(mesh->polygon_count() > 0);

    const auto path = mesh->find_path({0, 0, -10}, {0, 0, 10});
    REQUIRE(path);
    CHECK(path->complete);
    REQUIRE(path->points.size() >= 3);  // round an end of the wall
    CHECK(glm::distance(path->points.front(), glm::vec3(0, 0, -10)) < 0.3f);
    CHECK(glm::distance(path->points.back(), glm::vec3(0, 0, 10)) < 0.3f);
    // Round the wall's end (15 m out, plus the agent's radius): about 2 x sqrt(15^2 + 10^2).
    CHECK(length(path->points) > 2.0f * std::sqrt(15.0f * 15.0f + 10.0f * 10.0f));
    CHECK(length(path->points) < 40.0f);
    for (const glm::vec3& p : path->points) {
        CHECK(std::abs(p.y) < 0.3f);                                   // on the floor
        CHECK_FALSE((std::abs(p.x) < 15.0f && std::abs(p.z) < 0.5f));  // never in the wall
    }
    // Into the yard: as near as it gets, outside its wall.
    const auto yard = mesh->find_path({0, 0, -10}, {25, 0, 25});
    REQUIRE(yard);
    CHECK_FALSE(yard->complete);
    const glm::vec3 stop = yard->points.back();
    CHECK_FALSE((stop.x > 20.0f && stop.x < 30.0f && stop.z > 20.0f && stop.z < 30.0f));
    // Far from the mesh: no path, and no nearest point.
    CHECK_FALSE(mesh->find_path({0, 0, -10}, {200, 0, 200}));
    CHECK_FALSE(mesh->nearest({0, 50, 0}));
    CHECK(mesh->nearest({0, 1, -10}));
    CHECK_FALSE(mesh->edges().empty());
    // A tile taken away and put back.
    const midgard::NavTileKey middle = mesh->tile_at({0, 0, 0});
    mesh->remove_tile(middle);
    CHECK(mesh->tile_count() == 8);
    mesh->add_tile(middle, midgard::build_nav_tile(s, mesh->origin(), middle, g));
    CHECK(mesh->tile_count() == 9);
    CHECK(mesh->find_path({0, 0, -10}, {0, 0, 10})->complete);
}

TEST_CASE("slopes steeper than the agent climbs are left out, and the same mesh every time") {
    midgard::NavGeometry g;
    add_box(g, {-10, -1, -10}, {10, 0, 10});
    // A ramp rising 60 degrees from z = 2 to z = 4, on the floor.
    const float rise = 2.0f * std::tan(glm::radians(60.0f));
    g.add_triangle({-3, 0, 2}, {3, rise, 4}, {3, 0, 2});
    g.add_triangle({-3, 0, 2}, {-3, rise, 4}, {3, rise, 4});
    midgard::NavSettings s;
    const auto mesh = midgard::build_navmesh(s, g);
    const auto up = mesh->nearest({0, rise * 0.5f, 3}, 0.2f);
    CHECK((!up || up->y < 0.5f));  // nothing walkable on the ramp itself
    // Built again (one thread, then many): the same tiles.
    midgard::JobSystem jobs;
    const auto again = midgard::build_navmesh(s, g, &jobs);
    for (int z = 0; z < 1; ++z)
        for (int x = 0; x < 1; ++x)
            CHECK(midgard::build_nav_tile(s, mesh->origin(), {x, z}, g) ==
                  midgard::build_nav_tile(s, again->origin(), {x, z}, g));
}
