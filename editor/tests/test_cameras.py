# SPDX-License-Identifier: Apache-2.0
"""Cameras in the editor (milestone 5.6): the view action, the camera component and
midgard.camera in game scripts."""
import copy
import json
import sqlite3
import sys

import pytest
from test_live_link import FAKE_RUNTIME, echoed
from test_scripting import needs_scripting, simulate, write_script

from midgard_editor import contract, engine
from midgard_editor.cook import cook
from midgard_editor.docks.camera_editor import VIEWS, CameraDialog, CameraEditor
from midgard_editor.docks.entity_form import EntityForm
from midgard_editor.docks.viewport import ViewportPanel
from midgard_editor.live_link import PlaySession, State


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


# ---- the Camera editor ----------------------------------------------------------------------

def camera_of(project) -> dict:
    return project.entity("e000001").components["camera"]


@pytest.fixture
def editor(qtbot, project):
    widget = CameraEditor(project, "e000001")
    qtbot.addWidget(widget)
    return widget


def test_the_camera_editor_shows_each_views_settings(editor, project):
    assert editor.start.currentData() == "third"
    assert editor.amounts[("first", "fov")].value() == 70
    assert editor.amounts[("third", "fov")].value() == 55  # the camera's own field of view
    assert editor.amounts[("third", "distance")].box.text() == "Where placed"
    assert editor.amounts[("top", "min_pitch")].value() == 60  # top-down's one angle
    assert ("top", "max_pitch") not in editor.amounts and ("first", "shoulder") not in \
        editor.amounts

    # Changed elsewhere (the Inspector, or undo): the editor follows.
    entity = project.entity("e000001")
    entity.components["camera"] = {**camera_of(project), "view": "top",
                                   "views": {"first": {"height": 0.6}}}
    project.put_entity(entity)
    assert editor.start.currentData() == "top"
    assert editor.amounts[("first", "height")].value() == pytest.approx(0.6)


def test_every_camera_editor_change_is_undoable(editor, project):
    stack = project.undo_stack
    before = copy.deepcopy(camera_of(project))

    editor.amounts[("third", "shoulder")].box.setValue(0.5)
    editor.amounts[("third", "shoulder")].box.setValue(0.7)  # a drag: one step
    assert camera_of(project)["views"] == {"third": {"shoulder": 0.7}}
    assert stack.count() == 1 and "shoulder offset" in stack.undoText()

    editor.amounts[("third", "fov")].box.setValue(65)  # third person's is the camera's own
    assert camera_of(project)["fov"] == 65 and "fov" not in camera_of(project)["views"]["third"]
    editor.amounts[("third", "distance")].box.setValue(9)
    editor.amounts[("third", "distance")].box.setValue(0)  # back to where the camera is placed
    assert "distance" not in camera_of(project)["views"]["third"]
    editor.amounts[("top", "min_pitch")].box.setValue(70)
    assert camera_of(project)["views"]["top"] == {"min_pitch": 70, "max_pitch": 70}
    editor.amounts[("first", "max_pitch")].box.setValue(-85)  # below the lowest: it follows
    assert camera_of(project)["views"]["first"] == {"max_pitch": -85, "min_pitch": -85}
    editor.start.setCurrentIndex(editor.start.findData("first"))
    editor.blend.box.setValue(1.2)
    assert camera_of(project)["view"] == "first" and camera_of(project)["blend"] == 1.2

    while stack.canUndo():
        stack.undo()
    assert camera_of(project) == before
    assert editor.amounts[("third", "shoulder")].value() == 0
    while stack.canRedo():
        stack.redo()
    assert camera_of(project)["views"]["third"] == {"shoulder": 0.7}
    assert camera_of(project)["blend"] == 1.2

    editor.reset_view("third")
    assert "third" not in camera_of(project).get("views", {}) and "fov" not in camera_of(project)
    stack.undo()
    assert camera_of(project)["fov"] == 65


class RecordingView:
    """The engine's viewport, with the cameras it was given written down."""

    def __init__(self, view):
        self.view, self.cameras = view, []

    def set_camera(self, eye, target, fov=55.0):
        self.cameras.append((list(eye), list(target), fov))
        self.view.set_camera(eye, target, fov)

    def __getattr__(self, name):
        return getattr(self.view, name)


@pytest.mark.skipif(engine.module() is None, reason="engine module not built")
def test_the_preview_shows_the_games_view_and_follows_edits(qtbot, project):
    panel = ViewportPanel()
    qtbot.addWidget(panel)
    panel.view = RecordingView(engine.module().Viewport())  # (no native window offscreen)
    panel.set_project(project)
    widget = CameraEditor(project, "e000001", panel)
    qtbot.addWidget(widget)

    widget.tabs.setCurrentIndex(list(VIEWS).index("first"))
    widget.preview.setChecked(True)
    assert panel.preview_view == "first"
    eye, target, fov = panel.view.cameras[-1]
    assert eye == pytest.approx([0.0, 0.85, 0.0], abs=1e-4) and fov == 70  # the player's eyes
    assert target[1] == pytest.approx(0.85, abs=1e-4)

    widget.amounts[("first", "height")].box.setValue(0.6)  # follows the edit
    assert panel.view.cameras[-1][0][1] == pytest.approx(1.1, abs=1e-4)
    widget.tabs.setCurrentIndex(list(VIEWS).index("top"))  # follows the tab
    assert panel.preview_view == "top" and panel.view.cameras[-1][0][1] > 10

    panel.set_preview(None)  # the designer moved the view: the preview ends
    assert not widget.preview.isChecked()
    assert panel.view.cameras[-1][0] == pytest.approx(panel.camera.eye())


@pytest.mark.skipif(engine.module() is None, reason="engine module not built")
def test_the_inspector_opens_the_camera_editor(qtbot, project):
    form = EntityForm(project, "e000001")
    qtbot.addWidget(form)
    dialog = form.edit_camera()
    assert isinstance(dialog, CameraDialog) and dialog.isVisible()
    assert form.edit_camera() is dialog  # the same camera: the same window
    dialog.close()


def test_the_running_game_gets_camera_edits(project, qtbot, tmp_path, editor):
    fake = tmp_path / "fake_runtime.py"
    fake.write_text(FAKE_RUNTIME, encoding="utf-8")
    received = []
    session = PlaySession(project)
    session.message_received.connect(received.append)
    try:
        session.start([sys.executable, str(fake)], cook(project, tmp_path / "pkg").out_dir)
        qtbot.waitUntil(lambda: session.state == State.PLAYING, timeout=15000)
        editor.amounts[("third", "shoulder")].box.setValue(0.5)

        def sent():
            return [m for m in echoed(received) if m["type"] == "entity.put"
                    and m["entity"]["id"] == "e000001"]

        qtbot.waitUntil(lambda: bool(sent()), timeout=5000)
        camera = sent()[-1]["entity"]["components"]["camera"]
        assert camera["views"]["third"]["shoulder"] == 0.5
        project.undo_stack.undo()
        qtbot.waitUntil(lambda: len(sent()) == 2, timeout=5000)
        assert "views" not in sent()[-1]["entity"]["components"]["camera"]
    finally:
        session.stop()
