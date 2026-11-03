# SPDX-License-Identifier: Apache-2.0
"""Weather types (milestone 5.15 step 1): ten in every project, kept with undo, checked as the
game is cooked, and read by the engine."""
import pytest

from midgard_editor import engine, weather
from midgard_editor.cook import CookError, cook

TEN = ["ash_fall", "blizzard", "clear", "cloudy", "fog", "rain", "sandstorm", "snow",
       "thunderstorm", "wind"]


def test_a_project_starts_with_ten_weather_types(project):
    types = {w["id"]: w for w in weather.load_all(project)}
    assert sorted(types) == TEN
    assert types["wind"]["name"] == "Windy"
    assert types["blizzard"]["weather"]["falls"] == "snow"
    assert types["thunderstorm"]["weather"]["lightning"] > 0
    # The ten are as the engine wants them.
    assert weather.problems(project, []) == []
    for w in types.values():
        assert set(w["weather"]) <= set(weather.NUMBERS) | {"tint", "falls", "sounds", "thunder"}


def test_weather_types_are_kept_with_undo(project, qtbot):
    hail = {"id": weather.new_id(project, "Hail storm"), "name": "Hail storm",
            "weather": {"clouds": 1, "falls": "rain", "amount": 0.8}}
    assert hail["id"] == "hail_storm"
    with qtbot.waitSignal(project.weather_changed):
        project.undo_stack.push(weather.WeatherCommand(project, None, hail, ""))
    assert weather.load(project, "hail_storm")["weather"]["amount"] == 0.8

    # Changes to the same setting in a row are one step.
    for amount in (0.5, 0.3):
        new = {**hail, "weather": {**hail["weather"], "amount": amount}}
        project.undo_stack.push(weather.WeatherCommand(project, weather.load(project, "hail_storm"),
                                                       new, "amount", field="amount"))
    assert weather.load(project, "hail_storm")["weather"]["amount"] == 0.3
    project.undo_stack.undo()
    assert weather.load(project, "hail_storm")["weather"]["amount"] == 0.8
    project.undo_stack.undo()
    assert weather.load(project, "hail_storm") is None

    rain = weather.load(project, "rain")
    project.undo_stack.push(weather.WeatherCommand(project, rain, None, ""))
    assert weather.load(project, "rain") is None
    project.undo_stack.undo()
    assert weather.load(project, "rain") == rain
    assert weather.new_id(project, "Rain") == "rain_2"


def test_what_is_wrong_with_a_weather_type_is_said_plainly(project):
    events = [{"id": "sfx_rain", "name": "Rain"}]
    found = weather.problems_of({"clouds": 3, "wind": "strong", "tint": [1, 1],
                                 "falls": "frogs", "sounds": ["sfx_rain", {"event": "sfx_hiss"}],
                                 "thunder": "sfx_boom"}, events)
    text = "\n".join(found)
    assert "clouds is 3; it is between 0 and 1, so 1 is used" in text
    assert "wind is 'strong', not a number" in text
    assert "tint isn't three numbers" in text
    assert "'frogs' falls from its sky" in text
    assert "sfx_hiss" in text and "sfx_boom" in text
    assert "sfx_rain" not in text
    assert weather.problems_of({"sounds": [{"event": "Rain"}]}, events) == []  # (by name too)


def test_the_cook_warns_about_weather_sounds_the_project_lacks(project, tmp_path):
    weather.put(project, {"id": "drizzle", "name": "Drizzle",
                          "weather": {"falls": "rain", "amount": 0.2,
                                      "sounds": [{"event": "sfx_drizzle"}]}})
    result = cook(project, tmp_path / "pkg")
    assert any("Weather 'Drizzle'" in w and "sfx_drizzle" in w for w in result.warnings)


def test_the_engine_refuses_a_weather_type_that_isnt_one(project, tmp_path):
    if engine.module() is None:
        pytest.skip("the engine isn't here to check")
    weather.put(project, {"id": "frogs", "name": "Frogs", "weather": {"falls": "frogs"}})
    with pytest.raises(CookError, match="Frogs"):
        cook(project, tmp_path / "pkg")
