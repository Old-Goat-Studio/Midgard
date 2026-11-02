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

namespace {

// A path in one piece: its points a few metres apart, on the ground, from start to end.
void check_joined(const midgard::Terrain& t, const midgard::NavMesh::Path& path,
                  const glm::dvec3& from, const glm::dvec3& to, double longest) {
    CHECK(path.complete);
    check_on_ground(t, path, to);
    CHECK(glm::distance(glm::dvec3(path.points.front()), from) < 1.0);
    double length = 0.0;
    for (size_t i = 1; i < path.points.size(); ++i) {
        const double step = glm::distance(path.points[i - 1], path.points[i]);
        CHECK(step < 4.5);
        length += step;
    }
    CHECK(length < longest);
}

}  // namespace

TEST_CASE("paths cross a cylinder's join") {
    midgard::JobSystem jobs;
    SUBCASE("inside a cylinder") {
        const midgard::Terrain t = make_terrain(
            R"({"id": "h", "shape": "curved_cylinder", "radius": 1000, "length": 5000,
                "seed": 8, "origin": [0, 1000, 0]})",
            R"({"seed": 8, "height_scale": 6, "base": 6})");
        midgard::NavWorld nav(t, &jobs);
        // 12.6 m one side of the join to 31.4 m the other: about 44 m, not 6 km round.
        const glm::dvec3 from = ground(t, 0, 0.002, 0.5), to = ground(t, 0, 0.995, 0.5);
        nav.update(from, true);
        const auto path = nav.find_path(from, to);
        REQUIRE(path);
        check_joined(t, *path, from, to, 60.0);
        // And the other way.
        nav.update(to, true);
        const auto back = nav.find_path(to, from);
        REQUIRE(back);
        check_joined(t, *back, to, from, 60.0);
    }
    SUBCASE("a flat-wrap cylinder, from a place in the room past its edge") {
        const midgard::Terrain t = make_terrain(
            R"({"id": "s", "shape": "flat_cylinder", "circumference": 6000, "length": 5000,
                "seed": 9})",
            R"({"seed": 9, "height_scale": 6, "base": 6})");
        midgard::NavWorld nav(t, &jobs);
        // The room repeats: x = 3005 is u = 0.0008 (a player who walked over the edge).
        const double h = t.height(0, 5.0 / 6000.0, 0.5);
        const glm::dvec3 from(3005.0, h, 0.0);
        const glm::dvec3 to(2970.0, t.height(0, 1.0 - 30.0 / 6000.0, 0.5), 0.0);
        nav.update(from, true);
        const auto path = nav.find_path(from, to);
        REQUIRE(path);
        check_joined(t, *path, from, to, 50.0);  // beside the start, in one piece
    }
}

TEST_CASE("paths cross a planet's face edges and corners") {
    midgard::JobSystem jobs;
    const auto check_sphere = [&](const char* body) {
        const midgard::Terrain t =
            make_terrain(body, R"({"seed": 10, "height_scale": 6, "base": 6})");
        midgard::NavWorld nav(t, &jobs);
        const auto across = [&](double u, double v) {  // a face's frame continued past its edge
            const midgard::SurfacePoint s = midgard::cube_surface(midgard::cube_direction(2, u, v));
            return ground(t, s.face, s.u, s.v);
        };
        // Over an edge: 31 m from face 2's edge to 47 m onto the next face.
        const glm::dvec3 from = ground(t, 2, 0.99, 0.5), to = across(1.015, 0.5);
        CHECK(t.body().surface(to).face != 2);
        nav.update(from, true);
        const auto path = nav.find_path(from, to);
        REQUIRE(path);
        check_joined(t, *path, from, to, 95.0);
        // Through a corner, where three faces meet.
        const glm::dvec3 corner_from = ground(t, 2, 0.99, 0.99), corner_to = across(1.012, 1.012);
        nav.update(corner_from, true);
        const auto corner = nav.find_path(corner_from, corner_to);
        REQUIRE(corner);
        check_joined(t, *corner, corner_from, corner_to, 120.0);
    };
    SUBCASE("a planet") {
        check_sphere(R"({"id": "p", "shape": "sphere_outside", "radius": 2000, "seed": 10})");
    }
    SUBCASE("a hollow world") {
        check_sphere(R"({"id": "w", "shape": "sphere_inside", "radius": 2000, "seed": 10})");
    }
}

