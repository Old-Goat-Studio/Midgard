# SPDX-License-Identifier: Apache-2.0
"""Cameras in the editor (milestone 5.6): the view action and the camera component."""
import sqlite3

from midgard_editor import contract


def test_older_projects_gain_the_view_action_when_they_open(tmp_path):
    db = tmp_path / "old.db"
    con = sqlite3.connect(db)
    for number, path in contract.migrations():
        if number < 7:
            con.executescript(path.read_text("utf-8"))
    con.execute("PRAGMA user_version = 6")
    # A designer's own binding for V elsewhere is kept: only the new action is added.
    con.execute("UPDATE input_binding SET control = 'V' WHERE action = 'jump' "
                "AND device = 'keyboard'")
    con.commit()
    con.close()

    assert contract.migrate(db) == len(contract.migrations())
    con = sqlite3.connect(db)
    assert con.execute("SELECT kind FROM input_action WHERE id = 'view'").fetchone() == ("button",)
    assert sorted(con.execute("SELECT device, control FROM input_binding WHERE action = 'view'")) \
        == [("gamepad", "y"), ("keyboard", "V")]
    assert con.execute("SELECT control FROM input_binding WHERE action = 'jump' "
                       "AND device = 'keyboard'").fetchone() == ("V",)
    con.close()
