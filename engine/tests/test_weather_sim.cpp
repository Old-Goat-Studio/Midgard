// SPDX-License-Identifier: Apache-2.0
// The weather simulation (milestone 5.15, step 2).
#include <doctest/doctest.h>

#include <cmath>
#include <map>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "midgard/weather_sim.h"
#include "midgard/world_type.h"

using namespace midgard;
using namespace midgard::weather;

namespace {

WeatherTypes some_types() {
    WeatherTypes t;
    for (const char* id : {"clear", "cloudy", "rain", "thunderstorm", "snow", "sandstorm"})
        t[id] = weather_type_from_json(id, id, "{}");
    return t;
}

WeatherSim sim(const Shape& shape, const char* json, std::uint64_t seed = 7,
               std::optional<Calendar> calendar = std::nullopt, WeatherSim::BiomeAt biome = {}) {
    return WeatherSim(shape, weather_settings_from_json(json), some_types(), seed, calendar,
                      std::move(biome));
}

// A calendar of twelve 30-day months, 24-hour days and two seasons of half a year each.
Calendar two_seasons() {
    Calendar c;
    for (int m = 0; m < 12; ++m) c.months.push_back({"m", 30});
    c.weekdays = {"d"};
    c.seasons = {{"summer", 0.5, 0.6, 10, 25}, {"winter", 0.5, 0.4, -10, 5}};
    return c;
}

constexpr std::int64_t kHour = 3600, kDay = 24 * kHour;

const char* kTemperate = R"({"cell": 2000,
    "tables": [{"name": "Temperate", "weather": {"clear": 3, "rain": 1}}],
    "lasts": {"clear": [2, 6], "rain": [1, 3]}})";

}  // namespace

TEST_CASE("weather settings are read, with numbers brought into range") {
    const WeatherSettings s = weather_settings_from_json(R"({"cell": 3000,
        "tables": [{"name": "Hills", "biomes": ["Hills"], "seasons": [0, 2],
                    "weather": {"clear": 2, "rain": 1, "fog": 0},
                    "after": {"thunderstorm": {"rain": 3, "cloudy": 1}}}],
        "lasts": {"rain": [6, 1], "fog": 0.01, "default": 4},
        "fronts": {"every": [10, 20], "speed": 12, "radius": [2, 4], "lasts": [5, 9],
                   "weather": {"thunderstorm": 1}},
        "controlled": [{"weather": "clear", "hours": 20}]})");
    CHECK(s.cell == 3000.0);
    REQUIRE(s.tables.size() == 1);
    CHECK(s.tables[0].name == "Hills");
    CHECK(s.tables[0].biomes == std::vector<std::string>{"Hills"});
    CHECK(s.tables[0].seasons == std::vector<int>{0, 2});
    CHECK(s.tables[0].weather == std::map<std::string, double>{{"clear", 2}, {"rain", 1}});
    CHECK(s.tables[0].after.at("thunderstorm").at("rain") == 3.0);
    CHECK(s.lasts.at("rain") == Range{1, 6});
    CHECK(s.lasts.at("fog") == Range{0.25, 0.25});
    CHECK(s.lasts_default == Range{4, 4});
    CHECK(s.fronts.every == Range{10, 20});
    CHECK(s.fronts.speed == Range{12, 12});
    CHECK(s.fronts.weather.at("thunderstorm") == 1.0);
    REQUIRE(s.controlled.size() == 1);
    CHECK(s.controlled[0].hours == 20.0);

    CHECK(weather_settings_from_json("") == WeatherSettings{});
    CHECK(weather_settings_from_json("{}") == WeatherSettings{});
    CHECK(weather_settings_from_json(R"({"cell": 1})").cell == 100.0);
    for (const char* bad :
         {"nope", "[]", R"({"cell": "big"})", R"({"tables": {}})", R"({"tables": [3]})",
          R"({"tables": [{"seasons": ["summer"]}]})", R"({"tables": [{"weather": {"a": "x"}}]})",
          R"({"lasts": {"rain": "long"}})", R"({"fronts": {"speed": [1, 2, 3]}})",
          R"({"controlled": [{"hours": 2}]})"})
        CHECK_THROWS_AS(weather_settings_from_json(bad), WeatherError);
}

