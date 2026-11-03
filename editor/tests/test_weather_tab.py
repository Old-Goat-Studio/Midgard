# SPDX-License-Identifier: Apache-2.0
"""The Calendar dock's Weather tab (milestone 5.15 step 8): weather types and the scene's
weather edited with undo, a forecast made by the engine, and a game under the editor following
the edits."""
import pytest

from midgard_editor import engine, sound_events, weather
from midgard_editor.docks.weather_tab import WeatherTab

needs_engine = pytest.mark.skipif(
    engine.module() is None or not hasattr(engine.module(), "weather_forecast"),
    reason="needs the engine module")


@pytest.fixture
def tab(qtbot, project):
    t = WeatherTab()
    qtbot.addWidget(t)
    t.set_project(project)
    return t


def test_the_types_part_edits_weather_types_with_undo(tab, project):
    tab.choose_type("rain")
    assert tab.chosen_type()["id"] == "rain"
    tab.wind.setValue(9.0)
    assert weather.load(project, "rain")["weather"]["wind"] == 9.0
    tab.choose_type("rain")
    tab.clouds.setValue(40.0)  # (percent)
    assert weather.load(project, "rain")["weather"]["clouds"] == 0.4
    tab.choose_type("rain")
    tab.falls.setCurrentIndex(tab.falls.findData("snow"))
    assert weather.load(project, "rain")["weather"]["falls"] == "snow"
    project.undo_stack.undo()
    assert weather.load(project, "rain")["weather"]["falls"] == "rain"

    # Its sounds and thunder, from the project's sound events.
    sound_events.put(project, {"id": "sfx_patter", "name": "Patter",
                               "event": {"sounds": [], "loop": True}})
    tab.choose_type("thunderstorm")
    tab.thunder.setCurrentIndex(tab.thunder.findData("sfx_patter"))
    assert weather.load(project, "thunderstorm")["weather"]["thunder"] == "sfx_patter"

    # New, renamed, deleted.
    before = len(weather.load_all(project))
    tab.new_type()
    assert len(weather.load_all(project)) == before + 1
    assert tab.chosen_type()["name"] == "New weather"
    tab.delete_type()
    assert len(weather.load_all(project)) == before


def test_the_tables_part_makes_the_scenes_weather(tab, project):
    assert weather.own_settings(project) is None
    assert not tab.has_weather.isChecked()
    tab.has_weather.setChecked(True)  # (the starting weather: mostly fair)
    assert weather.own_settings(project) == weather.STARTING_SETTINGS
    tab._set_weight(0, "snow", 4.0)
    assert weather.own_settings(project)["tables"][0]["weather"]["snow"] == 4.0
    tab._set_weight(0, "fog", 0.0)
    assert "fog" not in weather.own_settings(project)["tables"][0]["weather"]
    tab.add_table()
    assert len(weather.own_settings(project)["tables"]) == 2
    assert tab.table_index == 1
    tab._set_table(1, "seasons", [3], "seasons")
    assert weather.own_settings(project)["tables"][1]["seasons"] == [3]
    tab._add_after(1, weather.load_all(project))
    assert weather.own_settings(project)["tables"][1]["after"]
    tab._set_lasts("rain", [6.0, 1.0])
    assert weather.own_settings(project)["lasts"]["rain"] == [1.0, 6.0]
    # Undo walks back, one change at a time, to no weather of its own.
    while project.undo_stack.canUndo():
        project.undo_stack.undo()
    assert weather.own_settings(project) is None

    # A timetable instead, for inside worlds; and fronts.
    tab.has_weather.setChecked(True)
    tab.controlled.setChecked(True)
    assert weather.own_settings(project)["controlled"]
    tab.controlled.setChecked(False)
    tab.fronts_on.setChecked(True)
    assert weather.own_settings(project)["fronts"]["weather"]
    tab._set_front("speed", [20.0, 10.0])
    assert weather.own_settings(project)["fronts"]["speed"] == [10.0, 20.0]
    assert weather.settings_problems(weather.own_settings(project),
                                     {w["id"] for w in weather.load_all(project)}) == []


def test_the_scenes_weather_starts_from_its_world_types(tab, project):
    from midgard_editor import world_types

    project.undo_stack.push(world_types.SaveRecipeCommand(
        project, "wt_moor", {"terrain": {}, "weather": {"tables": [{"weather": {"fog": 1}}]}},
        "Moor"))
    settings = project.scene_settings()
    settings["terrain"] = {"world_type": "wt_moor", "version": 1, "seed": 3}
    project.set_scene_settings(settings)
    tab.refresh()
    assert "comes from its world type" in tab.where.text()
    assert tab.has_weather.isChecked()
    tab._set_weight(0, "rain", 2.0)  # (the first change: a copy, then the change)
    assert weather.own_settings(project)["tables"][0]["weather"] == {"fog": 1, "rain": 2.0}
    assert world_types.recipe(project, "wt_moor")["weather"]["tables"][0]["weather"] == {"fog": 1}


@needs_engine
def test_the_forecast_is_the_engines(tab, project):
    tab.has_weather.setChecked(True)
    tab.refresh_forecast()
    text = tab.forecast_text.text()
    assert "Over a year here:" in text and "Clear" in text
    assert tab.strip.spells
    made = weather.forecast(project, hours=72.0)
    assert made["spells"] == tab.strip.spells  # (from day 0, three days)
    # Only rain: a year of rain.
    tab.edit_settings(lambda n: n["tables"][0].__setitem__("weather", {"rain": 1}), "only rain")
    tab.refresh_forecast()
    assert "Rain 100%" in tab.forecast_text.text()


@needs_engine
def test_a_game_under_the_editor_follows_the_weather_tab(qtbot, tab, project):
    from midgard_editor.about import runtime_candidates
    from midgard_editor.cook import cook
    from midgard_editor.live_link import PlaySession, State
    if not runtime_candidates():
        pytest.skip("engine not built here")
    session = PlaySession(project)
    try:
        received = []
        session.message_received.connect(received.append)
        session.start(runtime_candidates()[0], cook(project).out_dir)
        qtbot.waitUntil(lambda: session.state == State.PLAYING, timeout=20000)
        tab.has_weather.setChecked(True)
        tab.edit_settings(lambda n: n["tables"][0].__setitem__("weather", {"blizzard": 1}),
                          "only blizzards")
        qtbot.waitUntil(lambda: sum(m.get("text") == "applied weather.put"
                                    for m in received) >= 2, timeout=10000)
        session.send({"type": "scene.dump"})
        qtbot.waitUntil(lambda: any(m.get("type") == "scene" for m in received), timeout=5000)
        dumped = next(m for m in received if m.get("type") == "scene")["scene"]
        assert dumped["weather"]["now"] == "blizzard"
        assert not [m for m in received if m.get("level") in ("warn", "error")]
    finally:
        session.stop()