namespace {

// A path all the way round a closed loop of ground points, leg by leg as an agent walking it
// would ask (the navigation made round each leg's start): every leg complete, and ending
// where it should.
void round_and_round(const midgard::Terrain& t, const std::vector<glm::dvec3>& loop,
                     midgard::JobSystem& jobs) {
    midgard::NavWorld nav(t, &jobs);
    nav.set_range(70.0);
    int legs = 0;
    double length = 0.0;
    for (size_t i = 0; i < loop.size(); ++i) {
        const glm::dvec3 a = loop[i], b = loop[(i + 1) % loop.size()];
        nav.update(a, true);
        const auto path = nav.find_path(a, b);
        REQUIRE(path);
        CHECK(path->complete);
        // (A flat-wrap cylinder's room repeats: the end may be a whole way round over.)
        glm::dvec3 end(path->points.back());
        if (t.body().shape.kind() == midgard::Shape::Kind::FlatCylinder) {
            const double round = t.body().shape.face_size().x;
            end.x -= round * std::round((end.x - b.x) / round);
        }
        CHECK(glm::distance(end, b) < 1.0);
        for (size_t k = 1; k < path->points.size(); ++k)
            length += glm::distance(path->points[k - 1], path->points[k]);
        ++legs;
    }
    MESSAGE(legs << " legs, " << length << " m");
}

// Ground points every `step` metres round a circle of directions (cos t a + sin t b) from a
// sphere's centre.
std::vector<glm::dvec3> sphere_loop(const midgard::Terrain& t, const glm::dvec3& a,
                                    const glm::dvec3& b, double step) {
    const double radius = t.body().shape.radius();
    const int n = int(std::ceil(2.0 * 3.141592653589793 * radius / step));
    std::vector<glm::dvec3> out;
    for (int i = 0; i < n; ++i) {
        const double angle = 2.0 * 3.141592653589793 * i / n;
        midgard::SurfacePoint s = midgard::cube_surface(std::cos(angle) * a + std::sin(angle) * b);
        s.height = t.height(s.face, s.u, s.v);
        out.push_back(t.body().position(s));
    }
    return out;
}

}  // namespace

TEST_CASE("the exit: a path all the way round each shape") {
    midgard::JobSystem jobs;
    const char* hills = R"({"seed": 14, "height_scale": 4, "base": 6})";
    SUBCASE("a planet: round its equator, and over both poles") {
        const midgard::Terrain t = make_terrain(
            R"({"id": "p", "shape": "sphere_outside", "radius": 1000, "seed": 14})", hills);
        round_and_round(t, sphere_loop(t, {1, 0, 0}, {0, 0, 1}, 60.0), jobs);
        round_and_round(t, sphere_loop(t, {1, 0, 0}, {0, 1, 0}, 60.0), jobs);
    }
    SUBCASE("a hollow world") {
        const midgard::Terrain t = make_terrain(
            R"({"id": "w", "shape": "sphere_inside", "radius": 1000, "seed": 14})", hills);
        round_and_round(t, sphere_loop(t, {1, 0, 0}, {0, 0, 1}, 60.0), jobs);
    }
    SUBCASE("inside a cylinder, and a flat-wrap cylinder") {
        for (const char* body :
             {R"({"id": "h", "shape": "curved_cylinder", "radius": 1000, "length": 5000,
                  "seed": 14})",
              R"({"id": "s", "shape": "flat_cylinder", "circumference": 6000, "length": 5000,
                  "seed": 14})"}) {
            const midgard::Terrain t = make_terrain(body, hills);
            const double round = t.body().shape.face_size().x;
            std::vector<glm::dvec3> loop;
            const int n = int(std::ceil(round / 60.0));
            for (int i = 0; i < n; ++i) loop.push_back(ground(t, 0, double(i) / n, 0.5));
            round_and_round(t, loop, jobs);
        }
    }
    SUBCASE("a bounded area: a loop round its middle") {
        const midgard::Terrain t = make_terrain(
            R"({"id": "a", "shape": "bounded_area", "width": 2000, "length": 2000, "seed": 14})",
            hills);
        std::vector<glm::dvec3> loop;
        const auto side = [&](double u0, double v0, double u1, double v1) {
            for (int i = 0; i < 10; ++i)
                loop.push_back(ground(t, 0, u0 + (u1 - u0) * i / 10.0, v0 + (v1 - v0) * i / 10.0));
        };
        side(0.3, 0.3, 0.7, 0.3);  // 800 m sides, 80 m legs
        side(0.7, 0.3, 0.7, 0.7);
        side(0.7, 0.7, 0.3, 0.7);
        side(0.3, 0.7, 0.3, 0.3);
        round_and_round(t, loop, jobs);
    }
}
