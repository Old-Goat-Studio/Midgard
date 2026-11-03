// SPDX-License-Identifier: Apache-2.0
// Weather types (milestone 5.15, step 1).
#include <doctest/doctest.h>

#include "midgard/weather.h"

using namespace midgard::weather;

TEST_CASE("a weather type is read from its row") {
    const WeatherType w = weather_type_from_json("storm", "Storm", R"({
        "clouds": 1, "darkness": 0.75, "tint": [0.8, 0.85, 0.9], "visibility": 1500,
        "falls": "rain", "amount": 0.9, "lightning": 4, "wind": 12, "gusts": 0.7,
        "wetness": 1, "temperature": -5,
        "sounds": [{"event": "sfx_rain", "volume": 0.8}, "sfx_wind"], "thunder": "sfx_thunder"})");
    CHECK(w.id == "storm");
    CHECK(w.name == "Storm");
    CHECK(w.clouds == 1.0f);
    CHECK(w.darkness == doctest::Approx(0.75));
    CHECK(w.tint[1] == doctest::Approx(0.85));
    CHECK(w.visibility == 1500.0f);
    CHECK(w.falls == Falls::rain);
    CHECK(w.amount == doctest::Approx(0.9));
    CHECK(w.lightning == 4.0f);
    CHECK(w.wind == 12.0f);
    CHECK(w.gusts == doctest::Approx(0.7));
    CHECK(w.wetness == 1.0f);
    CHECK(w.temperature == -5.0f);
    REQUIRE(w.sounds.size() == 2);
    CHECK(w.sounds[0].event == "sfx_rain");
    CHECK(w.sounds[0].volume == doctest::Approx(0.8));
    CHECK(w.sounds[1].event == "sfx_wind");
    CHECK(w.sounds[1].volume == 1.0f);
    CHECK(w.thunder == "sfx_thunder");
}

TEST_CASE("an empty weather type is a clear, still sky") {
    const WeatherType w = weather_type_from_json("calm", "Calm", "{}");
    CHECK(w.clouds == 0.0f);
    CHECK(w.darkness == 0.0f);
    CHECK(w.tint == std::array<float, 3>{1, 1, 1});
    CHECK(w.visibility == 0.0f);
    CHECK(w.falls == Falls::none);
    CHECK(w.amount == 0.0f);
    CHECK(w.wind == 0.0f);
    CHECK(w.wetness == 0.0f);
    CHECK(w.temperature == 0.0f);
    CHECK(w.sounds.empty());
    CHECK(w.thunder.empty());
    CHECK(weather_type_to_json(w) == "{}");
}

TEST_CASE("weather type numbers are brought into range") {
    const WeatherType w = weather_type_from_json("x", "X", R"({
        "clouds": 3, "darkness": -1, "tint": [5, -1, 1], "visibility": -10, "falls": "snow",
        "amount": 2, "lightning": 1000, "wind": -4, "gusts": 9, "wetness": 2,
        "temperature": -500, "sounds": [{"event": "s", "volume": 99}]})");
    CHECK(w.clouds == 1.0f);
    CHECK(w.darkness == 0.0f);
    CHECK(w.tint == std::array<float, 3>{2, 0, 1});
    CHECK(w.visibility == 0.0f);
    CHECK(w.amount == 1.0f);
    CHECK(w.lightning == 60.0f);
    CHECK(w.wind == 0.0f);
    CHECK(w.gusts == 1.0f);
    CHECK(w.wetness == 1.0f);
    CHECK(w.temperature == -100.0f);
    CHECK(w.sounds[0].volume == 4.0f);
}

TEST_CASE("something falling with no amount falls moderately; nothing falling has none") {
    CHECK(weather_type_from_json("a", "A", R"({"falls": "ash"})").amount == 0.5f);
    CHECK(weather_type_from_json("b", "B", R"({"falls": "none", "amount": 1})").amount == 0.0f);
    CHECK(weather_type_from_json("c", "C", R"({"falls": "sand"})").falls == Falls::sand);
}

TEST_CASE("what isn't a weather type is reported by name") {
    const auto bad = [](const char* json) {
        CHECK_THROWS_WITH_AS(weather_type_from_json("w", "Muddle", json),
                             doctest::Contains("Muddle"), WeatherError);
    };
    bad("not json");
    bad("[]");
    bad(R"({"clouds": "lots"})");
    bad(R"({"falls": "frogs"})");
    bad(R"({"falls": 2})");
    bad(R"({"tint": [1, 1]})");
    bad(R"({"tint": [1, "red", 1]})");
    bad(R"({"sounds": "sfx_rain"})");
    bad(R"({"sounds": [3]})");
    bad(R"({"thunder": 5})");
    CHECK_THROWS_WITH(weather_type_from_json("w", "W", R"({"falls": "frogs"})"),
                      doctest::Contains("none, rain, snow, ash or sand"));
}

TEST_CASE("a weather type's JSON reads back the same") {
    const WeatherType w = weather_type_from_json("b", "Blizzard", R"({
        "clouds": 1, "darkness": 0.6, "tint": [0.9, 0.93, 1.0], "visibility": 120,
        "falls": "snow", "amount": 1, "wind": 18, "gusts": 0.6, "wetness": 0.4,
        "temperature": -10, "sounds": [{"event": "sfx_howl", "volume": 0.7}],
        "thunder": "sfx_crack"})");
    const std::string json = weather_type_to_json(w);
    const WeatherType again = weather_type_from_json("b", "Blizzard", json);
    CHECK(weather_type_to_json(again) == json);
    CHECK(again.falls == Falls::snow);
    CHECK(again.visibility == 120.0f);
    CHECK(again.sounds == w.sounds);
    CHECK(json.find("0.6000000") == std::string::npos);  // tidy numbers
}

TEST_CASE("what falls has a name") {
    for (const Falls f : {Falls::none, Falls::rain, Falls::snow, Falls::ash, Falls::sand})
        CHECK(falls_from_name(falls_name(f)) == f);
    CHECK_FALSE(falls_from_name("hail").has_value());
}
