// SPDX-License-Identifier: Apache-2.0
// Light models per world shape (midgard/sunlight.h): against the shared table
// (schema/tests/light_models.json, from tools/reference/light.py), and the behaviour milestone 5.8
// asks for: a planet's noon moving with longitude, polar day and night in Latitude mode, and the
// stylised sun of inside worlds.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "midgard/sunlight.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::optional<midgard::Body> body_named(const json& bodies, const std::string& name) {
    const json& b = bodies.at(name);
    if (b.is_null()) return std::nullopt;
    json settings{{"id", name}, {"shape", b.at("shape")}, {"origin", b.at("origin")}};
    const std::string shape = b.at("shape").get<std::string>();
    if (shape == "bounded_area") {
        settings["width"] = 4000;
        settings["length"] = 4000;
    } else if (shape == "curved_cylinder") {
        settings["radius"] = 1000;
        settings["length"] = 5000;
    } else {
        settings["radius"] = 1000;
    }
    return midgard::body_from_json(settings);
}

midgard::SunClock clock_from(const json& c) {
    midgard::SunClock clock;
    clock.time = c.at("time").get<double>();
    clock.daylight = c.at("daylight").get<double>();
    clock.noon = c.at("noon").get<double>();
    clock.mode = *midgard::sun_mode_from_name(c.at("mode").get<std::string>());
    clock.tilt = c.at("tilt").get<double>();
    clock.year = c.at("year").get<double>();
    return clock;
}

glm::dvec3 vec(const json& v) {
    return {v[0].get<double>(), v[1].get<double>(), v[2].get<double>()};
}

// A planet like the table's, its surface through the origin.
const midgard::Body kPlanet = midgard::body_from_json(json::parse(
    R"({"id": "planet", "shape": "sphere_outside", "radius": 1000, "origin": [0, -1000, 0]})"));
const glm::dvec3 kPlaced = glm::normalize(glm::dvec3(0.3, 0.75, -0.6));

// The point on the planet `degrees` round the equator under the start, east of it.
glm::dvec3 on_planet(double ex, double ey, double ez) {
    return kPlanet.origin + glm::normalize(glm::dvec3(ex, ey, ez)) * 1000.0;
}

}  // namespace

TEST_CASE("light models match the shared table") {
    std::ifstream in(fs::path(MIDGARD_SCHEMA_DIR) / "tests" / "light_models.json");
    REQUIRE(in);
    const json table = json::parse(in);
    const glm::dvec3 placed = vec(table.at("placed"));
    int checked = 0;
    for (const json& c : table.at("cases")) {
        CAPTURE(c.dump());
        const auto body = body_named(table.at("bodies"), c.at("body").get<std::string>());
        const midgard::LocalSun sun =
            midgard::local_sun(body, placed, clock_from(c.at("clock")), vec(c.at("point")));
        CHECK(sun.phase == doctest::Approx(c.at("phase").get<double>()).epsilon(1e-9));
        CHECK(glm::length(sun.to_sun - vec(c.at("to_sun"))) < 1e-9);
        CHECK(sun.height == doctest::Approx(c.at("height").get<double>()).epsilon(1e-9));
        ++checked;
    }
    CHECK(checked == 288);
}

TEST_CASE("solar phase and time of day undo each other") {
    for (const double daylight : {0.3, 0.5, 0.7})
        for (int i = 0; i < 100; ++i) {
            const double phase = i / 100.0;
            const double t = midgard::time_of_phase(phase, daylight, 0.5);
            // (0 and 1 are the same moment: sunrise.)
            const double back = midgard::phase_of_time(t, daylight, 0.5);
            const double apart = std::abs(back - phase);
            CHECK(std::min(apart, 1.0 - apart) < 1e-12);
        }
    // Sunrise and sunset fall at the calendar's times: 70% daylight round noon.
    CHECK(midgard::time_of_phase(0.0, 0.7, 0.5) == doctest::Approx(0.15));
    CHECK(midgard::time_of_phase(0.5, 0.7, 0.5) == doctest::Approx(0.85));
}

