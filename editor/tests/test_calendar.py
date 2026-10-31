# SPDX-License-Identifier: Apache-2.0
"""Calendars in the editor (milestone 5.7): every project has the standard calendar, older
projects gain it, and the cook refuses calendars that can't work."""
import sqlite3

import pytest

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
