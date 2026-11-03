# SPDX-License-Identifier: Apache-2.0
"""Weather types (milestone 5.15 step 1): ten in every project, kept with undo, checked as the
game is cooked, and read by the engine."""
import pytest
from test_scripting import needs_scripting, simulate, write_script

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


def a_world_type_with_weather(project, weather_settings):
    from midgard_editor import world_types

    project.undo_stack.push(world_types.SaveRecipeCommand(
        project, "wt_moors", {"terrain": {}, "weather": weather_settings}, "Moors"))


def test_a_world_types_weather_tables_are_checked(project, tmp_path):
    a_world_type_with_weather(project, {
        "tables": [{"name": "Wet", "weather": {"rain": 2, "drizzle": 1},
                    "after": {"thunderstorm": {"hail": 1}}},
                   {"name": "Nothing", "weather": {"clear": 0}}],
        "fronts": {"weather": {"meteors": 1}},
        "lasts": {"default": [2, 8], "fog": [1, 2]}})
    found = weather.world_problems(project)
    assert ("World type 'Moors': weather table 'Wet' has weather 'drizzle', which isn't one "
            "of the weather types.") in found
    text = "\n".join(found)
    assert "'hail'" in text and "'meteors'" in text and "'Nothing' has no weather" in text
    assert "'thunderstorm'" not in text and "'default'" not in text and "'fog'" not in text
    result = cook(project, tmp_path / "pkg")
    assert any("drizzle" in w for w in result.warnings)


def test_the_engine_refuses_weather_settings_it_cant_read(project, tmp_path):
    if engine.module() is None:
        pytest.skip("the engine isn't here to check")
    a_world_type_with_weather(project, {"tables": [{"weather": {"rain": 1}}]})
    cook(project, tmp_path / "ok")
    a_world_type_with_weather(project, {"tables": "rainy"})
    with pytest.raises(CookError, match="weather settings"):
        cook(project, tmp_path / "pkg")


STORM_SCRIPT = """
import midgard

def on_start(entity):
    midgard.weather.on_change(changed)
    now = midgard.weather.current()
    print("now", now.id, now == "clear", now.forced)
    midgard.weather.force("thunderstorm", hours=0.02)
    print("forced", midgard.weather.current().id, midgard.weather.current().wind)
    for spell in midgard.weather.forecast(1):
        print("spell", spell.id, round(spell.in_hours, 3), round(spell.hours, 3), spell.forced)
    try:
        midgard.weather.force("frogs")
    except ValueError as e:
        print("refused", e)
    print("types", len(midgard.weather.types))
    midgard.weather.wetness = 0.5
    print("ground", round(midgard.weather.wetness, 2), round(midgard.weather.snow, 2),
          round(midgard.weather.temperature, 1))
    exposure = midgard.weather.exposure()
    print("exposure", exposure.sheltered, exposure.heat, exposure.feels_like <= exposure.air)

def changed(before, now):
    print("changed", before.id, "->", now.id, now.forced)
"""


@needs_scripting
def test_scripts_read_force_and_hear_the_weather(project, tmp_path):
    from midgard_editor.scene_model import Entity

    settings = project.scene_settings()
    settings["weather"] = {"tables": [{"weather": {"clear": 1}}]}
    project.set_scene_settings(settings)
    project.put_entity(Entity("e000081", "Forecaster", None, {
        "transform": {}, "script": {"module": "storm"}}))
    write_script(project, "storm", STORM_SCRIPT)
    package = cook(project, tmp_path / "pkg").out_dir
    # 0.02 hours is 72 game seconds: 144 frames at the clock's usual 30 times.
    dumped, run = simulate(package, 200)
    assert run.returncode == 0, run.stderr
    lines = run.stdout.splitlines()
    assert "now clear True False" in lines
    assert "forced thunderstorm 12.0" in lines
    assert "spell thunderstorm 0.0 0.02 True" in lines
    assert "spell clear 0.02 0.98 False" in lines
    assert any(line.startswith("refused there is no weather type 'frogs'") for line in lines)
    assert "types 10" in lines
    assert "ground 0.5 0.0 10.0" in lines  # (15 degrees without a climate; the storm's -5)
    assert "exposure False 0.0 True" in lines
    changes = [line for line in lines if line.startswith("changed")]
    assert changes == ["changed clear -> thunderstorm True", "changed thunderstorm -> clear False"]
    assert dumped["weather"]["now"] == "clear" and not dumped["weather"]["forced"]
    assert dumped["weather"]["forecast"][0]["weather"] == "clear"


def test_a_heat_source_is_added_and_set_in_the_inspector(project, qtbot):
    from test_physics_editing import form_for

    from midgard_editor.scene_model import Entity

    project.put_entity(Entity("e000082", "Campfire", None, {"transform": {}}))
    form = form_for(qtbot, project, "e000082")
    form.add("heat_source")
    form = form_for(qtbot, project, "e000082")
    assert project.entity("e000082").components["heat_source"] == {"heat": 15.0, "radius": 3.0}
    assert form.heat.value() == 15.0 and form.heat_radius.value() == 3.0
    form.heat.setValue(22.0)
    form.heat_radius.setValue(4.5)
    assert project.entity("e000082").components["heat_source"] == {"heat": 22.0, "radius": 4.5}
    project.undo_stack.undo()
    assert project.entity("e000082").components["heat_source"]["radius"] == 3.0


SCRIPTED_STORM = """
import midgard

def on_start(entity):
    midgard.time.set_scale(60)  # (a game minute a real second)
    midgard.weather.force("thunderstorm", hours=0.5)
"""


@needs_scripting
def test_the_exit_a_scripted_storm_in_a_game(project, tmp_path):
    """Milestone 5.15's exit, in a game made in the editor: a script brings a storm; it darkens
    the sky and soaks the ground; its half hour over, the weather the forecast had comes
    back."""
    from midgard_editor.scene_model import Entity

    settings = project.scene_settings()
    settings["weather"] = {"tables": [{"weather": {"clear": 1}}]}
    project.set_scene_settings(settings)
    project.put_entity(Entity("e000083", "Storm bringer", None, {
        "transform": {}, "script": {"module": "storm_bringer"}}))
    write_script(project, "storm_bringer", SCRIPTED_STORM)
    package = cook(project, tmp_path / "pkg").out_dir
    during, run = simulate(package, 900)  # fifteen real seconds: fifteen game minutes
    assert run.returncode == 0, run.stderr
    w = during["weather"]
    assert w["now"] == "thunderstorm" and w["forced"]
    assert w["look"]["clouds"] > 0.9 and w["look"]["darkness"] > 0.6
    assert w["falling"]["kind"] == 1 and w["falling"]["amount"] > 0.8  # (rain)
    assert w["ground"]["wetness"] > 0.4
    assert w["forecast"][0]["weather"] == "thunderstorm" and w["forecast"][0]["forced"]
    assert w["forecast"][1]["weather"] == "clear"
    after, run = simulate(package, 2400)  # forty real seconds: forty game minutes
    assert run.returncode == 0, run.stderr
    w = after["weather"]
    assert w["now"] == "clear" and not w["forced"]
    assert w["ground"]["wetness"] > 0.3  # (still drying)
