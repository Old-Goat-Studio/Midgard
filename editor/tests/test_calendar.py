# SPDX-License-Identifier: Apache-2.0
"""Calendars and the clock (milestone 5.7): every project has the standard calendar, older
projects gain it, the cook refuses calendars that can't work, scripts read and move the clock,
and a world keeps it between runs."""
import sqlite3

import pytest
from test_scripting import needs_scripting, simulate, write_script

from midgard_editor import contract, engine
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
