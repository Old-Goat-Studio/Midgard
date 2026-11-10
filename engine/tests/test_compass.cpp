// SPDX-License-Identifier: Apache-2.0
// The compass and the minimap's ground (milestone 6.0, midgard/compass.h): north holds on every
// world shape, round a planet too.
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>

#include <nlohmann/json.hpp>

#include "midgard/compass.h"
#include "midgard/world_map.h"

using json = nlohmann::json;
using midgard::bearing_at;
using midgard::heading_of;

namespace {

constexpr double kPi = 3.14159265358979323846;

midgard::Body body(const char* text) {
    return midgard::body_from_json(json::parse(text));
}

// How far apart two headings are, the short way round.
double apart(double a, double b) {
    const double d = std::fmod(std::abs(a - b), 360.0);
    return std::min(d, 360.0 - d);
}

}  // namespace

TEST_CASE("on flat ground north is -Z and east +X") {
    const midgard::Bearing b = bearing_at(std::nullopt, glm::dvec3(5.0, 1.0, 3.0));
    CHECK(heading_of(b, {0, 0, -1}) == doctest::Approx(0.0));
    CHECK(heading_of(b, {1, 0, 0}) == doctest::Approx(90.0));
    CHECK(heading_of(b, {0, 0, 1}) == doctest::Approx(180.0));
    CHECK(heading_of(b, {-1, 0, 0}) == doctest::Approx(270.0));
    CHECK(heading_of(b, {1, 0, -1}) == doctest::Approx(45.0));
    CHECK(heading_of(b, {0, 1, 0}, 123.0) == 123.0);  // (straight up: no heading)
    const glm::dvec2 en = midgard::east_north(b, {0, 0, 0}, {10, 3, -5});
    CHECK(en.x == doctest::Approx(10.0));
    CHECK(en.y == doctest::Approx(5.0));
    // A bounded area's own north is the same.
    const midgard::Bearing area = bearing_at(
        body(R"({"id": "a", "shape": "bounded_area", "width": 400, "length": 400})"), {10, 0, 10});
    CHECK(glm::length(area.north - glm::dvec3(0, 0, -1)) < 1e-9);
}

