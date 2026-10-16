// SPDX-License-Identifier: Apache-2.0
// World shapes: the shared table (schema/tests/shape_points.json, which the Python reference
// matches), then the properties every later system relies on: exact seams, round trips, walking
// all the way round with no jumps, and gravity.
#include <doctest/doctest.h>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "midgard/body.h"
#include "midgard/det.h"
#include "midgard/shape.h"

using midgard::Shape;
using midgard::SurfacePoint;

namespace {

nlohmann::json table(const std::string& name) {
    std::ifstream in(std::filesystem::path(MIDGARD_SCHEMA_DIR) / "tests" / (name + ".json"));
    REQUIRE(in);
    return nlohmann::json::parse(in).at("cases");
}

std::string bits(double x) {
    std::uint64_t b = 0;
    std::memcpy(&b, &x, sizeof(b));
    char text[17];
    std::snprintf(text, sizeof(text), "%016" PRIx64, b);
    return text;
}

std::vector<std::string> bits(const glm::dvec3& v) {
    return {bits(v.x), bits(v.y), bits(v.z)};
}

double hexf(const nlohmann::json& v) {
    return std::strtod(v.get<std::string>().c_str(), nullptr);
}

// The shapes the table uses, by name.
Shape named(const std::string& name) {
    if (name == "bounded_area") return Shape::bounded_area(2000.0, 3000.0, 9.81);
    if (name == "flat_cylinder") return Shape::flat_cylinder(6283.0, 5000.0, 9.81);
    if (name == "curved_cylinder") return Shape::curved_cylinder(1000.0, 5000.0, 9.81);
    if (name == "sphere_outside") return Shape::sphere_outside(1000.0, 9.81);
    return Shape::sphere_inside(1500.0, 7.5);
}

std::vector<Shape> all_shapes() {
    return {named("bounded_area"), named("flat_cylinder"), named("curved_cylinder"),
            named("sphere_outside"), named("sphere_inside")};
}

double angle_between(const glm::dvec3& a, const glm::dvec3& b) {
    return std::acos(std::clamp(glm::dot(a, b), -1.0, 1.0));
}

}  // namespace

TEST_CASE("shapes match their table bit for bit") {
    for (const auto& c : table("shape_points")) {
        const Shape shape = named(c["shape"]);
        const auto& s = c["surface"];
        const SurfacePoint point{s[0].get<int>(), hexf(s[1]), hexf(s[2]), hexf(s[3])};
        const glm::dvec3 p = shape.position(point);
        CHECK_MESSAGE(bits(p) == c["position"].get<std::vector<std::string>>(), c.dump());
        const SurfacePoint back = shape.surface(p);
        CHECK_MESSAGE(back.face == c["back"][0].get<int>(), c.dump());
        CHECK_MESSAGE(bits(back.u) == c["back"][1].get<std::string>(), c.dump());
        CHECK_MESSAGE(bits(back.v) == c["back"][2].get<std::string>(), c.dump());
        CHECK_MESSAGE(bits(back.height) == c["back"][3].get<std::string>(), c.dump());
        CHECK_MESSAGE(bits(shape.up(p)) == c["up"].get<std::vector<std::string>>(), c.dump());
        CHECK_MESSAGE(bits(shape.gravity(p)) == c["gravity"].get<std::vector<std::string>>(),
                      c.dump());
    }
}

TEST_CASE("cube-sphere seams are exact on power-of-two grids") {
    // Every point on a face's edge must come out bit-identical from every face that has it:
    // otherwise terrain cracks, and chunk hashes differ, along the seams.
    for (const Shape& sphere : {named("sphere_outside"), named("sphere_inside")}) {
        constexpr int N = 64;
        std::map<std::vector<std::string>, int> shared;
        for (int f = 0; f < 6; ++f)
            for (int i = 0; i <= N; ++i)
                for (int j = 0; j <= N; ++j)
                    if (i == 0 || i == N || j == 0 || j == N)
                        ++shared[bits(sphere.position({f, double(i) / N, double(j) / N, 12.5}))];
        int corners = 0, edges = 0;
        for (const auto& [point, faces] : shared) {
            REQUIRE(faces >= 2);  // no edge point without its twin on the neighbouring face
            corners += faces == 3;
            edges += faces == 2;
        }
        CHECK(corners == 8);
        CHECK(edges == 12 * (N - 1));
    }
}

