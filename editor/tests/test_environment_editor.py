# SPDX-License-Identifier: Apache-2.0
"""The Environment editor (milestone 5.8): curves over the day, edited, undone and previewed, and
the running game following."""
import json
from pathlib import Path

import pytest
from test_scripting import needs_scripting, scripting_runtime

from midgard_editor import engine, environments
from midgard_editor.cook import cook

TABLES = Path(__file__).resolve().parents[2] / "schema" / "tests"
needs_engine = pytest.mark.skipif(engine.module() is None, reason="engine module not built")


@pytest.fixture
def dock(qtbot, project):
    from midgard_editor.docks.environment_panel import EnvironmentPanel

    panel = EnvironmentPanel()
    qtbot.addWidget(panel)
    panel.set_project(project)
    return panel


def earth(project) -> dict:
    return environments.load(project, "env_earth")


def choose(dock, name: str) -> None:
    for i in range(dock.curves.count()):
        if dock.curves.item(i).data(0x0100) == name:
            dock.curves.setCurrentRow(i)


def test_the_editors_curves_match_the_engines_table():
    data = json.loads((TABLES / "environment_curves.json").read_text("utf-8"))
    for c in data["cases"]:
        value = environments.evaluate_curve(data["curves"][c["curve"]], c["phase"])
        expected = c["value"]
        if isinstance(expected, list):
            assert value == pytest.approx(expected, abs=1e-12), c
        else:
            assert value == pytest.approx(expected, abs=1e-12), c


def test_the_dock_opens_on_the_earth_like_environment(dock):
    assert dock.chooser.currentData() == "env_earth"
    assert dock.name.text() == "Earth-like"
    assert dock.current_curve() == "zenith"
    assert dock.view.keys == []  # not set: the analytic sky
    assert dock.settings["sun_size"].value() == pytest.approx(1.5)


def test_editing_a_curve_is_undoable_and_a_drag_is_one_step(dock, project):
    choose(dock, "sunlight_intensity")
    depth = project.undo_stack.index()
    dock.view.add_key(0.25, 1.2)
    dock.view.add_key(0.6, 0.0)
    assert earth(project)["curves"]["sunlight_intensity"] == [[0.25, 1.2], [0.6, 0.0]]
    for value in (1.4, 1.6, 1.8):  # a key dragged up
        dock.view.move_key(0, 0.26, value)
    assert earth(project)["curves"]["sunlight_intensity"][0] == [0.26, 1.8]
    assert project.undo_stack.index() == depth + 1  # adding and dragging the keys: one step
    dock.view.remove_key(1)
    assert earth(project)["curves"]["sunlight_intensity"] == [[0.26, 1.8]]
    while project.undo_stack.index() > depth:
        project.undo_stack.undo()
    assert "sunlight_intensity" not in earth(project)["curves"]
    assert dock.view.keys == []  # the dock follows Undo


def test_keys_stay_in_order_as_they_are_dragged_past_each_other(dock, project):
    choose(dock, "fog_density")
    dock.view.add_key(0.1, 0.01)
    dock.view.add_key(0.4, 0.02)
    dock.view.move_key(0, 0.7)  # the first key dragged past the second
    keys = earth(project)["curves"]["fog_density"]
    assert [k[0] for k in keys] == [0.4, 0.7]
    assert dock.view.selected == 1  # still the key being dragged


@needs_engine
def test_start_from_the_analytic_sky_then_clear(dock, project):
    choose(dock, "zenith")
    dock.start_from_analytic()
    keys = earth(project)["curves"]["zenith"]
    assert len(keys) == len(environments.SAMPLES)
    noon = next(v for p, v in keys if p == 0.25)
    assert noon[2] > noon[0]  # the analytic sky is blue at noon
    dock.clear.click()
    assert "zenith" not in earth(project)["curves"]


def test_settings_presets_names_and_new_environments(dock, project):
    dock.settings["sun_size"].setValue(3.0)
    assert earth(project)["settings"]["sun_size"] == 3.0
    dock.inside_atmosphere()
    env = earth(project)
    assert env["curves"]["fog_density"] == [[0.0, 0.0012]]
    assert env["settings"]["fog_height_falloff"] == 0.0
    dock.name.setText("Earth-like (tuned)")
    dock.name.editingFinished.emit()
    assert earth(project)["name"] == "Earth-like (tuned)"

    made = dock.new_environment("Mars dusk")
    assert made == "env_mars_dusk" and dock.chooser.currentData() == made
    copy = dock.duplicate()
    assert copy == "env_mars_dusk_copy"
    dock.delete_environment(confirm=False)
    assert environments.load(project, copy) is None
    project.undo_stack.undo()
    assert environments.load(project, copy) is not None


@needs_engine
def test_the_dock_reports_curves_the_cook_refuses(dock, project):
    env = earth(project)
    env["curves"]["fog_density"] = [[0.6, 0.01], [0.2, 0.02]]
    environments.put(project, env)
    assert "out of order" in dock.problems.text()


def test_the_preview_slider_moves_the_viewports_sun(dock):
    seen = []
    dock.phase_previewed.connect(seen.append)
    dock.preview.setValue(750)
    assert seen[-1] == pytest.approx(0.75)
    assert "midnight" in dock.preview_label.text()
    assert dock.view.preview == pytest.approx(0.75)


def test_the_main_window_has_the_environment_dock(window, project):
    window.set_project(project)
    assert "environment" in window._layout_record()["panels"]
    assert window.environment_panel.chooser.currentData() == "env_earth"


