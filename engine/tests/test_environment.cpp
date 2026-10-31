// SPDX-License-Identifier: Apache-2.0
// Environments (midgard/environment.h): curves against the shared table
// (schema/tests/environment_curves.json, from tools/reference/environment.py), the cook's checks,
// and the analytic sky standing in for curves that aren't set.
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "midgard/environment.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

TEST_CASE("environment curves match the shared table") {
    std::ifstream in(fs::path(MIDGARD_SCHEMA_DIR) / "tests" / "environment_curves.json");
    REQUIRE(in);
    const json table = json::parse(in);
    const midgard::EnvironmentDef env =
        midgard::environment_from_json("env_test", "Test", table.at("curves").dump(), "{}");
    int checked = 0;
    for (const json& c : table.at("cases")) {
        const midgard::Curve* curve = env.curve(c.at("curve").get<std::string>());
        REQUIRE(curve != nullptr);
        const auto v = curve->at(c.at("phase").get<double>());
        CAPTURE(c.dump());
        if (c.at("value").is_array()) {
            for (size_t i = 0; i < 3; ++i)
                CHECK(v[i] == doctest::Approx(c.at("value")[i].get<double>()).epsilon(1e-5));
        } else {
            CHECK(v[0] == doctest::Approx(c.at("value").get<double>()).epsilon(1e-5));
        }
        ++checked;
    }
    CHECK(checked == 225);
}

TEST_CASE("environments the cook refuses, and JSON that can't be read") {
    CHECK_THROWS(midgard::environment_from_json("e", "E", "{not json", "{}"));
    CHECK_THROWS(midgard::environment_from_json("e", "E", R"({"zenith": 3})", "{}"));
    CHECK_THROWS(midgard::environment_from_json("e", "E", R"({"zenith": [[0.1, "red"]]})", "{}"));
    const auto problems = [](const char* curves, const char* settings = "{}") {
        std::string all;
        for (const std::string& p : midgard::environment_problems(
                 midgard::environment_from_json("env_bad", "Bad", curves, settings)))
            all += p + "\n";
        return all;
    };
    CHECK(problems(R"({"zenith": [[0.5, [0, 0, 1]], [0.2, [0, 0, 1]]]})").find("out of order") !=
          std::string::npos);
    CHECK(problems(R"({"fog_density": [[1.2, 0.01]]})").find("outside the day") !=
          std::string::npos);
    CHECK(problems(R"({"sunlight_intensity": [[0.1, -1]]})").find("negative") != std::string::npos);
    CHECK(problems(R"({"rainbow": [[0.1, 1]]})").find("nothing uses") != std::string::npos);
    CHECK(problems("{}", R"({"sun_size": 0})").find("sun's size") != std::string::npos);
    CHECK(problems(R"({"zenith": [[0.0, [0.1, 0.2, 0.5]], [0.5, [0.0, 0.0, 0.05]]]})").empty());
}

TEST_CASE("an empty environment is the analytic sky; set curves take over from it") {
    const midgard::EnvironmentDef earth =
        midgard::environment_from_json("env_earth", "E", "{}", "{}");
    const float noon = glm::radians(60.0f), night = glm::radians(-30.0f);
    const midgard::render::AnalyticSky analytic = midgard::render::analytic_sky(noon);
    const midgard::EnvironmentLook day = midgard::evaluate(earth, 0.25, noon);
    CHECK(day.sky.zenith == analytic.colors.zenith);
    CHECK(day.sunlight_intensity == doctest::Approx(analytic.sunlight_intensity));
    CHECK(day.sunlight_intensity > 0.5f);
    CHECK(midgard::evaluate(earth, 0.75, night).sunlight_intensity == doctest::Approx(0.0f));
    CHECK_FALSE(day.ambient);        // the scene's own
    CHECK(day.fog_density == 0.0f);  // no fog unless it's set

    const midgard::EnvironmentDef dusk = midgard::environment_from_json(
        "env_dusk", "Dusk",
        R"({"zenith": [[0.0, [0.5, 0.2, 0.1]]], "sunlight_intensity": [[0.0, 2.0], [0.5, 0.0]],
            "fog_density": [[0.0, 0.02]], "ambient": [[0.0, [0.1, 0.1, 0.2]]]})",
        R"({"sun_size": 3})");
    const midgard::EnvironmentLook look = midgard::evaluate(dusk, 0.25, noon);
    CHECK(look.sky.zenith == glm::vec3(0.5f, 0.2f, 0.1f));
    CHECK(look.sky.horizon == analytic.colors.horizon);       // not set: still the analytic sky
    CHECK(look.sunlight_intensity == doctest::Approx(1.0f));  // halfway, eased
    CHECK(look.fog_density == doctest::Approx(0.02f));
    CHECK(look.ambient.has_value());
    CHECK(look.sky.sun_size == 3.0f);
}
