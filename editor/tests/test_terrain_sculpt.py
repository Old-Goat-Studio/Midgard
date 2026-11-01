# SPDX-License-Identifier: Apache-2.0
"""Sculpting and painting terrain in the editor (milestone 5.9): strokes become terrain stamps
(undoable), the Terrain dock's brush, a stroke dragged in the viewport, and the running game
following."""
import json

import pytest
from test_scripting import needs_scripting, scripting_runtime

from midgard_editor import engine, terrain_stamps
from midgard_editor.cook import cook
from midgard_editor.terrain_stamps import ClearStampsCommand, StampCommand

needs_engine = pytest.mark.skipif(engine.module() is None, reason="engine module not built")
BODY = {"id": "world", "shape": "bounded_area", "width": 3000, "length": 3000, "gravity": 9.81,
        "origin": [0, 0, 0]}
MOUND = {"tool": "raise", "radius": 40.0, "strength": 30.0, "face": 0, "points": [[0.5, 0.5]]}


@pytest.fixture
def terrain_project(project):
    settings = project.scene_settings()
    settings["body"] = BODY
    settings["terrain"] = {"seed": 7}
    project.set_scene_settings(settings)
    return project


def test_strokes_are_undoable_and_keep_their_order(terrain_project):
    project = terrain_project
    seen = []
    project.terrain_changed.connect(seen.append)
    project.undo_stack.push(StampCommand(project, MOUND))
    project.undo_stack.push(StampCommand(project, {**MOUND, "tool": "lower", "strength": 5.0}))
    assert [s["tool"] for s in terrain_stamps.stamps(project)] == ["raise", "lower"]
    assert seen and project.undo_stack.undoText() == "Terrain: lower"
    project.undo_stack.undo()
    project.undo_stack.undo()
    assert terrain_stamps.stamps(project) == []
    project.undo_stack.redo()
    project.undo_stack.redo()
    assert [s["tool"] for s in terrain_stamps.stamps(project)] == ["raise", "lower"]
    project.undo_stack.push(ClearStampsCommand(project))
    assert terrain_stamps.stamps(project) == []
    project.undo_stack.undo()
    assert [s["tool"] for s in terrain_stamps.stamps(project)] == ["raise", "lower"]


def test_the_terrain_dock_gives_the_brush(qtbot, terrain_project, project):
    from midgard_editor.docks.terrain_panel import TerrainPanel

    dock = TerrainPanel()
    qtbot.addWidget(dock)
    dock.set_project(terrain_project)
    assert dock.brush() is None  # Sculpt is off
    dock.sculpt.setChecked(True)
    dock.radius.setValue(25)
    assert dock.brush() == {"tool": "raise", "radius": 25.0, "strength": 2.0}
    dock.tool.setCurrentIndex(dock.tool.findData("paint"))
    dock.paint.setCurrentIndex(dock.paint.findData("sand"))
    assert dock.brush()["paint"] == "sand" and dock.brush()["strength"] == 0.8
    terrain_project.undo_stack.push(StampCommand(terrain_project, MOUND))
    assert "1 stroke" in dock.status.text()
    dock.clear_all()
    assert terrain_stamps.stamps(terrain_project) == []
    # Without terrain there's nothing to sculpt.
    settings = terrain_project.scene_settings()
    del settings["terrain"]
    terrain_project.set_scene_settings(settings)
    assert dock.brush() is None and not dock.sculpt.isEnabled()


class FakeView:
    """The engine view's terrain calls, for a panel without a 3D view (offscreen tests)."""

    def __init__(self):
        self.sent = []

    def terrain_hit(self, origin, direction, distance):
        t = -origin[1] / direction[1]  # the plane y = 0
        x, z = origin[0] + direction[0] * t, origin[2] + direction[2] * t
        return 0, x / 3000 + 0.5, z / 3000 + 0.5, (x, 0.0, z), 0.0

    def set_terrain_stamps(self, stamps_json):
        self.sent.append(json.loads(stamps_json))


def test_a_stroke_dragged_in_the_viewport_becomes_one_stamp(qtbot, terrain_project):
    from midgard_editor.docks.viewport import OrbitCamera, ViewportPanel

    panel = ViewportPanel()
    qtbot.addWidget(panel)
    panel.project = terrain_project
    panel.view = FakeView()
    panel.camera = OrbitCamera.looking([0.0, 100.0, 100.0], [0.0, 0.0, 0.0])
    panel.brush = lambda: {"tool": "flatten", "radius": 10.0, "strength": 0.8}
    assert panel.sculpting()
    assert panel.begin_stroke(200, 150, 400, 300)
    for x in range(200, 300, 10):
        panel.extend_stroke(x, 150, 400, 300)
    stroke = panel.view.sent[-1][-1]  # the preview: the stroke drawn so far
    assert stroke["tool"] == "flatten" and stroke["height"] == 0.0 and len(stroke["points"]) > 2
    panel.end_stroke()
    saved = terrain_stamps.stamps(terrain_project)
    assert len(saved) == 1 and saved[0]["points"] == stroke["points"]
    terrain_project.undo_stack.undo()
    assert terrain_stamps.stamps(terrain_project) == []


