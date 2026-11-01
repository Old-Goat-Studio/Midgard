# SPDX-License-Identifier: Apache-2.0
"""Calendars and the clock (milestone 5.7): every project has the standard calendar, older
projects gain it, the cook refuses calendars that can't work, scripts read and move the clock,
and a world keeps it between runs."""
import sqlite3

import pytest
from test_scripting import needs_scripting, scripting_runtime, simulate, write_script

from midgard_editor import calendars, contract, engine
from midgard_editor.cook import CookError, cook


def test_new_projects_have_the_standard_calendar(project):
    with project.connect() as con:
        rows = con.execute("SELECT id, standard, hours_per_day FROM calendar").fetchall()
        months = con.execute("SELECT COUNT(*), SUM(days) FROM calendar_month "
                             "WHERE calendar = 'cal_standard'").fetchone()
        name = con.execute("SELECT english FROM string WHERE key = 'cal_standard.month.3'"
                           ).fetchone()
    assert rows == [("cal_standard", 1, 24)]
    assert months == (12, 365)
    assert name == ("April",)


def test_older_projects_gain_the_standard_calendar_when_they_open(tmp_path):
    db = tmp_path / "old.db"
    con = sqlite3.connect(db)
    for number, path in contract.migrations():
        if number < 9:
            con.executescript(path.read_text("utf-8"))
    con.execute("PRAGMA user_version = 8")
    con.commit()
    con.close()
    assert contract.migrate(db) == len(contract.migrations())
    con = sqlite3.connect(db)
    assert con.execute("SELECT COUNT(*) FROM calendar_season").fetchone() == (4,)
    con.close()


@pytest.mark.skipif(engine.module() is None, reason="engine module not built")
def test_the_cook_refuses_a_calendar_that_cant_work(project, tmp_path):
    cook(project, tmp_path / "ok")  # the standard calendar as it comes
    with project.connect() as con:
        con.execute("UPDATE calendar_season SET share = 0.4 WHERE idx = 0")
    with pytest.raises(CookError, match="shares add up to"):
        cook(project, tmp_path / "bad")


# ---- the clock in the running game ------------------------------------------------------------

START = 90 * 86400 + 6 * 3600  # year 1, April 1st, 06:00 on the standard calendar

CLOCK = """
import midgard

hours = 0
frames = 0

def count_hour():
    global hours
    hours += 1

def on_start(entity):
    t = midgard.time.now()
    print("start", t.year, t.month, t.day, t.hour, t.month_name, t.weekday, t.season)
    print("standard", midgard.time.standard().time == t.time == midgard.time.seconds)
    print("night", midgard.time.is_day(), midgard.time.solar_phase() > 0.5, midgard.time.scale)
    midgard.time.on("hour", count_hour)
    midgard.time.on("dusk", lambda: print("dusk, now", midgard.time.now().hour))
    for bad in (lambda: midgard.time.on("noon", count_hour), lambda: midgard.time.set_scale(-1),
                lambda: midgard.time.advance(hours=-1), lambda: midgard.time.advance(skip="x")):
        try:
            bad()
        except ValueError:
            print("refused")
    midgard.time.advance(hours=13)  # 06:00 to 19:00, past sunset

def on_update(entity, dt):
    global frames
    frames += 1
    if frames == 3:
        print("hours", hours, "now", midgard.time.now().hour, midgard.time.is_day(),
              midgard.time() > 0)
"""


@needs_scripting
def test_scripts_read_and_move_the_clock(project, tmp_path):
    player = project.entity("e000004")
    player.components["script"] = {"module": "clock"}
    project.put_entity(player)
    write_script(project, "clock", CLOCK)
    scene, run = simulate(cook(project, tmp_path / "pkg").out_dir, 3)
    assert run.returncode == 0, run.stderr
    lines = run.stdout.splitlines()
    assert "start 1 4 1 6 April Laugardagr Winter" in lines
    assert "standard True" in lines
    assert "night False True 30.0" in lines
    assert lines.count("refused") == 4
    assert "dusk, now 19" in lines  # (sunset is at 16:42 on April 1st)
    assert "hours 13 now 19 False True" in lines
    assert scene["time"] == START + 13 * 3600 + 1  # and three frames at 30x: 1.5 s


