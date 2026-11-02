// SPDX-License-Identifier: Apache-2.0
// Navigation over terrain (midgard/nav_world.h, milestone 5.12): tiles made round a point in
// each face's frame, on a bounded area, inside a cylinder and on a planet; paths that follow
// the ground; none under the sea.
#include <doctest/doctest.h>

#include <cmath>

#include <nlohmann/json.hpp>

#include "midgard/jobs.h"
#include "midgard/nav_world.h"

namespace {

midgard::Terrain make_terrain(const char* body_json, const char* settings_json) {
    return midgard::Terrain(midgard::body_from_json(nlohmann::json::parse(body_json)),
                            midgard::terrain_from_json(nlohmann::json::parse(settings_json)));
}

// The world point on the ground at (u, v) of a face.
glm::dvec3 ground(const midgard::Terrain& t, int face, double u, double v) {
    return t.body().position({face, u, v, t.height(face, u, v)});
}

// Every point of a path is on the ground (within `slack` metres), and it ends where asked.
void check_on_ground(const midgard::Terrain& t, const midgard::NavMesh::Path& path,
                     const glm::dvec3& to, double slack = 0.6) {
    REQUIRE(path.points.size() >= 2);
    for (const glm::vec3& p : path.points) CHECK(std::abs(t.above_ground(glm::dvec3(p))) < slack);
    CHECK(glm::distance(glm::dvec3(path.points.back()), to) < 1.0);
}

}  // namespace

TEST_CASE("navigation over a bounded area's hills, round the player") {
    const midgard::Terrain t = make_terrain(
        R"({"id": "a", "shape": "bounded_area", "width": 3000, "length": 3000, "seed": 3})",
        R"({"seed": 3, "height_scale": 12, "base": 10, "feature_size": 300})");
    midgard::JobSystem jobs;
    midgard::NavWorld nav(t, &jobs);
    nav.set_scenery(midgard::Scene{}, [](const std::string&) {
        return static_cast<const midgard::render::MeshData*>(nullptr);
    });
    const glm::dvec3 here = ground(t, 0, 0.5, 0.5);
    nav.update(here, true);
    CHECK(nav.stats().complete);
    CHECK(nav.stats().tiles >= 25);  // round the point, 96 m each way in 32 m tiles
    const glm::dvec3 to = ground(t, 0, 0.5 + 60.0 / 3000.0, 0.5 + 40.0 / 3000.0);
    const auto path = nav.find_path(here, to);
    REQUIRE(path);
    CHECK(path->complete);
    check_on_ground(t, *path, to);
    // Its points are a few metres apart (the path follows the ground between corners).
    for (size_t i = 1; i < path->points.size(); ++i)
        CHECK(glm::distance(path->points[i - 1], path->points[i]) < 4.5f);
    // Far away (inland, 600 m off): nothing made there, so no path.
    const glm::dvec3 away = ground(t, 0, 0.7, 0.7);
    CHECK_FALSE(nav.find_path(here, away));
    // Moving on: tiles round the new place are made, far ones go.
    nav.update(away, true);
    CHECK(nav.find_path(away, ground(t, 0, 0.7 + 0.01, 0.7)));
    CHECK_FALSE(nav.find_path(here, to));
}

TEST_CASE("no walking under the sea") {
    // The ocean round a bounded area: 30 m down at its edge.
    const midgard::Terrain t = make_terrain(
        R"({"id": "a", "shape": "bounded_area", "width": 2000, "length": 2000, "seed": 4})",
        R"({"seed": 4, "height_scale": 2, "base": 6, "ocean_band": 300, "ocean_depth": 30})");
    midgard::NavWorld nav(t, nullptr);
    const glm::dvec3 shore = ground(t, 0, 0.5, 200.0 / 2000.0);
    nav.update(shore, true);
    const glm::dvec3 deep = ground(t, 0, 0.5, 120.0 / 2000.0);
    REQUIRE(t.above_ground(deep) > -0.01);
    CHECK(deep.y < t.settings().sea_level - 5.0);
    const auto path = nav.find_path(shore, deep, 3.0f);
    CHECK((!path || !path->complete));
}

TEST_CASE("navigation inside a cylinder and on a planet, in each face's frame") {
    midgard::JobSystem jobs;
    SUBCASE("inside a cylinder: the unrolled surface") {
        const midgard::Terrain t = make_terrain(
            R"({"id": "h", "shape": "curved_cylinder", "radius": 1000, "length": 5000,
                "seed": 5, "origin": [0, 1000, 0]})",
            R"({"seed": 5, "height_scale": 8, "base": 6})");
        midgard::NavWorld nav(t, &jobs);
        const glm::dvec3 here = ground(t, 0, 0.25, 0.5);
        nav.update(here, true);
        CHECK(nav.stats().complete);
        // Round the cylinder a little (u) and along it (v).
        const double circumference = t.body().shape.face_size().x;
        const glm::dvec3 to = ground(t, 0, 0.25 + 50.0 / circumference, 0.5 + 30.0 / 5000.0);
        const auto path = nav.find_path(here, to);
        REQUIRE(path);
        CHECK(path->complete);
        check_on_ground(t, *path, to);
        // A place and back: the frame round-trips.
        const midgard::NavWorld::Place p = nav.place(here);
        CHECK(glm::distance(nav.world(p.face, p.at), here) < 0.01);
    }
    SUBCASE("on a planet: one of its six faces") {
        const midgard::Terrain t = make_terrain(
            R"({"id": "p", "shape": "sphere_outside", "radius": 2000, "seed": 6,
                "origin": [0, -2000, 0]})",
            R"({"seed": 6, "height_scale": 8, "base": 6})");
        midgard::NavWorld nav(t, &jobs);
        CHECK(nav.faces() == 6);
        const glm::dvec3 here = ground(t, 2, 0.5, 0.5);
        nav.update(here, true);
        CHECK(nav.stats().complete);
        const glm::dvec3 to = ground(t, 2, 0.52, 0.49);
        const auto path = nav.find_path(here, to);
        REQUIRE(path);
        CHECK(path->complete);
        check_on_ground(t, *path, to);
        CHECK(nav.mesh(2).tile_count() > 0);
        CHECK(nav.mesh(0).tile_count() == 0);  // (only round the point)
    }
}