TEST_CASE("Uniform: a planet's noon moves round it with longitude") {
    midgard::SunClock clock;
    clock.time = 0.5;  // noon at the start
    const auto at = [&](const glm::dvec3& p) {
        return midgard::local_sun(kPlanet, kPlaced, clock, p);
    };
    const midgard::LocalSun start = at(glm::dvec3(0.0));
    CHECK(start.phase == doctest::Approx(0.25));
    CHECK(std::asin(start.height) == doctest::Approx(std::asin(kPlaced.y)));  // as placed
    CHECK(at(on_planet(0, -1, 0)).phase == doctest::Approx(0.75));  // midnight on the far side
    // A quarter of the way round, east and west: sunset and sunrise there.
    const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 1, 0), -kPlaced));
    double east_phase = at(kPlanet.origin + glm::normalize(east) * 1000.0).phase;
    double west_phase = at(kPlanet.origin - glm::normalize(east) * 1000.0).phase;
    CHECK(std::min(east_phase, west_phase) < 0.02);                  // one is at sunrise...
    CHECK(std::abs(std::max(east_phase, west_phase) - 0.5) < 0.02);  // ...the other at sunset
    // Noon reaches the east side a quarter of a day earlier.
    for (int i = 0; i < 4; ++i) {
        clock.time = 0.25 + i * 0.25;
        CHECK(at(glm::dvec3(0.0)).phase ==
              doctest::Approx(midgard::phase_of_time(clock.time, 0.5, 0.5)));
    }
}

TEST_CASE("Latitude: polar day in summer, polar night in winter, and seasons at the start") {
    midgard::SunClock clock;
    clock.mode = midgard::SunMode::Latitude;
    clock.tilt = 23.5;
    // The start's latitude is set by the sun's noon height: placed at 48 degrees, 42 north.
    const auto noon_height = [&](double year) {
        clock.time = 0.5;
        clock.year = year;
        return std::asin(midgard::local_sun(kPlanet, kPlaced, clock, glm::dvec3(0.0)).height);
    };
    const double equinox = noon_height(0.0), summer = noon_height(0.25), winter = noon_height(0.75);
    CHECK(glm::degrees(equinox) == doctest::Approx(glm::degrees(std::asin(kPlaced.y))));
    CHECK(glm::degrees(summer - equinox) == doctest::Approx(23.5).epsilon(1e-6));
    CHECK(glm::degrees(equinox - winter) == doctest::Approx(23.5).epsilon(1e-6));
    // The north pole: the planet's axis, through the start's north (away from the noon sun).
    const glm::dvec3 north0 = -glm::normalize(kPlaced - glm::dvec3(0, kPlaced.y, 0));
    const double lat0 = glm::radians(90.0) - std::asin(kPlaced.y);
    const glm::dvec3 axis = north0 * std::cos(lat0) + glm::dvec3(0, 1, 0) * std::sin(lat0);
    const glm::dvec3 pole = kPlanet.origin + axis * 1000.0;
    for (int hour = 0; hour < 24; ++hour) {
        clock.time = hour / 24.0;
        clock.year = 0.25;  // midsummer: the sun never sets
        const midgard::LocalSun day = midgard::local_sun(kPlanet, kPlaced, clock, pole);
        CHECK(day.height > 0.3);
        CHECK(day.phase < 0.5);
        clock.year = 0.75;  // midwinter: it never rises
        const midgard::LocalSun night = midgard::local_sun(kPlanet, kPlaced, clock, pole);
        CHECK(night.height < -0.3);
        CHECK(night.phase >= 0.5);
    }
}