@needs_scripting
def test_a_world_keeps_its_clock_between_runs(project, tmp_path):
    package = cook(project, tmp_path / "pkg").out_dir
    world = tmp_path / "world"
    scene, run = simulate(package, 60, "--world", str(world))  # 1 s at 30x
    assert run.returncode == 0, run.stderr
    assert scene["time"] == START + 30
    assert (world / "world.json").is_file() and (world / "world.db").is_file()
    scene, _run = simulate(package, 2, "--world", str(world))
    assert scene["time"] == START + 31  # carried on from where it was saved
    scene, _run = simulate(package, 2)  # no world: a new game
    assert scene["time"] == START + 1


# ---- the Calendar dock -------------------------------------------------------------------------
needs_engine = pytest.mark.skipif(engine.module() is None, reason="engine module not built")


@pytest.fixture
def dock(qtbot, project):
    from midgard_editor.docks.calendar_panel import CalendarPanel

    panel = CalendarPanel()
    qtbot.addWidget(panel)
    panel.set_project(project)
    return panel


def standard(project) -> dict:
    return calendars.load(project, "cal_standard")


@needs_engine
def test_the_dock_shows_the_standard_calendar_from_its_start(dock):
    assert dock.chooser.currentText() == "Standard (standard)"
    assert dock.standard.isChecked() and not dock.standard.isEnabled()  # (there is one)
    assert dock.hours_per_day.value() == 24 and dock.real_minutes.value() == 48
    assert "30 times as fast" in dock.scale_label.text()
    assert dock.months.rowCount() == 12 and dock.months.item(1, 0).text() == "February"
    assert dock.seasons.rowCount() == 4 and dock.moons.rowCount() == 1
    # The preview starts where a new game does: April 1st, year 1, 06:00, before sunrise.
    assert dock.date_label.text().startswith("Laugardagr, April 1, Year 1 · Winter")
    assert "sunrise 07:17, sunset 16:42" in dock.date_label.text()
    assert dock.time_label.text().startswith("06:00") and "(night)" in dock.time_label.text()
    assert len(dock.year_strip.daylight) == 365
    assert min(dock.year_strip.daylight) == pytest.approx(0.35, abs=0.002)
    assert max(dock.year_strip.daylight) == pytest.approx(0.65, abs=0.002)
    assert "-hour day" in dock.rolled.text()
    assert dock.problems.text() == ""


@needs_engine
def test_every_calendar_change_is_undoable(dock, project):
    stack = project.undo_stack
    before = standard(project)
    dock.hours_per_day.setValue(20)
    dock.hours_per_day.setValue(21)  # a number being dragged: one step
    assert standard(project)["hours_per_day"] == 21 and stack.count() == 1
    dock.months.item(0, 0).setText("Þorri")  # a table edit
    dock.months.item(0, 1).setText("29")
    assert standard(project)["months"][0] == {"name": "Þorri", "days": 29}
    assert project.string("cal_standard.month.0") == "Þorri"  # a translatable string
    dock._add_row("seasons")
    assert len(standard(project)["seasons"]) == 5
    assert "season 5 has no share" in dock.problems.text()
    dock.seasons.setCurrentCell(4, 0)
    dock._remove_row("seasons", dock.seasons)
    assert len(standard(project)["seasons"]) == 4
    dock.name.setText("Northern")
    dock.name.editingFinished.emit()
    assert dock.chooser.currentText() == "Northern (standard)"
    dock.leap_every.setValue(0)
    assert standard(project)["leap_every"] == 0

    while stack.canUndo():
        stack.undo()
    assert standard(project) == before
    assert dock.hours_per_day.value() == 24 and dock.months.item(0, 0).text() == "January"
    while stack.canRedo():
        stack.redo()
    assert standard(project)["name"] == "Northern"


@needs_engine
def test_the_dock_reports_calendars_that_cant_work(dock, project):
    dock.seasons.item(0, 1).setText("40")  # winter 40%: the shares add up to 115%
    assert "shares add up to" in dock.problems.text()
    assert "No preview" in dock.date_label.text()
    project.undo_stack.undo()
    assert dock.problems.text() == "" and "April 1" in dock.date_label.text()