TEST_CASE("the cylinder's join is exact") {
    for (const Shape& c : {named("curved_cylinder"), named("flat_cylinder")})
        for (int j = 0; j <= 16; ++j) {
            const double v = j / 16.0;
            CHECK(bits(c.position({0, 0.0, v, 2.5})) == bits(c.position({0, 1.0, v, 2.5})));
            CHECK(bits(c.position({0, -0.25, v, 0.0})) == bits(c.position({0, 0.75, v, 0.0})));
        }
}

TEST_CASE("surface to position and back, for millions of points") {
    midgard::det::Rng rng(11);
    for (const Shape& shape : all_shapes()) {
        double worst = 0.0;
        for (int i = 0; i < 400'000; ++i) {
            const SurfacePoint s{static_cast<int>(rng.below(shape.faces())), rng.uniform(),
                                 rng.uniform(), rng.uniform() * 200.0 - 100.0};
            const glm::dvec3 p = shape.position(s);
            const SurfacePoint back = shape.surface(p);
            worst = std::max(worst, glm::length(shape.position(back) - p));
            // Back where it was on the surface too (u wraps on cylinders; seams may change face).
            REQUIRE(std::fabs(back.height - s.height) < 1e-6);
        }
        CHECK(worst < 1e-8);  // metres
    }
}

TEST_CASE(
    "walking all the way round a sphere: over the poles, along the equator, through corners") {
    for (const Shape& sphere : {named("sphere_outside"), named("sphere_inside")}) {
        const double R = sphere.radius();
        // Great circles: the equator, one over the poles, and one through four cube corners.
        const glm::dvec3 planes[3][2] = {
            {{1, 0, 0}, {0, 0, 1}},
            {{1, 0, 0}, {0, 1, 0}},
            {glm::normalize(glm::dvec3(1, 1, 1)), glm::normalize(glm::dvec3(1, -1, 0))},
        };
        for (const auto& plane : planes) {
            const glm::dvec3 a = plane[0],
                             b = glm::normalize(plane[1] - plane[0] * glm::dot(plane[0], plane[1]));
            constexpr int steps = 20000;  // about 0.3 m apart on a 1 km sphere
            glm::dvec3 previous_p{}, previous_up{};
            int faces_seen = 0, last_face = -1;
            for (int k = 0; k <= steps; ++k) {
                const double t = 2.0 * 3.141592653589793 * k / steps;
                const glm::dvec3 target = (a * std::cos(t) + b * std::sin(t)) * R;
                SurfacePoint s = sphere.surface(target);
                s.height = 0.0;
                const glm::dvec3 p = sphere.position(s);
                REQUIRE(glm::length(p - target) < 1e-8);
                const glm::dvec3 up = sphere.up(p);
                REQUIRE(std::fabs(glm::length(sphere.gravity(p)) - sphere.surface_gravity()) <
                        1e-9);
                if (k > 0) {
                    REQUIRE(glm::length(p - previous_p) < 2 * 3.1416 * R / steps * 1.001);
                    REQUIRE(angle_between(up, previous_up) < 1e-3);  // up never flips
                }
                if (s.face != last_face) ++faces_seen, last_face = s.face;
                previous_p = p;
                previous_up = up;
            }
            CHECK(glm::length(previous_p - a * R) < 1e-8);  // back where it started
            CHECK(faces_seen >= 4);
        }
    }
}

TEST_CASE("walking round the inside of a cylinder, with spin gravity") {
    const Shape tube = named("curved_cylinder");
    const double R = tube.radius();
    glm::dvec3 p = tube.position({0, 0.0, 0.5, 0.0});
    const glm::dvec3 start = p;
    glm::dvec3 previous_up = tube.up(p);
    const double step = 0.5;  // metres
    const int steps = static_cast<int>(std::round(2 * 3.141592653589793 * R / step));
    for (int k = 0; k < steps; ++k) {
        // A step along the surface's own east, then back onto the surface.
        const midgard::SurfaceFrame f = tube.frame(p);
        SurfacePoint s = tube.surface(p + f.east * step);
        s.height = 0.0;
        const glm::dvec3 next = tube.position(s);
        REQUIRE(std::fabs(glm::length(next - p) - step) < 1e-3);
        REQUIRE(angle_between(tube.up(next), previous_up) < 1e-3);
        REQUIRE(std::fabs(glm::length(tube.gravity(next)) - 9.81) < 1e-9);
        previous_up = tube.up(next);
        p = next;
    }
    CHECK(glm::length(p - start) < step);  // round, give or take the last part-step
    // Spin gravity fades towards the axis: half the radius, half the gravity.
    const glm::dvec3 halfway = tube.position({0, 0.3, 0.5, R / 2});
    CHECK(glm::length(tube.gravity(halfway)) == doctest::Approx(9.81 / 2));
    CHECK(glm::dot(tube.gravity(halfway), tube.up(halfway)) < 0.0);  // away from the axis
}

