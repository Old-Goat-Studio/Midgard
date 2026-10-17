# SPDX-License-Identifier: Apache-2.0
"""Groups in the editor (several parts moving as one), the transform maths behind them, and
deleting from the Scene panel."""
import math
import random

import pytest
from PySide6.QtCore import Qt
from PySide6.QtTest import QTest

from midgard_editor import transforms
from midgard_editor.docks.entity_form import EntityForm
from midgard_editor.docks.gizmos import GizmoPanel
from midgard_editor.docks.scene_panel import ScenePanel
from midgard_editor.scene_model import Entity, look_at_rotation


def close(a, b, tolerance=1e-3) -> bool:
    return all(abs(x - y) <= tolerance for x, y in zip(a, b, strict=True))


def world_position(project, entity_id):
    by_id = {e.id: e for e in project.entities()}
    return transforms.position_of(transforms.world_matrix(by_id, entity_id))


@pytest.fixture
def panel(qtbot, project):
    p = ScenePanel()
    qtbot.addWidget(p)
    p.set_project(project)
    return p


# ---- the maths -------------------------------------------------------------------------
def test_transforms_round_trip():
    rng = random.Random(7)
    for _ in range(200):
        t = {"position": [rng.uniform(-50, 50) for _ in range(3)],
             "rotation": [rng.uniform(-80, 80), rng.uniform(-170, 170), rng.uniform(-170, 170)],
             "scale": [rng.uniform(0.2, 3) for _ in range(3)]}
        back = transforms.decompose(transforms.compose(t))
        assert close(back["position"], t["position"])
        assert close(back["scale"], t["scale"])
        # The same turn (Euler angles can differ for the same rotation; the matrices can't).
        a = transforms.rotation(t["rotation"])
        b = transforms.rotation(back["rotation"])
        assert all(close(a[r], b[r]) for r in range(3))


def test_rotation_order_matches_the_engine():
    """look_at_rotation (R = Ry · Rx · Rz, as the engine) points -Z from eye at target."""
    eye, target = (4.0, 3.0, 6.0), (0.0, 0.5, 0.0)
    r = transforms.rotation(look_at_rotation(eye, target))
    forward = [-r[i][2] for i in range(3)]  # the rotated -Z
    d = [target[i] - eye[i] for i in range(3)]
    length = math.sqrt(sum(x * x for x in d))
    assert close(forward, [x / length for x in d])


# ---- deleting ----------------------------------------------------------------------------
def test_the_delete_key_and_right_click_delete(qtbot, project, panel):
    panel.add_primitive("prim_cube")
    cube = next(e.id for e in project.entities() if e.name == "Cube")
    panel.select(cube)
    panel.tree.setFocus()
    QTest.keyClick(panel.tree, Qt.Key.Key_Delete)
    assert project.entity(cube) is None
    project.undo_stack.undo()
    assert project.entity(cube) is not None

    # Right-click: the menu's one item is Delete, for the entity clicked.
    panel.refresh()
    item = next(i for i in panel._all_items() if i.data(0, Qt.ItemDataRole.UserRole) == cube)
    menu = panel.context_menu(item)
    assert [a.text() for a in menu.actions()] == ["Delete"]
    menu.actions()[0].trigger()
    assert project.entity(cube) is None


# ---- groups ------------------------------------------------------------------------------
def test_parts_dragged_into_a_turned_group_stay_put(project, panel):
    panel.add_group()
    group = next(e for e in project.entities() if "group" in e.components)
    assert group.name == "Group" and group.components["group"] == {}
    group.components["transform"] = {"position": [3.0, 1.0, -2.0], "rotation": [0.0, 40.0, 10.0],
                                     "scale": [2.0, 2.0, 2.0]}
    project.put_entity(group)
    cube = Entity(project.next_entity_id(), "Cube", None,
                  {"transform": {"position": [1.0, 0.5, 2.0], "rotation": [0.0, 15.0, 0.0]},
                   "mesh": {"asset": "prim_cube"}})
    project.put_entity(cube)
    before = world_position(project, cube.id)

    panel.move_entity(cube.id, group.id)  # in: its transform is now relative to the group
    moved = project.entity(cube.id)
    assert moved.parent == group.id
    assert moved.components["transform"]["scale"] == pytest.approx([0.5, 0.5, 0.5])
    assert close(world_position(project, cube.id), before)
    panel.move_entity(cube.id, None)  # and out again
    assert close(world_position(project, cube.id), before)
    assert close(project.entity(cube.id).components["transform"]["position"], before)