TEST_CASE("weather settings' problems name what isn't a weather type") {
    const WeatherSettings s = weather_settings_from_json(R"({
        "tables": [{"name": "Odd", "weather": {"clear": 1, "frogs": 1},
                    "after": {"hail": {"rain": 1}}}, {"name": "Empty"}],
        "fronts": {"weather": {"meteors": 1}}})");
    const auto found = weather_settings_problems(s, some_types());
    const auto has = [&](const char* text) {
        return std::any_of(found.begin(), found.end(),
                           [&](const std::string& p) { return p.find(text) != p.npos; });
    };
    CHECK(has("'Odd' has weather 'frogs'"));
    CHECK(has("'Odd' has weather 'hail'"));
    CHECK(has("'Empty' has no weather"));
    CHECK(has("fronts has weather 'meteors'"));
    CHECK_FALSE(has("'clear'"));
    CHECK(weather_settings_problems(weather_settings_from_json(kTemperate), some_types()).empty());
}

TEST_CASE("a world is cut into weather cells on every face") {
    const Shape planet = Shape::sphere_outside(20000.0, 9.81);
    WeatherSim w = sim(planet, kTemperate);
    const auto [nu, nv] = w.cells_per_face();
    CHECK(nu == int(std::round(planet.face_size().x / 2000.0)));
    CHECK(nv == nu);
    for (int face = 0; face < 6; ++face) {
        const Cell c{face, nu - 1, 3};
        CHECK(w.cell_at(w.centre(c)) == c);
    }
    CHECK(w.cell_at({2, 1.0, 0.0, 0.0}) == Cell{2, nu - 1, 0});  // (the far edge is in)
    WeatherSim small = sim(Shape::bounded_area(500, 300, 9.81), kTemperate);
    CHECK(small.cells_per_face() == std::pair<int, int>{1, 1});
}

TEST_CASE("the same seed gives the same weather; another seed, other weather") {
    const Shape area = Shape::bounded_area(20000, 20000, 9.81);
    WeatherSim a = sim(area, kTemperate, 7), b = sim(area, kTemperate, 7),
               c = sim(area, kTemperate, 8);
    int differ = 0;
    for (int i = 0; i < 10; ++i) {
        const Cell cell{0, i, 9 - i};
        const auto fa = a.forecast(cell, 0, 30 * kDay);
        CHECK(fa == b.forecast(cell, 0, 30 * kDay));
        differ += fa != c.forecast(cell, 0, 30 * kDay);
    }
    CHECK(differ >= 9);
    // Cells side by side have weather of their own.
    CHECK(a.forecast({0, 0, 0}, 0, 30 * kDay) != a.forecast({0, 1, 0}, 0, 30 * kDay));
}

TEST_CASE("the weather at a moment doesn't depend on what was asked before") {
    const Shape area = Shape::bounded_area(20000, 20000, 9.81);
    WeatherSim stepped = sim(area, kTemperate), jumped = sim(area, kTemperate);
    const Cell cell{0, 3, 4};
    std::map<std::int64_t, std::string> seen;
    for (std::int64_t t = 0; t < 40 * kDay; t += 600) seen[t] = stepped.at(cell, t).weather;
    // Straight to the end, then back to the start, then here and there.
    CHECK(jumped.at(cell, 40 * kDay - 600).weather == seen.at(40 * kDay - 600));
    CHECK(jumped.at(cell, 0).weather == seen.at(0));
    for (const std::int64_t t : {17 * kDay + 1200, 3 * kDay, 39 * kDay, kHour / 6})
        CHECK(jumped.at(cell, t).weather == seen.at(t));
    // Before the weather starts, it's as it starts.
    CHECK(jumped.at(cell, -5 * kDay).weather == seen.at(0));
}

TEST_CASE("over a long run, each weather is there as much of the time as its table says") {
    const Shape area = Shape::bounded_area(20000, 20000, 9.81);
    WeatherSim w = sim(area, kTemperate);
    std::map<std::string, double> time;
    double total = 0.0;
    for (int i = 0; i < 10; ++i)
        for (const Spell& s : w.forecast({0, i, i}, 0, 400 * kDay)) {
            time[s.weather] += double(s.end - s.start);
            total += double(s.end - s.start);
        }
    CHECK(time["clear"] / total == doctest::Approx(0.75).epsilon(0.03));
    CHECK(time["rain"] / total == doctest::Approx(0.25).epsilon(0.08));
    CHECK(time.size() == 2);

    // Spells last as long as the settings say.
    WeatherSim r = sim(area, R"({"tables": [{"weather": {"rain": 1, "clear": 1}}],
                                 "lasts": {"rain": [1, 3], "clear": [2, 6]}})");
    for (std::int64_t t = 0; t < 30 * kDay;) {
        const Spell s = r.own_spell({0, 0, 0}, t);
        const double h = double(s.end - s.start) / kHour;
        if (s.weather == "rain") CHECK((h >= 1.0 && h <= 3.0));
        if (s.weather == "clear") CHECK((h >= 2.0 && h <= 6.0));
        t = s.end;
    }
}

