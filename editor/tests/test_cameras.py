# SPDX-License-Identifier: Apache-2.0
"""Cameras in the editor (milestone 5.6): the view action, the camera component and
midgard.camera in game scripts."""
import json
import sqlite3

from test_scripting import needs_scripting, simulate, write_script

from midgard_editor import contract
from midgard_editor.cook import cook


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


DIRECTOR = """
import midgard

frames = 0

def on_start(entity):
    cam = midgard.camera
    print("start", cam.active, cam.view)
    cam.set_view("top", blend=False)
    for bad in (lambda: setattr(cam, "view", "sideways"), lambda: cam.shake(-1),
                lambda: cam.look((0, 1, 0), (0, 1, 0))):
        try:
            bad()
        except ValueError:
            print("refused")

def on_update(entity, dt):
    global frames
    frames += 1
    cam = midgard.camera
    if frames == 2:
        print("top", cam.view, round(cam.position[1], 1), cam.blending)
        cam.view = "first"
        cam.shake(0.1, 0.2)
        print("blending", cam.blending)
    if frames == 40:
        print("first", cam.view, cam.blending, round(cam.fov))
        cam.look((10, 2, 0), (0, 0.5, 0), 10.0)
    if frames == 80:
        print("look", cam.scripted, [round(v, 2) for v in cam.position])
        cam.release()
    if frames == 140:
        print("back", cam.scripted, cam.view)
"""


@needs_scripting
def test_scripts_drive_the_camera(project, tmp_path):
    player = project.entity("e000004")
    player.components["script"] = {"module": "director"}
    project.put_entity(player)
    write_script(project, "director", DIRECTOR)
    _scene, run = simulate(cook(project, tmp_path / "pkg").out_dir, 150)
    assert run.returncode == 0, run.stderr
    lines = run.stdout.splitlines()
    assert "start True third" in lines
    assert lines.count("refused") == 3
    top = next(line.split() for line in lines if line.startswith("top "))
    assert top[1] == "top" and float(top[2]) > 10.0 and top[3] == "False"  # high above, at once
    assert "blending True" in lines
    assert "first first False 70" in lines
    assert "look True [10.0, 2.0, 0.0]" in lines
    assert "back False first" in lines  # released: the player's own view came back


SWITCHER = """
import midgard

def on_start(entity):
    print("starts in", midgard.camera.view)
    if midgard.camera.view == "third":
        midgard.camera.view = "top"
"""


@needs_scripting
def test_the_runtime_keeps_the_players_last_view_in_their_profile(project, tmp_path):
    player = project.entity("e000004")
    player.components["script"] = {"module": "switcher"}
    project.put_entity(player)
    write_script(project, "switcher", SWITCHER)
    package = cook(project, tmp_path / "pkg").out_dir
    profile = tmp_path / "profile"

    _scene, run = simulate(package, 5)  # no profile: nothing is kept
    assert "starts in third" in run.stdout
    _scene, run = simulate(package, 5, "--player-profile", str(profile))
    assert "starts in third" in run.stdout, run.stderr
    assert json.loads((profile / "profile.json").read_text("utf-8"))["camera"]["view"] == "top"
    _scene, run = simulate(package, 5, "--player-profile", str(profile))
    assert "starts in top" in run.stdout, run.stderr