def test_the_player_and_cameras_stay_out_of_groups(project, panel):
    panel.add_group()
    group = next(e for e in project.entities() if "group" in e.components)
    for entity_id in ("e000004", "e000001"):  # the player, the camera
        parent = project.entity(entity_id).parent
        panel.move_entity(entity_id, group.id)
        assert project.entity(entity_id).parent == parent


def test_groups_scale_evenly_and_centre_on_their_contents(qtbot, project, panel):
    panel.add_group()
    group = next(e for e in project.entities() if "group" in e.components)
    for x in (2.0, 6.0):
        part = Entity(project.next_entity_id(), f"Part {x}", group.id,
                      {"transform": {"position": [x, 0.5, 4.0]}, "mesh": {"asset": "prim_cube"}})
        project.put_entity(part)
    parts = [e.id for e in project.entities() if e.parent == group.id]
    before = {p: world_position(project, p) for p in parts}

    form = EntityForm(project, group.id)
    qtbot.addWidget(form)
    form.scale.boxes[1].setValue(3.0)  # any one box sets all three
    assert project.entity(group.id).transform()["scale"] == [3.0, 3.0, 3.0]
    project.undo_stack.undo()

    form.centre_group()  # the centre moves to the middle of the parts; they stay put
    assert close(project.entity(group.id).transform()["position"], [4.0, 0.5, 4.0])
    for p in parts:
        assert close(world_position(project, p), before[p])
    project.undo_stack.undo()  # one step
    assert project.entity(group.id).transform()["position"] == [0.0, 0.0, 0.0]


def test_gizmos_move_and_turn_along_world_axes_inside_a_turned_group(qtbot, project):
    group = Entity(project.next_entity_id(), "Turned", None, {
        "transform": {"position": [0.0, 0.0, 0.0], "rotation": [0.0, 90.0, 0.0]}, "group": {}})
    project.put_entity(group)
    part = Entity(project.next_entity_id(), "Part", group.id,
                  {"transform": {"position": [1.0, 0.0, 0.0]}, "mesh": {"asset": "prim_cube"}})
    project.put_entity(part)
    start = world_position(project, part.id)

    panel = GizmoPanel(lambda: (0.0, 0.0), lambda: 0.01)
    qtbot.addWidget(panel)
    panel.set_entity(project, part.id)
    panel.begin("position", 0)  # world X
    panel.drag(100)  # 1 m
    panel.end()
    now = world_position(project, part.id)
    assert close(now, [start[0] + 1.0, start[1], start[2]])  # along world X, not the group's

    panel.begin("rotation", 1)  # a quarter turn about world Y
    panel.drag(180)
    panel.end()
    by_id = {e.id: e for e in project.entities()}
    r = transforms.world_matrix(by_id, part.id)
    # It began turned 90 degrees with its group (its X along world -Z); a quarter turn more
    # about world Y points its X along world -X.
    assert close([r[0][0], r[2][0]], [-1.0, 0.0])
    assert project.entity(part.id).transform()["scale"] == [1.0, 1.0, 1.0]

    panel.set_entity(project, group.id)  # a group scales evenly, whichever handle
    panel.begin("scale", 0)
    panel.drag(100)
    panel.end()
    assert project.entity(group.id).transform()["scale"] == [2.0, 2.0, 2.0]