@needs_engine
def test_new_duplicate_delete_and_the_standard_calendar(dock, project, monkeypatch):
    from PySide6.QtWidgets import QMessageBox

    stack = project.undo_stack
    martian = dock.new_calendar("Mars Base")
    assert martian == "cal_mars_base"
    assert dock.chooser.currentData() == martian and not dock.standard.isChecked()
    copy_id = dock.duplicate()
    assert copy_id == "cal_mars_base_copy"
    assert len(calendars.load_all(project)) == 3

    dock.select(martian)
    dock.standard.setChecked(True)  # one undo step for both calendars
    found = {c["id"]: c["standard"] for c in calendars.load_all(project)}
    assert found == {"cal_standard": False, martian: True, copy_id: False}
    assert stack.undoText() == "Make Mars Base the standard calendar"
    stack.undo()
    assert calendars.load(project, "cal_standard")["standard"]

    shown = []
    monkeypatch.setattr(QMessageBox, "information", lambda *a: shown.append(a))
    dock.select("cal_standard")
    dock.delete_calendar(confirm=False)  # the standard calendar stays
    assert shown and calendars.load(project, "cal_standard") is not None
    dock.select(copy_id)
    dock.delete_calendar(confirm=False)
    assert calendars.load(project, copy_id) is None
    assert project.string(f"{copy_id}.month.0") is None  # its names went with it
    stack.undo()
    assert calendars.load(project, copy_id)["months"][3]["name"] == "April"


@needs_engine
def test_the_scrubber_gives_the_time_and_solar_phase(dock):
    scrubbed = []
    dock.time_scrubbed.connect(lambda t, phase: scrubbed.append((t, phase)))
    noon = 12 * 3600
    dock.scrub(noon)
    t, phase = scrubbed[-1]
    assert t == START - 6 * 3600 + noon  # April 1st, 12:00
    assert phase == pytest.approx(0.25)  # solar noon
    assert dock.time_label.text().startswith("12:00") and "(day)" in dock.time_label.text()
    assert dock.day_bar.time == noon
    dock.day_bar.time_picked.emit(20 * 3600)  # clicked on the day bar: evening, night
    assert scrubbed[-1][1] > 0.5
    dock.year_strip.day_picked.emit(200)  # another day of the year, the same time
    assert "July 20" in dock.date_label.text() and "Summer" in dock.date_label.text()
    assert scrubbed[-1][0] == START - 6 * 3600 + (200 - 90) * 86400 + 20 * 3600


@needs_engine
def test_the_scrubber_gives_the_season_for_the_environments_tints(dock):
    seasons = []
    dock.season_scrubbed.connect(seasons.append)
    dock.year_strip.day_picked.emit(200)  # July 20: summer
    dock.scrub(12 * 3600)
    summer = seasons[-1]
    dock.year_strip.day_picked.emit(20)  # January 21: winter
    dock.scrub(12 * 3600)
    assert summer > 0.5 and seasons[-1] < -0.5


def test_a_sun_can_follow_the_clock(qtbot, project):
    from midgard_editor.docks.entity_form import EntityForm

    form = EntityForm(project, "e000002")  # the new scene's Sun
    qtbot.addWidget(form)
    assert form.follows_clock is not None and not form.follows_clock.isChecked()
    form.follows_clock.setChecked(True)
    assert project.entity("e000002").components["light"]["follows_clock"] is True
    project.undo_stack.undo()
    assert "follows_clock" not in project.entity("e000002").components["light"]
    assert not form.follows_clock.isChecked()


@needs_scripting
def test_the_scrubber_moves_the_running_games_clock(project, qtbot, tmp_path):
    from midgard_editor.live_link import PlaySession, State

    received = []
    session = PlaySession(project)
    session.message_received.connect(received.append)

    def game_time() -> int:
        count = len(received)
        session.send({"type": "scene.dump"})
        qtbot.waitUntil(lambda: any(m.get("type") == "scene" for m in received[count:]),
                        timeout=5000)
        return next(m for m in received[count:] if m.get("type") == "scene")["scene"]["time"]

    try:
        session.start([str(scripting_runtime()), "--headless"],
                      cook(project, tmp_path / "pkg").out_dir)
        qtbot.waitUntil(lambda: session.state == State.PLAYING, timeout=20000)
        assert START <= game_time() < START + 3600
        midnight = START - 6 * 3600 + 86400 * 100  # a hundred days on, at midnight
        assert session.set_time(midnight)
        assert midnight <= game_time() < midnight + 60  # (it keeps running at 30x)
    finally:
        session.stop()


