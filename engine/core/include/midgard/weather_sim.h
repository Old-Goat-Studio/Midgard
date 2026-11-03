// SPDX-License-Identifier: Apache-2.0
// The weather simulation (milestone 5.15): which weather it is, where and when.
//
// A world's surface is cut into weather cells (about `cell` metres across, on every face of its
// shape). Each cell has its own run of weather, spell after spell: a spell's weather is drawn
// from the weather table that fits the cell's biome and the season when the spell starts (the
// table for both the biome and the season, else for the biome, else for the season, else for
// anywhere and any time; then the first), and lasts a while (hours, drawn between the
// two numbers its weather type's "lasts" gives). A table's "weather" says how much of the time
// each kind of weather is there (clear 3, rain 1: rain a quarter of the time); its "after" says
// what may follow a weather instead, by how often (after a thunderstorm, rain 3, cloudy 1).
//
// Fronts are weather that travels: now and then one starts somewhere, moves in a straight line
// (a great circle on a sphere, across its cube faces; across a cylinder's join) at some metres a
// second, and for as long as it lasts every cell whose middle it covers has its weather.
//
// Inside worlds (habitats) can instead have controlled weather: a fixed schedule, the same
// everywhere, repeating (20 hours clear, then 4 hours of rain).
//
// Everything is drawn from the world's seed and the clock, never from what happened while the
// game ran, so the same world always has the same weather, and a forecast is what will happen.
// A cell's run starts at the calendar's start; asking about a moment replays it from there (once:
// each cell remembers where it is).
//
// Settings (JSON), in a world type's recipe as "weather":
//   {"cell": 4000,                                   metres across a weather cell
//    "tables": [{"name": "Temperate", "biomes": [], "seasons": [],   (empty: any; seasons by
//                "weather": {"clear": 3, "rain": 1},                  number, from 0)
//                "after": {"thunderstorm": {"rain": 3, "cloudy": 1}}}],
//    "lasts": {"rain": [1, 6], "default": [2, 8]},   hours a spell lasts, by weather type
//    "fronts": {"every": [24, 72], "speed": [5, 15], "radius": [3, 8], "lasts": [12, 48],
//               "weather": {"thunderstorm": 1, "rain": 2}},      (hours, m/s, cells, hours)
//    "controlled": [{"weather": "clear", "hours": 20}, {"weather": "rain", "hours": 4}]}
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "midgard/calendar.h"
#include "midgard/shape.h"
#include "midgard/weather.h"

namespace midgard::weather {

struct WeatherTable {
    std::string name;
    std::vector<std::string> biomes;        // empty: any
    std::vector<int> seasons;               // 0-based; empty: any
    std::map<std::string, double> weather;  // how much of the time, by weather type
    std::map<std::string, std::map<std::string, double>> after;  // what follows, how often
    bool operator==(const WeatherTable&) const = default;
};

using Range = std::pair<double, double>;

struct Fronts {
    Range every{24.0, 72.0};                // hours from one front starting to the next
    Range speed{5.0, 15.0};                 // metres a second
    Range radius{3.0, 8.0};                 // cells
    Range lasts{12.0, 48.0};                // hours
    std::map<std::string, double> weather;  // how often each; empty: no fronts
    bool operator==(const Fronts&) const = default;
};

struct Controlled {
    std::string weather;
    double hours = 1.0;
    bool operator==(const Controlled&) const = default;
};

struct WeatherSettings {
    double cell = 4000.0;  // metres
    std::vector<WeatherTable> tables;
    std::map<std::string, Range> lasts;  // hours, by weather type
    Range lasts_default{2.0, 8.0};
    Fronts fronts;
    std::vector<Controlled> controlled;  // not empty: this, everywhere, and no tables or fronts
    bool operator==(const WeatherSettings&) const = default;
};

// Settings from their JSON (empty: no weather). WeatherError naming what's wrong. Numbers are
// brought into range (a spell lasts at least a quarter of an hour; a cell is 100 metres or more).
WeatherSettings weather_settings_from_json(std::string_view json);
// What's wrong with settings, given the project's weather types: each a sentence.
std::vector<std::string> weather_settings_problems(const WeatherSettings& s,
                                                   const WeatherTypes& types);

struct Cell {
    int face = 0, i = 0, j = 0;
    auto operator<=>(const Cell&) const = default;
};

// The weather somewhere at a moment: a weather type's id ("" when no table fits), and the
// front it comes from, if one (and in a game, whether a script forces it).
struct Now {
    std::string weather;
    int front = -1;  // the front's number, or -1: the cell's own weather
    bool forced = false;
    bool operator==(const Now&) const = default;
};

// A stretch of the same weather.
struct Spell {
    std::string weather;
    std::int64_t start = 0, end = 0;  // clock seconds; end is when the next starts
    int front = -1;
    bool forced = false;
    bool operator==(const Spell&) const = default;
};

struct Front {
    int number = 0;
    std::string weather;
    std::int64_t start = 0, end = 0;
    double speed = 0.0, radius = 0.0;  // m/s, metres
    SurfacePoint centre;               // where it is (at the moment asked about)
};

class WeatherSim {
public:
    // The biome's name at a point (empty: none).
    using BiomeAt = std::function<std::string(const SurfacePoint&)>;

