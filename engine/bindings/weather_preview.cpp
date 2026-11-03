// SPDX-License-Identifier: Apache-2.0
// The editor's weather forecast (milestone 5.15): the game's own weather for a scene's world,
// made as the game makes it (midgard/scene.h's scene_weather), so the Weather tab's preview is
// what the game will have.
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "midgard/calendar.h"
#include "midgard/scene.h"
#include "midgard/terrain.h"
#include "midgard/weather_sim.h"

namespace py = pybind11;

namespace {

using namespace midgard;

// The game's clock's calendar: the standard one, or plain days without seasons.
Calendar game_calendar(const std::filesystem::path& database) {
    try {
        for (Calendar& c : load_calendars(database))
            if (c.standard && c.problems().empty()) return c;
    } catch (const CalendarError&) {
    }
    Calendar plain;
    plain.id = "builtin";
    plain.months.assign(12, {"", 30});
    plain.weekdays.assign(7, "");
    return plain;
}

py::dict forecast(const std::filesystem::path& database, const std::string& scene,
                  const std::string& types_json, const std::string& settings_json,
                  std::array<double, 3> at, double hours, std::optional<std::int64_t> start) {
    SceneSettings settings = Scene::read_settings(database, scene);
    if (!settings_json.empty()) settings.weather = settings_json;  // (the tab's, as edited)
    weather::WeatherTypes types;
    for (const nlohmann::json& t : nlohmann::json::parse(types_json)) {
        weather::WeatherType w = weather::weather_type_from_json(
            t.at("id"), t.value("name", ""), t.value("weather", nlohmann::json::object()).dump());
        types.emplace(w.id, std::move(w));
    }
    if (!settings.weather.empty()) weather::weather_settings_from_json(settings.weather);
    const Calendar calendar = game_calendar(database);
    std::shared_ptr<Terrain> terrain;
    if (settings.body && settings.terrain)
        terrain = std::make_shared<Terrain>(*settings.body, *settings.terrain);
    auto sim = scene_weather(settings, types, calendar,
                             [terrain]() -> const Terrain* { return terrain.get(); });
    py::dict out;
    const std::int64_t from = start.value_or(calendar.start_time());
    const std::int64_t hour = std::max<std::int64_t>(calendar.seconds_per_hour(), 1);
    out["start"] = from;
    out["seconds_per_hour"] = hour;
    py::list spells;
    if (!sim) {
        out["cell"] = py::none();
        out["biome"] = "";
        out["spells"] = spells;
        out["shares"] = py::dict();
        return out;
    }
    const weather::Cell cell = sim->cell_at(weather_point(settings, {at[0], at[1], at[2]}));
    out["cell"] = py::make_tuple(cell.face, cell.i, cell.j);
    out["biome"] = sim->biome(cell);
    const std::int64_t until = from + std::max<std::int64_t>(std::llround(hours * double(hour)), 1);
    for (const weather::Spell& s : sim->forecast(cell, from, until)) {
        py::dict d;
        d["weather"] = s.weather;
        d["start"] = s.start;
        d["end"] = s.end;
        d["front"] = s.front;
        spells.append(d);
    }
    out["spells"] = spells;
    // How much of a year each weather is there (the weather each hour, through a year).
    std::map<std::string, int> count;
    const int year_hours = calendar.year_days(1) * calendar.hours_per_day;
    for (int h = 0; h < year_hours; ++h) ++count[sim->at(cell, from + h * hour).weather];
    py::dict shares;
    for (const auto& [id, n] : count) shares[py::str(id)] = double(n) / double(year_hours);
    out["shares"] = shares;
    return out;
}

}  // namespace

void bind_weather_preview(py::module_& m) {
    m.def("weather_forecast", &forecast, py::arg("database"), py::arg("scene"), py::arg("types"),
          py::arg("settings") = "", py::arg("at") = std::array<double, 3>{},
          py::arg("hours") = 72.0, py::arg("start") = py::none(),
          "The game's weather for a scene's world, at a point, as the game will have it: "
          "{start, seconds_per_hour, cell, biome, spells: [{weather, start, end, front}], "
          "shares: {weather: share of a year}}. types: the weather types as JSON ([{id, name, "
          "weather}]); settings: weather settings as JSON (empty: the scene's or its world "
          "type's). ValueError (RuntimeError) for what can't be read.");
}