def test_the_main_window_has_the_calendar_dock(window, project):
    assert "calendar" in window._layout_record()["panels"]
    window.set_project(project)
    assert window.calendar_panel.chooser.currentData() == "cal_standard"
    window._time_scrubbed(START, 0.9)  # (no 3D view offscreen, and no game running: no harm)


# ---- the exit check (milestone 5.7) -------------------------------------------------------------
HOUR = 3600
# Times of the 20-hour day to look at: around sunrise (03:00), noon (10:00) and sunset (17:00),
# and the night.
NIGHT = (0 * HOUR, 1 * HOUR + 1800, 2 * HOUR + 2700, 17 * HOUR + 900, 18 * HOUR, 19 * HOUR + 1800)
DAY = (3 * HOUR + 900, 5 * HOUR, 10 * HOUR, 14 * HOUR, 16 * HOUR + 2700)


def twenty_hour_day(dock, project) -> None:
    """The standard calendar made a 20-hour day with 70% daylight all year (no seasons), in the
    dock, and the scene's sun set to follow the clock."""
    dock._set("hours_per_day", 20, "hours a day")
    dock._set("daylight", 0.7, "daylight")
    dock._set("seasons", [], "seasons")
    sun = project.entity("e000002")
    sun.components["light"]["follows_clock"] = True
    project.put_entity(sun)


def offscreen_view(project):
    """The project's scene in an offscreen engine view, from its camera (as the Viewport dock
    shows it), or None without a GPU."""
    import json

    from midgard_editor import meshes
    from midgard_editor.scene_model import parents_first

    view = engine.module().Viewport()
    try:
        view.attach_offscreen(96, 64)
    except RuntimeError:
        return None
    view.set_settings(json.dumps(project.scene_settings()))
    for entity in parents_first(project.entities()):
        asset = entity.components.get("mesh", {}).get("asset", "")
        if asset in meshes.PRIMITIVES:
            view.set_mesh(asset, meshes.to_glb(meshes.primitive(asset)))
        view.put_entity(json.dumps({"id": entity.id, "name": entity.name, "parent": entity.parent,
                                    "components": entity.components}))
    camera = project.entity("e000001").components["transform"]["position"]
    view.set_camera(camera, [0.0, 0.5, 0.0], 55.0)
    return view


def brightness(view) -> float:
    view.render()
    pixels = view.read_pixels()
    return sum(pixels[i] + pixels[i + 1] + pixels[i + 2] for i in range(0, len(pixels), 4)) / (
        3 * 255 * len(pixels) / 4)


@needs_engine
def test_exit_check_the_scrubber_sweeps_a_20_hour_day_in_the_editor(dock, project):
    """Milestone 5.7's exit, in the editor: a 20-hour day with 70% daylight rises at 03:00 and
    sets at 17:00, and the scrubber takes the viewport's sun through it."""
    twenty_hour_day(dock, project)
    assert dock.day_bar.day_length == 20 * HOUR
    assert dock.day_bar.sunrise == pytest.approx(3 * HOUR, abs=1)
    assert dock.day_bar.sunset == pytest.approx(17 * HOUR, abs=1)
    scrubbed = []
    dock.time_scrubbed.connect(lambda t, phase: scrubbed.append(phase))
    for at, phase in ((3 * HOUR, 0.0), (10 * HOUR, 0.25), (17 * HOUR, 0.5)):
        dock.scrub(at)
        assert scrubbed[-1] == pytest.approx(phase, abs=1e-6)
        assert dock.time_label.text().startswith(f"{at // HOUR:02d}:00")

    view = offscreen_view(project)
    if view is None:
        pytest.skip("no offscreen rendering here")

    def seen(at: int) -> float:
        dock.scrub(at)
        view.solar_phase = scrubbed[-1]  # as the main window passes it to the Viewport dock
        return brightness(view)

    night = [seen(at) for at in NIGHT]
    day = [seen(at) for at in DAY]
    noon = seen(10 * HOUR)
    assert min(day) > max(night) + 0.02, (night, day)  # light from sunrise to sunset only
    assert noon >= max(day) - 1e-6  # brightest at noon
    view.solar_phase = None  # the scrubber let go: the sun back where it was placed