    WeatherSim(const Shape& shape, WeatherSettings settings, WeatherTypes types, std::uint64_t seed,
               std::optional<Calendar> calendar = std::nullopt, BiomeAt biome_at = {});

    const WeatherSettings& settings() const { return settings_; }
    const WeatherTypes& types() const { return types_; }
    // Where the weather starts: the calendar's start (0 without one). Before it, it's as then.
    std::int64_t origin() const { return origin_; }
    std::int64_t seconds_per_hour() const { return hour_; }

    // The cell a point is in, its middle, how many there are on each face, and its biome.
    Cell cell_at(const SurfacePoint& p) const;
    SurfacePoint centre(const Cell& c) const;
    std::pair<int, int> cells_per_face() const { return {cells_u_, cells_v_}; }
    const std::string& biome(const Cell& c);

    // The weather in a cell at a moment.
    Now at(const Cell& c, std::int64_t time);
    // The weather in a cell from `from` until `until`: its spells in order (the first starting
    // at `from`, the last ending at `until`). Fronts arriving and leaving are found to the
    // second.
    std::vector<Spell> forecast(const Cell& c, std::int64_t from, std::int64_t until);
    // The cell's own spell at a moment (without fronts), start and end exact.
    Spell own_spell(const Cell& c, std::int64_t time);
    // The fronts there are at a moment, with where each one is.
    std::vector<Front> fronts_at(std::int64_t time);

private:
    struct Run {  // a cell's own weather, at its k-th spell
        std::int64_t k = -1;
        Spell spell;
    };
    struct FrontPath {
        std::string weather;
        std::int64_t start = 0, end = 0;
        double speed = 0.0, radius = 0.0;
        glm::dvec3 from{0.0}, heading{0.0};  // sphere: unit vectors; else metres (x, y)
    };

    void advance(const Cell& c, Run& run, std::int64_t time);
    Spell draw(const Cell& c, std::int64_t k, const std::string& before, std::int64_t start);
    int season_at(std::int64_t time) const;
    const WeatherTable* table_for(const std::string& biome, int season) const;
    std::int64_t hours(const Range& r, double roll) const;
    void make_fronts_until(std::int64_t time);
    std::optional<int> front_over(const Cell& c, std::int64_t time);
    glm::dvec3 front_position(const FrontPath& f, std::int64_t time) const;
    bool covers(const FrontPath& f, std::int64_t time, const Cell& c) const;
    Spell controlled_spell(std::int64_t time) const;

    Shape shape_;
    WeatherSettings settings_;
    WeatherTypes types_;
    std::uint64_t seed_;
    std::optional<Calendar> calendar_;
    BiomeAt biome_at_;
    std::int64_t origin_ = 0, hour_ = 3600;
    int cells_u_ = 1, cells_v_ = 1;
    bool sphere_ = false, wraps_ = false;
    std::map<Cell, Run> runs_;
    std::map<Cell, std::string> biomes_;
    std::vector<FrontPath> fronts_;
    std::int64_t fronts_until_ = 0;  // fronts are made up to here
};

}  // namespace midgard::weather