TEST_CASE("the flat-wrap cylinder repeats every circumference; the bounded area doesn't wrap") {
    const Shape flat = named("flat_cylinder");
    const glm::dvec3 p = flat.position({0, 0.2, 0.4, 1.0});
    const SurfacePoint again = flat.surface(p + glm::dvec3(6283.0, 0.0, 0.0));
    CHECK(again.u == doctest::Approx(0.2));
    const Shape area = named("bounded_area");
    CHECK(area.surface(area.position({0, 0.9, 0.4, 0.0}) + glm::dvec3(400.0, 0, 0)).u ==
          doctest::Approx(1.1));
    CHECK(area.gravity(p) == glm::dvec3(0.0, -9.81, 0.0));
}

TEST_CASE("up, gravity and the frame point the right ways") {
    for (const Shape& shape : all_shapes()) {
        midgard::det::Rng rng(3);
        for (int i = 0; i < 2000; ++i) {
            const glm::dvec3 p = shape.position(
                {static_cast<int>(rng.below(shape.faces())), rng.uniform(), rng.uniform(), 1.0});
            const midgard::SurfaceFrame f = shape.frame(p);
            REQUIRE(std::fabs(glm::length(f.up) - 1.0) < 1e-12);
            REQUIRE(std::fabs(glm::dot(f.up, f.east)) < 1e-9);
            REQUIRE(glm::length(glm::cross(f.east, f.north) - f.up) < 1e-9);
            REQUIRE(glm::dot(shape.gravity(p), f.up) < 0.0);  // gravity pulls down
            // Height is along up: a point 1 m higher is 1 m further along up.
            SurfacePoint s = shape.surface(p);
            s.height += 1.0;
            REQUIRE(glm::length(shape.position(s) - (p + f.up)) < 1e-6);
        }
    }
}

TEST_CASE("the shape fingerprint matches its table") {
    const std::string want = table("shape_fingerprint")[0]["fingerprint"].get<std::string>();
    char got[17], constant[17];
    std::snprintf(got, sizeof(got), "%016" PRIx64, midgard::shape_fingerprint());
    std::snprintf(constant, sizeof(constant), "%016" PRIx64, midgard::kShapeFingerprint);
    CHECK(std::string(got) == want);
    CHECK(std::string(constant) == want);
}

TEST_CASE("bodies come from settings, within the roadmap's size limits") {
    const midgard::Body planet = midgard::body_from_json(nlohmann::json::parse(R"({
        "id": "test_planet", "shape": "sphere_outside", "radius": 1000, "gravity": 9.81,
        "seed": 829174332, "origin": [100, 0, -50]})"));
    CHECK(planet.id == "test_planet");
    CHECK(planet.seed == 829174332u);
    CHECK(planet.shape.kind() == Shape::Kind::SphereOutside);
    CHECK(std::string(midgard::shape_name(planet.shape.kind())) == "sphere_outside");
    // Placed in its room: positions move with the origin, and the rest follows.
    const glm::dvec3 top = planet.position({2, 0.5, 0.5, 0.0});  // the +Y face's middle
    CHECK(glm::length(top - glm::dvec3(100, 1000, -50)) < 1e-9);
    CHECK(glm::length(planet.up(top) - glm::dvec3(0, 1, 0)) < 1e-12);
    CHECK(planet.surface(top).face == 2);

    const auto fails = [](const char* json, const char* message) {
        CHECK_THROWS_WITH_AS(midgard::body_from_json(nlohmann::json::parse(json)),
                             doctest::Contains(message), midgard::BodyError);
    };
    fails(R"({"shape": "sphere_outside", "radius": 500})", "radius 500");
    fails(R"({"shape": "curved_cylinder", "radius": 2000, "length": 60000})", "length 60000");
    fails(R"({"shape": "flat_cylinder", "circumference": 9000})", "\"length\"");
    fails(R"({"shape": "torus", "radius": 2000})", "unknown shape \"torus\"");
    fails(R"({"shape": "sphere_inside", "radius": 2000, "gravity": 0})", "gravity must");
    fails(R"({"shape": "sphere_inside", "radius": 2000, "origin": [1, 2]})", "origin needs 3");
    fails(R"({"shape": "sphere_inside", "radius": 2000, "seed": -1.5})", "whole number");
}