TEST_CASE("inside worlds: the light is overhead at noon and low from the side at dawn") {
    for (const char* settings :
         {R"({"id": "w", "shape": "sphere_inside", "radius": 1000, "origin": [0, 1000, 0]})",
          R"({"id": "w", "shape": "curved_cylinder", "radius": 1000, "length": 5000,
              "origin": [0, 1000, 0]})"}) {
        const midgard::Body world = midgard::body_from_json(json::parse(settings));
        midgard::SunClock clock;
        CAPTURE(settings);
        clock.time = 0.5;  // noon at the start: overhead
        CHECK(midgard::local_sun(world, kPlaced, clock, glm::dvec3(0.0)).height ==
              doctest::Approx(1.0));
        clock.time = midgard::time_of_phase(0.02, 0.5, 0.5);  // just after its sunrise
        const midgard::LocalSun dawn = midgard::local_sun(world, kPlaced, clock, glm::dvec3(0.0));
        CHECK(dawn.phase == doctest::Approx(0.02));
        CHECK(dawn.height > 0.0);
        CHECK(dawn.height < 0.2);  // low, from the side
        // Halfway round the world it's the middle of the night.
        clock.time = 0.5;
        CHECK(midgard::local_sun(world, kPlaced, clock, glm::dvec3(0.0, 2000.0, 0.0)).phase ==
              doctest::Approx(0.75));
    }
}

TEST_CASE("each place's season matches the shared table") {
    std::ifstream in(fs::path(MIDGARD_SCHEMA_DIR) / "tests" / "seasons.json");
    REQUIRE(in);
    const json table = json::parse(in);
    const glm::dvec3 placed = vec(table.at("placed"));
    int checked = 0;
    for (const json& c : table.at("cases")) {
        CAPTURE(c.dump());
        const auto body = body_named(table.at("bodies"), c.at("body").get<std::string>());
        midgard::SunClock clock = clock_from(c.at("clock"));
        clock.season = c.at("clock").at("season").get<double>();
        const midgard::LocalSun sun = midgard::local_sun(body, placed, clock, vec(c.at("point")));
        CHECK(sun.season == doctest::Approx(c.at("season").get<double>()).epsilon(1e-9));
        ++checked;
    }
    CHECK(checked == 108);
}

TEST_CASE("Latitude: summer in the north is winter in the south, and the equator has neither") {
    midgard::SunClock clock;
    clock.mode = midgard::SunMode::Latitude;
    clock.tilt = 23.5;
    clock.year = 0.25;    // the north's midsummer
    clock.season = -0.3;  // the calendar's, ignored here
    // The start's latitude is 90 degrees less the sun's height at noon, as placed.
    const double lat0 = std::acos(-1.0) / 2 - std::asin(kPlaced.y);
    const double start = midgard::local_sun(kPlanet, kPlaced, clock, {0, 0, 0}).season;
    CHECK(start == doctest::Approx(std::min(lat0 / (std::acos(-1.0) / 4), 1.0)));
    const glm::dvec3 north0 = glm::normalize(glm::dvec3(-kPlaced.x, 0.0, -kPlaced.z));
    const glm::dvec3 pole_axis = north0 * std::cos(lat0) + glm::dvec3(0, 1, 0) * std::sin(lat0);
    const glm::dvec3 south = kPlanet.origin - pole_axis * 1000.0;
    CHECK(midgard::local_sun(kPlanet, kPlaced, clock, south).season == doctest::Approx(-1.0));
    // After the north's midsummer it's autumn there, and spring in the south.
    clock.year = 0.375;
    CHECK(midgard::local_sun(kPlanet, kPlaced, clock, {0, 0, 0}).autumn > 0.3);
    CHECK(midgard::local_sun(kPlanet, kPlaced, clock, south).autumn < -0.3);
    clock.year = 0.25;
    clock.tilt = 0.0;  // no tilt: the calendar's season everywhere
    CHECK(midgard::local_sun(kPlanet, kPlaced, clock, south).season == doctest::Approx(-0.3));
}