TEST_CASE("walking round a planet, the compass stays true across the cube's face edges") {
    for (const char* shape : {"sphere_outside", "sphere_inside"}) {
        CAPTURE(shape);
        const auto planet = body(
            (std::string(R"({"id": "p", "shape": ")") + shape + R"(", "radius": 3000})").c_str());
        // Northwards along a meridian, from near one pole to near the other (through four of the
        // cube's faces): the heading is north all the way.
        for (int i = -85; i <= 85; i += 5) {
            const double lat = i * kPi / 180.0;
            const glm::dvec3 p = 3000.0 * glm::dvec3(0.0, std::sin(lat), std::cos(lat));
            const glm::dvec3 northwards(0.0, std::cos(lat), -std::sin(lat));
            CHECK(heading_of(bearing_at(planet, p), northwards) ==
                  doctest::Approx(0.0).epsilon(1e-6));
        }
        // Eastwards round the equator (through four faces): east all the way. Inside a hollow
        // world east is on the right facing north just the same, so the way round is mirrored.
        const bool inside = std::string(shape) == "sphere_inside";
        for (int i = 0; i < 360; i += 10) {
            const double lon = i * kPi / 180.0;
            const glm::dvec3 p = 3000.0 * glm::dvec3(std::sin(lon), 0.0, std::cos(lon));
            const glm::dvec3 round(std::cos(lon), 0.0, -std::sin(lon));
            const double h = heading_of(bearing_at(planet, p), inside ? -round : round);
            CHECK(apart(h, 90.0) < 1e-6);
        }
        // At the pole itself it still has a frame (no NaN).
        const midgard::Bearing pole = bearing_at(planet, {0.0, 3000.0, 0.0});
        CHECK(std::isfinite(pole.north.x));
        CHECK(glm::length(pole.east) == doctest::Approx(1.0));
    }
}

TEST_CASE("on a cylinder north is along it, and east goes round") {
    const auto tube =
        body(R"({"id": "c", "shape": "curved_cylinder", "radius": 1500, "length": 8000})");
    for (int i = 0; i < 360; i += 30) {
        const double a = i * kPi / 180.0;
        const glm::dvec3 p(1500.0 * std::sin(a), -1500.0 * std::cos(a), 100.0);
        const midgard::Bearing b = bearing_at(tube, p);
        CHECK(std::abs(b.north.z) == doctest::Approx(1.0));  // along the tube
        CHECK(glm::dot(b.east, b.up) == doctest::Approx(0.0).epsilon(1e-9));
        CHECK(glm::dot(b.north, b.up) == doctest::Approx(0.0).epsilon(1e-9));
        // Facing along the frame's east is 90 degrees wherever you are round it.
        CHECK(heading_of(b, tube.frame(p).east) == doctest::Approx(90.0).epsilon(1e-6));
    }
}

TEST_CASE("the minimap's ground is a north-up piece of the world map") {
    midgard::TerrainSettings s;
    s.seed = 7;
    const midgard::Terrain area(
        body(R"({"id": "a", "shape": "bounded_area", "width": 4000, "length": 4000})"), s);
    const auto map = midgard::local_map(area, {0.0, 0.0, 0.0}, 200.0, 32);
    REQUIRE(map.size() == 32 * 32 * 4);
    CHECK(map[3] == 255);  // (all on the ground)
    // Near the area's edge, what's past it is see-through.
    const auto edge = midgard::local_map(area, {1990.0, 0.0, 0.0}, 200.0, 32);
    CHECK(edge[(16 * 32 + 1) * 4 + 3] == 255);  // west of the middle: ground
    CHECK(edge[(16 * 32 + 30) * 4 + 3] == 0);   // east, past the edge
    // The same place on the world map has the same colour (its middle pixel).
    const auto world = midgard::world_map(area, 400, 400);
    const auto here = midgard::local_map(area, {5.0, 0.0, 5.0}, 5.0, 2);
    const size_t w = (200 * 400 + 200) * 4;
    for (int c = 0; c < 3; ++c) CHECK(std::abs(int(here[c]) - int(world[w + c])) < 40);
}

TEST_CASE("the minimap's ground is quick enough to make as the player walks") {
    midgard::TerrainSettings s;
    s.seed = 3;
    const midgard::Terrain planet(body(R"({"id": "p", "shape": "sphere_outside", "radius": 3000})"),
                                  s);
    const auto start = std::chrono::steady_clock::now();
    const auto map = midgard::local_map(planet, {0.0, 3000.0, 0.0}, 120.0, 128);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("a 128 x 128 minimap ground in " << ms << " ms");
    CHECK(map.size() == 128 * 128 * 4);
    CHECK(map[3] == 255);  // (on a planet there is ground everywhere)
}

TEST_CASE("the HUD's position: latitude and longitude on spheres, metres elsewhere") {
    using midgard::place_text;
    // Flat ground without a body: x east, -z north.
    CHECK(place_text(std::nullopt, {100.0, 0.0, -50.0}) == "100 m E, 50 m N");
    CHECK(place_text(std::nullopt, {-12345.0, 0.0, 20.0}) == "12.3 km W, 20 m S");
    // A planet: the +Y pole is 90 N; +Z is longitude 0 and +X 90 E (as the world map has them).
    const midgard::Body planet =
        body(R"({"id": "p", "shape": "sphere_outside", "radius": 1000, "gravity": 9.81})");
    CHECK(place_text(planet, {0.0, 1010.0, 0.0}).rfind("90.00\xC2\xB0 N", 0) == 0);
    CHECK(place_text(planet, {1000.0, 0.0, 0.0}) == "0.00\xC2\xB0 N, 90.00\xC2\xB0 E");
    CHECK(place_text(planet, {0.0, -707.1, -707.1}) == "45.00\xC2\xB0 S, 180.00\xC2\xB0 E");
    // Inside a hollow world the same.
    const midgard::Body hollow =
        body(R"({"id": "h", "shape": "sphere_inside", "radius": 1000, "gravity": 9.81})");
    CHECK(place_text(hollow, {-990.0, 0.0, 0.0}) == "0.00\xC2\xB0 N, 90.00\xC2\xB0 W");
    // A bounded area: metres from its middle, east and north as the compass has them.
    const midgard::Body area =
        body(R"({"id": "a", "shape": "bounded_area", "width": 2000, "length": 2000,
                  "gravity": 9.81})");
    const midgard::Bearing b = bearing_at(area, {0.0, 0.0, 0.0});
    const glm::dvec3 p = b.east * 300.0 - b.north * 120.0;
    CHECK(place_text(area, p) == "300 m E, 120 m S");
}