TEST_CASE("what follows a weather can be its own (a storm clears through rain)") {
    const Shape area = Shape::bounded_area(20000, 20000, 9.81);
    WeatherSim w = sim(area, R"({"tables": [{"weather": {"clear": 2, "thunderstorm": 1},
        "after": {"thunderstorm": {"rain": 1}, "rain": {"cloudy": 3, "clear": 1}}}],
        "lasts": {"default": [1, 4]}})");
    std::map<std::string, std::map<std::string, int>> next;
    for (int i = 0; i < 10; ++i) {
        std::int64_t t = 0;
        Spell before = w.own_spell({0, i, 0}, t);
        while (t < 200 * kDay) {
            t = before.end;
            const Spell s = w.own_spell({0, i, 0}, t);
            ++next[before.weather][s.weather];
            before = s;
        }
    }
    CHECK(next["thunderstorm"].size() == 1);
    CHECK(next["thunderstorm"]["rain"] > 100);
    const double cloudy = next["rain"]["cloudy"], clear = next["rain"]["clear"];
    CHECK(next["rain"].size() == 2);
    CHECK(cloudy / (cloudy + clear) == doctest::Approx(0.75).epsilon(0.1));
    CHECK(next["cloudy"].count("thunderstorm") + next["clear"].count("thunderstorm") > 0);
}

TEST_CASE("the weather follows the seasons and the biomes") {
    const Shape area = Shape::bounded_area(20000, 20000, 9.81);
    const auto biome = [](const SurfacePoint& p) {
        return std::string(p.u < 0.5 ? "Desert" : "Forest");
    };
    WeatherSim w = sim(area, R"({"cell": 2000, "tables": [
        {"name": "Any", "weather": {"rain": 1}},
        {"name": "Summer", "seasons": [0], "weather": {"clear": 1}},
        {"name": "Winter", "seasons": [1], "weather": {"snow": 1}},
        {"name": "Desert", "biomes": ["Desert"], "weather": {"sandstorm": 1}},
        {"name": "Desert winter", "biomes": ["Desert"], "seasons": [1],
         "weather": {"cloudy": 1}}],
        "lasts": {"default": [2, 6]}})",
                       7, two_seasons(), biome);
    const Calendar cal = two_seasons();
    const Cell forest{0, 8, 2}, desert{0, 1, 2};
    CHECK(w.biome(forest) == "Forest");
    CHECK(w.biome(desert) == "Desert");
    int summer = 0, winter = 0;
    for (std::int64_t day = 1; day < 360; ++day) {
        const std::int64_t t = w.origin() + day * kDay + 12 * kHour;
        const int season = date_at(cal, t).season;
        if (season != date_at(cal, t - kDay).season) continue;  // (a spell may cross into it)
        CHECK(w.at(forest, t).weather == (season == 0 ? "clear" : "snow"));
        CHECK(w.at(desert, t).weather == (season == 0 ? "sandstorm" : "cloudy"));
        (season == 0 ? summer : winter)++;
    }
    CHECK(summer > 100);
    CHECK(winter > 100);
    // No table fits: no weather.
    WeatherSim none = sim(area, R"({"tables": [{"biomes": ["Moon"], "weather": {"rain": 1}}]})");
    CHECK(none.at({0, 0, 0}, kDay).weather.empty());
}

TEST_CASE("inside worlds can have controlled weather, the same everywhere") {
    const Shape habitat = Shape::curved_cylinder(2000, 8000, 9.81);
    WeatherSim w = sim(habitat, R"({"controlled": [{"weather": "clear", "hours": 20},
        {"weather": "rain", "hours": 4}], "tables": [{"weather": {"snow": 1}}],
        "fronts": {"weather": {"thunderstorm": 1}, "every": 1}})");
    for (int day = 0; day < 5; ++day) {
        const std::int64_t at = day * kDay;
        for (const Cell c : {Cell{0, 0, 0}, Cell{0, 3, 2}}) {
            CHECK(w.at(c, at + 19 * kHour).weather == "clear");
            CHECK(w.at(c, at + 21 * kHour).weather == "rain");
            CHECK(w.at(c, at + 23 * kHour + 3599).weather == "rain");
        }
    }
    const auto days = w.forecast({0, 1, 1}, 10 * kHour, 2 * kDay);
    REQUIRE(days.size() == 4);
    CHECK(days[0] == Spell{"clear", 10 * kHour, 20 * kHour, -1});
    CHECK(days[1] == Spell{"rain", 20 * kHour, kDay, -1});
    CHECK(days[3] == Spell{"rain", kDay + 20 * kHour, 2 * kDay, -1});
    CHECK(w.fronts_at(kDay).empty());
}