@needs_engine
@needs_scripting
def test_exit_check_the_scrubber_sweeps_a_20_hour_day_in_the_running_game(dock, project, qtbot,
                                                                          tmp_path):
    """Milestone 5.7's exit, in the running game: the scrubber's times move the game's clock,
    and its sun rises at 03:00 and sets at 17:00 on the 20-hour day."""
    from midgard_editor.live_link import PlaySession, State

    twenty_hour_day(dock, project)
    received = []
    session = PlaySession(project)
    session.message_received.connect(received.append)
    scrubbed = []
    dock.time_scrubbed.connect(lambda t, phase: scrubbed.append(t))

    def sun_at(at: int) -> dict:
        dock.scrub(at)  # the dock gives the moment; the main window sends it on while playing
        assert session.set_time(scrubbed[-1])
        count = len(received)
        session.send({"type": "scene.dump"})
        qtbot.waitUntil(lambda: any(m.get("type") == "scene" for m in received[count:]),
                        timeout=5000)
        scene = next(m for m in received[count:] if m.get("type") == "scene")["scene"]
        assert scrubbed[-1] <= scene["time"] < scrubbed[-1] + 120  # (it runs on at 25x)
        return scene

    try:
        session.start([str(scripting_runtime()), "--headless"],
                      cook(project, tmp_path / "pkg").out_dir)
        qtbot.waitUntil(lambda: session.state == State.PLAYING, timeout=20000)
        for at in NIGHT:
            sun = sun_at(at)["sun"]
            assert sun["height"] < 0.0 and sun["intensity"] < 0.5, (at, sun)
        heights = {}
        for at in DAY:
            scene = sun_at(at)
            assert scene["sun"]["height"] > 0.0 and scene["solar_phase"] < 0.5, (at, scene["sun"])
            heights[at] = scene["sun"]["height"]
        assert max(heights, key=heights.get) == 10 * HOUR  # highest at noon
        assert sun_at(10 * HOUR)["solar_phase"] == pytest.approx(0.25, abs=0.01)
        assert not [m for m in received if m.get("level") in ("warn", "error")], received
    finally:
        session.stop()


# ---- the exit check (milestone 5.8) -------------------------------------------------------------
WORLD_SHAPES = {
    "bounded_area": {"shape": "bounded_area"},
    "flat_cylinder": {"shape": "flat_cylinder"},
    "planet": {"shape": "sphere_outside", "radius": 1000, "origin": [0, -1000, 0]},
    "hollow": {"shape": "sphere_inside", "radius": 1000, "origin": [0, 1000, 0]},
    "habitat": {"shape": "curved_cylinder", "radius": 1000, "origin": [0, 1000, 0]},
}


@needs_engine
@pytest.mark.parametrize("shape", list(WORLD_SHAPES))
def test_exit_check_each_world_shape_has_a_day_and_night_in_the_editor(dock, project, shape):
    """Milestone 5.8's exit: on each world shape, with the Sky linked to the Earth-like
    environment, the scrubber takes the viewport through a day: light from sunrise to sunset,
    brightest about noon, and dark at night."""
    import json

    from midgard_editor import environments

    settings = project.scene_settings()
    settings["body"] = {"id": shape, **WORLD_SHAPES[shape]}
    project.set_scene_settings(settings)
    sky = project.entity("e000005")
    sky.components["sky"]["environment"] = "env_earth"
    project.put_entity(sky)
    view = offscreen_view(project)
    if view is None:
        pytest.skip("no offscreen rendering here")
    for env in environments.load_all(project):
        view.set_environment(env["id"], env["name"], json.dumps(env["curves"]),
                             json.dumps(env["settings"]))

    scrubbed = []
    dock.time_scrubbed.connect(lambda t, phase: scrubbed.append(phase))
    seen = []
    for hour in range(24):
        dock.scrub(hour * HOUR + 1800)
        view.solar_phase = scrubbed[-1]  # as the main window passes it to the Viewport dock
        seen.append((scrubbed[-1], brightness(view)))
    day = [b for p, b in seen if 0.04 < p < 0.46]
    night = [b for p, b in seen if 0.56 < p < 0.94]
    assert day and night
    assert min(day) > max(night) + 0.02, (shape, seen)
    brightest = max(seen, key=lambda s: s[1])[0]
    assert abs(brightest - 0.25) < 0.12, (shape, seen)  # about noon