@needs_scripting
def test_the_running_game_follows_environment_edits(project, qtbot, tmp_path):
    from midgard_editor.live_link import PlaySession, State

    sky = project.entity("e000005")
    sky.components["sky"]["environment"] = "env_earth"
    project.put_entity(sky)
    received = []
    session = PlaySession(project)
    session.message_received.connect(received.append)

    def sun() -> dict:
        count = len(received)
        session.send({"type": "scene.dump"})
        qtbot.waitUntil(lambda: any(m.get("type") == "scene" for m in received[count:]),
                        timeout=5000)
        return next(m for m in received[count:] if m.get("type") == "scene")["scene"]["sun"]

    try:
        session.start([str(scripting_runtime()), "--headless"],
                      cook(project, tmp_path / "pkg").out_dir)
        qtbot.waitUntil(lambda: session.state == State.PLAYING, timeout=20000)
        assert session.set_time(90 * 86400 + 12 * 3600)  # noon: the sun is up
        assert sun()["intensity"] > 0.5
        env = earth(project)
        env["curves"]["sunlight_intensity"] = [[0.0, 0.05]]  # the sunlight turned right down
        environments.put(project, env)
        qtbot.waitUntil(lambda: any(m.get("text") == "applied environment.put env_earth"
                                    for m in received), timeout=5000)
        assert sun()["intensity"] == pytest.approx(0.05, abs=0.01)
    finally:
        session.stop()


# ---- seasons' colours -----------------------------------------------------------------------
def test_the_editors_season_tints_match_the_engines_table():
    data = json.loads((TABLES / "seasons.json").read_text("utf-8"))
    for c in data["tints"]:
        got = environments.season_tint(data["settings"], c["season"])
        for key, rgb in c["tint"].items():
            assert got[key] == pytest.approx(rgb, abs=1e-12), c


def test_season_tints_are_set_undone_and_cleared(dock, project):
    depth = project.undo_stack.index()
    dock.set_tint("winter", "sunlight", [0.8, 0.9, 1.0])
    assert earth(project)["settings"]["winter"] == {"sunlight": [0.8, 0.9, 1.0]}
    assert dock.tints[("winter", "sunlight")].styleSheet().endswith("#ccE6ff;".lower()) or \
        "background-color" in dock.tints[("winter", "sunlight")].styleSheet()
    dock.set_tint("winter", "sunlight", [1.0, 1.0, 1.0])  # white: no tint
    assert "winter" not in earth(project)["settings"]
    dock.colder_winter()
    settings = earth(project)["settings"]
    assert settings["winter"] == environments.SEASON_PRESET["winter"]
    assert settings["summer"] == environments.SEASON_PRESET["summer"]
    dock.no_season_tints()
    assert "winter" not in earth(project)["settings"]
    while project.undo_stack.index() > depth:
        project.undo_stack.undo()
    assert "winter" not in earth(project)["settings"]


def test_the_season_slider_previews_in_the_viewport(dock):
    seen = []
    dock.season_previewed.connect(seen.append)
    dock.season.setValue(-100)
    assert seen[-1] == pytest.approx(-1.0) and dock.season_label.text() == "winter"
    dock.season.setValue(80)
    assert seen[-1] == pytest.approx(0.8) and dock.season_label.text() == "summer"
    dock.season.setValue(10)
    assert dock.season_label.text() == "spring or autumn"


@needs_engine
def test_the_engine_tints_winter_light_and_the_cook_takes_them(dock, project):
    dock.colder_winter()
    env = earth(project)
    assert dock.problems.text() == ""
    look = engine.module().Environment(json.dumps(env["curves"]), json.dumps(env["settings"]))
    plain = look.evaluate(0.25, 0.8, 0.0)
    winter = look.evaluate(0.25, 0.8, -1.0)
    assert winter["sunlight"][0] == pytest.approx(plain["sunlight"][0] * 0.82, rel=1e-5)
    assert winter["sunlight"][2] == pytest.approx(plain["sunlight"][2], rel=1e-5)
    assert winter["ambient_tint"] == pytest.approx((0.88, 0.93, 1.0), rel=1e-5)
    assert look.season_tint(1.0)["sunlight"] == pytest.approx((1.0, 0.95, 0.85), rel=1e-5)


@needs_scripting
def test_the_running_game_colours_its_light_by_the_calendars_season(project, qtbot, tmp_path):
    from midgard_editor.live_link import PlaySession, State

    sky = project.entity("e000005")
    sky.components["sky"]["environment"] = "env_earth"
    project.put_entity(sky)
    env = earth(project)
    env["settings"]["winter"] = {"sunlight": [0.5, 0.75, 1.0]}
    environments.put(project, env)
    received = []
    session = PlaySession(project)
    session.message_received.connect(received.append)

    def scene_at(day: int) -> dict:
        assert session.set_time(day * 86400 + 12 * 3600)  # noon on that day of year 1
        count = len(received)
        session.send({"type": "scene.dump"})
        qtbot.waitUntil(lambda: any(m.get("type") == "scene" for m in received[count:]),
                        timeout=5000)
        return next(m for m in received[count:] if m.get("type") == "scene")["scene"]

    try:
        session.start([str(scripting_runtime()), "--headless"],
                      cook(project, tmp_path / "pkg").out_dir)
        qtbot.waitUntil(lambda: session.state == State.PLAYING, timeout=20000)
        seasons = {}
        for day in range(0, 365, 5):
            s = scene_at(day)
            seasons[s["season"]] = s["sun"]["color"]
        winter, summer = seasons[min(seasons)], seasons[max(seasons)]
        assert min(seasons) < -0.99 and max(seasons) > 0.99  # the calendar's year goes round
        assert winter[0] / winter[2] == pytest.approx(0.5 * summer[0] / summer[2], rel=0.02)
    finally:
        session.stop()