TEST_CASE("the forecast is what happens, fronts and all") {
    const Shape planet = Shape::sphere_outside(20000.0, 9.81);
    const char* settings = R"({"cell": 2000,
        "tables": [{"weather": {"clear": 3, "cloudy": 2, "rain": 1}}],
        "lasts": {"default": [1, 5]},
        "fronts": {"every": [2, 6], "speed": [10, 20], "radius": [2, 4], "lasts": [6, 18],
                   "weather": {"thunderstorm": 1}}})";
    WeatherSim forecaster = sim(planet, settings), game = sim(planet, settings);
    int fronts_seen = 0;
    for (const Cell cell : {Cell{0, 3, 4}, Cell{4, 8, 8}, Cell{2, 0, 15}}) {
        const std::int64_t from = 2 * kDay + 1234;
        const auto spells = forecaster.forecast(cell, from, from + 3 * kDay);
        REQUIRE(!spells.empty());
        CHECK(spells.front().start == from);
        CHECK(spells.back().end == from + 3 * kDay);
        for (size_t n = 1; n < spells.size(); ++n) CHECK(spells[n].start == spells[n - 1].end);
        // The game, a frame now and then, sees exactly that.
        size_t n = 0;
        for (std::int64_t t = from; t < from + 3 * kDay; t += 37) {
            while (t >= spells[n].end) ++n;
            const Now now = game.at(cell, t);
            CHECK(now.weather == spells[n].weather);
            CHECK(now.front == spells[n].front);
            fronts_seen += now.front >= 0;
        }
    }
    CHECK(fronts_seen > 0);
}

TEST_CASE("fronts cross a cylinder's join") {
    const Shape ring = Shape::flat_cylinder(40000, 20000, 9.81);
    WeatherSim w = sim(ring, R"({"cell": 2000, "tables": [{"weather": {"clear": 1}}],
        "fronts": {"every": [3, 6], "speed": 15, "radius": [1.5, 2], "lasts": [20, 30],
                   "weather": {"rain": 1}}})");
    int crossings = 0;
    for (std::int64_t t = 0; t < 30 * kDay && crossings < 5; t += 60) {
        for (const Front& f : w.fronts_at(t)) {
            // (Just before the join, and still over the ring: fronts leave it at its ends.)
            if (f.centre.u < 0.985 || f.centre.v < 0.1 || f.centre.v > 0.9) continue;
            // The cell just past the join, on the other end of the ring, is under it.
            const Cell past{0, 0, w.cell_at(f.centre).j};
            const Now now = w.at(past, t);
            CHECK(now.front >= 0);
            CHECK(now.weather == "rain");
            ++crossings;
        }
    }
    CHECK(crossings >= 5);
    // And a front's centre wraps: it never leaves the ring.
    for (const Front& f : w.fronts_at(10 * kDay)) CHECK((f.centre.u >= 0.0 && f.centre.u < 1.0));
}

TEST_CASE("fronts travel across a planet's cube faces") {
    const Shape planet = Shape::sphere_outside(20000.0, 9.81);
    WeatherSim w = sim(planet, R"({"cell": 2000, "tables": [{"weather": {"clear": 1}}],
        "fronts": {"every": [6, 12], "speed": 20, "radius": [1.5, 2], "lasts": [24, 36],
                   "weather": {"thunderstorm": 1}}})");
    std::map<int, std::set<int>> faces;  // by front
    std::map<int, glm::dvec3> last;
    for (std::int64_t t = 0; t < 10 * kDay; t += 300) {
        for (const Front& f : w.fronts_at(t)) {
            faces[f.number].insert(f.centre.face);
            const glm::dvec3 p = planet.position(f.centre);
            if (last.count(f.number))  // (a straight run: 300 s at 20 m/s apart)
                CHECK(glm::length(p - last[f.number]) == doctest::Approx(6000.0).epsilon(0.01));
            last[f.number] = p;
            // Wherever it is, on whichever face, the cell under its middle has its weather.
            CHECK(w.at(w.cell_at(f.centre), t).front >= 0);
        }
    }
    int crossed = 0;
    for (const auto& [n, seen] : faces) crossed += seen.size() >= 2;
    CHECK(crossed >= 3);
}

TEST_CASE("a world type's recipe carries its weather") {
    const nlohmann::json j = nlohmann::json::parse(
        R"({"terrain": {}, "weather": {"tables": [{"weather": {"rain": 1}}]}})");
    const WorldRecipe r = recipe_from_json(j);
    CHECK(weather_settings_from_json(r.weather).tables.size() == 1);
    CHECK(recipe_to_json(r).at("weather") == j.at("weather"));
    CHECK(recipe_problems(r).empty());
    WorldRecipe bad = r;
    bad.weather = R"({"tables": 5})";
    const auto problems = recipe_problems(bad);
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].find("weather settings") != std::string::npos);
}
