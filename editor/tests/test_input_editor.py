# SPDX-License-Identifier: Apache-2.0
"""The Input editor (milestone 6.1 step 1): the built-in actions named (migration 22); a game's
own actions made, named, given ids, keys, mouse and gamepad buttons and touch controls, an
axis's keys each one way; what's wrong said (no bindings, a control two actions share, a
script reading an action that isn't there); every change one undo step; and a script reading
the new action in the game when its key is held."""
import pytest
from PySide6.QtCore import Qt
from test_scripting import needs_scripting, simulate, write_script

from midgard_editor import input_actions as ia
from midgard_editor.cook import cook
from midgard_editor.docks.input_editor import InputPanel
from midgard_editor.scene_model import Entity


@pytest.fixture
def panel(project, qtbot):
    p = InputPanel()
    qtbot.addWidget(p)
    p.set_project(project)
    return p


def test_the_built_in_actions_are_named_and_listed_first(project, panel):
    actions = ia.load_all(project)
    assert [a["id"] for a in actions] == list(ia.BUILT_IN)
    names = {a["id"]: a["name"] for a in actions}
    assert names["jump"] == "Jump" and names["descend"] == "Go down (flying)"
    rebind = {a["id"]: a["rebindable"] for a in actions}
    assert rebind["jump"] and not rebind["look_x"] and not rebind["menu"]
    assert panel.list.count() == len(ia.BUILT_IN)
    assert not panel.delete_button.isEnabled()  # (built in: kept)
    assert panel.id_edit.isReadOnly()
    assert ia.problems(project) == []  # (the defaults are sound)


def test_a_games_own_action_made_named_and_bound(project, panel):
    stack = project.undo_stack
    action = panel.new_action("Honk the horn")
    assert action == "honk_the_horn"
    assert panel.list.currentItem().text().startswith("Honk the horn")
    a = ia.load(project, action)
    assert a["kind"] == "button" and a["rebindable"] and a["bindings"] == []
    assert any("no keys" in p for p in ia.problems(project))
    # Its id, as scripts read it.
    assert panel.set_id("honk")
    assert ia.load(project, "honk") is not None and ia.load(project, action) is None
    assert not panel.set_id("Bad Id!") and not panel.set_id("jump")
    assert "can't be its id" in panel.problems.text()
    # Renamed: typing is one step.
    panel.name.setText("Honk")
    panel.name.textEdited.emit("Honk")
    panel.name.setText("Honk!")
    panel.name.textEdited.emit("Honk!")
    assert ia.load(project, "honk")["name"] == "Honk!"
    count = stack.count()
    # A key: pressed in the dialog (Escape is a key too there).
    dialog = panel.key_dialog()
    assert dialog.take(Qt.Key.Key_H.value) and dialog.key == "H"
    panel.add_binding("keyboard", dialog.key)
    panel.add_binding("gamepad", "leftshoulder")
    panel.add_binding("mouse", "middle")
    panel.add_binding("gamepad", "leftshoulder")  # (had it: not twice)
    a = ia.load(project, "honk")
    assert [(b["device"], b["control"]) for b in a["bindings"]] == [
        ("keyboard", "H"), ("gamepad", "leftshoulder"), ("mouse", "middle")]
    assert stack.count() == count + 3
    assert panel.bindings.rowCount() == 3
    assert panel.bindings.item(1, 0).text() == "Gamepad: Left bumper"
    # A clash: Space is Jump's too (both in the same context).
    panel.add_binding("keyboard", "Space")
    assert "'Honk!' and 'Jump' both use Space" in panel.problems.text()
    assert sum("both use Space" in p for p in ia.problems(project)) == 1  # (said once)
    panel.remove_binding(3)
    assert ia.problems(project) == []
    # Undo all the way back, redo it all.
    while stack.canUndo():
        stack.undo()
    assert ia.load(project, "honk") is None and ia.load(project, action) is None
    while stack.canRedo():
        stack.redo()
    assert [b["control"] for b in ia.load(project, "honk")["bindings"]] == [
        "H", "leftshoulder", "middle"]
    # Deleted (one step).
    panel.choose("honk")
    assert panel.delete_action()
    assert ia.load(project, "honk") is None
    stack.undo()
    assert ia.load(project, "honk")["name"] == "Honk!"


def test_an_axis_keys_push_each_way(project, panel):
    panel.new_action("Lean")
    panel.kind.setCurrentIndex(panel.kind.findData("axis"))
    panel.kind.activated.emit(panel.kind.currentIndex())
    panel.add_binding("keyboard", "Q")
    panel.add_binding("keyboard", "R")
    panel.add_binding("mouse", "delta_x")
    way = panel.bindings.cellWidget(0, 1)
    way.setCurrentIndex(1)  # Q: the other way
    way.activated.emit(1)
    a = ia.load(project, "lean")
    assert [b["scale"] for b in a["bindings"]] == [-1.0, 1.0, 0.15]
    assert ia.binding_label(a["bindings"][0], "axis") == "Q (−)"
    assert "midgard.action" in panel.id_label.text()  # (an axis is read as a number)
    # Back to a button: its bindings push one way.
    panel.kind.setCurrentIndex(panel.kind.findData("button"))
    panel.kind.activated.emit(panel.kind.currentIndex())
    assert [b["scale"] for b in ia.load(project, "lean")["bindings"]] == [1.0, 1.0, 0.15]


def test_keys_are_named_as_the_game_names_them():
    k = Qt.Key
    assert ia.key_name(k.Key_W.value) == "W"
    assert ia.key_name(k.Key_5.value) == "5"
    assert ia.key_name(k.Key_F3.value) == "F3"
    assert ia.key_name(k.Key_Space.value) == "Space"
    assert ia.key_name(k.Key_Shift.value, 42) == "Left Shift"
    assert ia.key_name(k.Key_Shift.value, 54) == "Right Shift"
    assert ia.key_name(k.Key_Control.value) == "Left Ctrl"
    assert ia.key_name(k.Key_Escape.value) == "Escape"
    assert ia.key_name(k.Key_Comma.value) == ","
    assert ia.key_name(k.Key_MediaPlay.value) is None


HONK_SCRIPT = """
import midgard

seen = {"held": 0, "pressed": 0}


def on_update(entity, dt):
    if midgard.pressed("honk"):
        seen["pressed"] += 1
        print("honk pressed")
    if midgard.down("honk"):
        seen["held"] += 1
    if seen["held"] == 5:
        print("held for 5 frames")
        seen["held"] += 1
"""


@needs_scripting
def test_a_script_reads_the_games_own_action(project, panel, tmp_path):
    panel.new_action("Honk")
    panel.add_binding("keyboard", "H")
    project.put_entity(Entity("e000093", "Horn", None, {
        "transform": {}, "script": {"module": "horn"}}))
    write_script(project, "horn", HONK_SCRIPT)
    result = cook(project, tmp_path / "pkg")
    assert not [w for w in result.warnings if "Input" in w or "action" in w]
    _dump, run = simulate(result.out_dir, 30, "--hold", "H:10:20")
    assert run.returncode == 0, run.stderr
    lines = run.stdout.splitlines()
    assert lines.count("honk pressed") == 1 and "held for 5 frames" in lines
    # A script reading an action the project doesn't have: said.
    write_script(project, "horn", HONK_SCRIPT.replace('"honk"', '"hoot"'))
    warnings = cook(project, tmp_path / "pkg2").warnings
    assert any("reads action 'hoot'" in w for w in warnings)