def test_the_pointer_ray_goes_through_the_middle_of_the_view():
    from midgard_editor.docks.viewport import OrbitCamera

    camera = OrbitCamera.looking([0.0, 10.0, 10.0], [0.0, 0.0, 0.0])
    origin, direction = camera.ray(199.5, 149.5, 400, 300)
    assert origin == pytest.approx([0.0, 10.0, 10.0])
    assert direction == pytest.approx([0.0, -0.7071, -0.7071], abs=1e-3)


@needs_engine
def test_the_engine_view_finds_the_terrain_and_takes_stamps():
    view = engine.module().Viewport()
    view.set_settings(json.dumps({"body": BODY, "terrain": {"seed": 7}}))
    hit = view.terrain_hit([0.0, 300.0, 0.0], [0.0, -1.0, 0.0], 1000.0)
    assert hit is not None
    face, u, v, point, height = hit
    assert (face, u, v) == (0, pytest.approx(0.5), pytest.approx(0.5))
    view.set_terrain_stamps(json.dumps([MOUND]))
    raised = view.terrain_hit([0.0, 300.0, 0.0], [0.0, -1.0, 0.0], 1000.0)
    assert raised[4] == pytest.approx(height + 30.0, abs=0.01)
    with pytest.raises(ValueError):
        view.set_terrain_stamps(json.dumps([{"tool": "dig", "points": [[0, 0]]}]))
    assert engine.module().stamp_problems(json.dumps({**MOUND, "radius": 0}))


@needs_scripting
def test_the_running_game_follows_sculpting(terrain_project, qtbot, tmp_path):
    from midgard_editor.live_link import PlaySession, State

    received = []
    session = PlaySession(terrain_project)
    session.message_received.connect(received.append)
    try:
        session.start([str(scripting_runtime()), "--headless"],
                      cook(terrain_project, tmp_path / "pkg").out_dir)
        qtbot.waitUntil(lambda: session.state == State.PLAYING, timeout=20000)
        terrain_project.undo_stack.push(StampCommand(terrain_project, MOUND))
        qtbot.waitUntil(lambda: any(m.get("text") == "applied terrain.stamps 1"
                                    for m in received), timeout=5000)
        terrain_project.undo_stack.undo()
        qtbot.waitUntil(lambda: any(m.get("text") == "applied terrain.stamps 0"
                                    for m in received), timeout=5000)
    finally:
        session.stop()


def test_what_a_drag_does():
    from PySide6.QtCore import Qt

    from midgard_editor.docks.viewport import drag_action

    left, right, middle = (Qt.MouseButton.LeftButton, Qt.MouseButton.RightButton,
                           Qt.MouseButton.MiddleButton)
    none, shift = Qt.KeyboardModifier.NoModifier, Qt.KeyboardModifier.ShiftModifier
    assert drag_action(left, none, False) == "orbit"
    assert drag_action(right, none, False) == "orbit"
    assert drag_action(middle, none, False) == "pan"
    assert drag_action(left, shift, False) == "pan"  # Shift + left pans too
    assert drag_action(left, none, True) == "stroke"  # sculpting
    assert drag_action(left, shift, True) == "pan"    # (and pans while sculpting)
    assert drag_action(right, none, True) == "orbit"
    assert drag_action(Qt.MouseButton.NoButton, none, False) is None


@needs_engine
def test_the_view_stays_above_the_terrain(qtbot, terrain_project):
    from midgard_editor.docks.viewport import OrbitCamera, ViewportPanel

    panel = ViewportPanel()
    qtbot.addWidget(panel)
    panel.view = engine.module().Viewport()  # (no window: only its terrain is asked about)
    panel.view.set_settings(json.dumps(terrain_project.scene_settings()))
    panel.camera = OrbitCamera.looking([4.0, -40.0, 6.0], [0.0, -50.0, 0.0])  # buried
    panel.keep_above_ground()
    target_above = panel.view.terrain_ground(panel.camera.target)[0]
    eye_above = panel.view.terrain_ground(panel.camera.eye())[0]
    assert target_above >= 0.5 - 1e-6 and eye_above >= 1.0 - 1e-6
    # Without terrain nothing moves.
    panel.view.set_settings(json.dumps({"body": BODY}))
    panel.camera = OrbitCamera.looking([4.0, -40.0, 6.0], [0.0, -50.0, 0.0])
    panel.keep_above_ground()
    assert panel.camera.target == [0.0, -50.0, 0.0]
