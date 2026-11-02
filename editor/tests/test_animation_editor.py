# SPDX-License-Identifier: Apache-2.0
"""The Animation editor (milestone 5.13 step 5): graph edits (states, transitions, layers,
parameters, renames carried everywhere); the dock making a character's graph with the mouse and
its settings, every change undoable and every graph one the engine reads; the preview playing
it, its parameters set by hand."""
import json

import pytest
from PySide6.QtCore import QPoint, QPointF, Qt
from test_characters import HERO, LIBRARY, with_samples

from midgard_editor import anim_graphs as ag
from midgard_editor import engine
from midgard_editor.scene_model import Entity

needs_engine = pytest.mark.skipif(engine.module() is None, reason="engine module not built")


def engine_reads(graph: dict) -> str:
    return engine.module().anim_graph_problem(json.dumps(graph))


def test_graph_edits_carry_names_everywhere():
    g = ag.new_graph(["Idle_Loop", "Walk_Loop"])
    assert g["layers"][0]["states"]["Idle"]["clip"] == "Idle_Loop"
    g, walk = ag.add_state(g, 0, (200, 0), "Walk_Loop", name="Walk")
    g, i = ag.add_transition(g, 0, "Idle", walk)
    g, j = ag.add_transition(g, 0, ag.ANY, "Idle")
    back, k = ag.add_transition(g, 0, walk, "Idle")  # (back: the opposite, not to and fro)
    assert back["layers"][0]["transitions"][k]["when"] == [["speed", "<=", 0.1]]
    with_trigger, _ = ag.add_parameter(g, trigger=True)
    jumping, k = ag.add_transition(with_trigger, 0, ag.ANY, walk)
    assert jumping["layers"][0]["transitions"][k]["when"] == [["trigger"]]
    g, wave = ag.add_parameter(g)
    g = ag.set_layer(g, 0, weight=wave)
    t = g["layers"][0]["transitions"][i]
    t["when"] = [[wave, ">", 0.5]]
    g = ag.set_transition(g, 0, i, t)
    # A state renamed: its transitions and the start follow.
    g2 = ag.rename_state(g, 0, "Idle", "Rest")
    lay = g2["layers"][0]
    assert lay["start"] == "Rest" and list(lay["states"]) == ["Rest", "Walk"]
    assert lay["transitions"][i]["from"] == "Rest" and lay["transitions"][j]["to"] == "Rest"
    assert ag.rename_state(g, 0, "Idle", "Walk") == g  # (taken)
    assert g["layers"][0]["start"] == "Idle"  # (the graph given is left alone)
    # A parameter renamed: everything using it follows; it can't go while used.
    g3 = ag.rename_parameter(g, wave, "waving")
    assert g3["layers"][0]["weight"] == "waving"
    assert g3["layers"][0]["transitions"][i]["when"] == [["waving", ">", 0.5]]
    assert ag.uses(g3, "waving") == 2
    assert ag.remove_parameter(g3, "waving") == g3
    assert ag.rename_parameter(g3, "speed", "pace") == g3  # (automatic: kept)
    # A state removed: its transitions too; the start moves on.
    g4 = ag.remove_states(g, 0, ["Idle"])
    assert list(g4["layers"][0]["states"]) == ["Walk"] and g4["layers"][0]["start"] == "Walk"
    assert g4["layers"][0]["transitions"] == []
    assert list(ag.remove_states(g4, 0, ["Walk"])["layers"][0]["states"]) == ["Walk"]
    g5, layer = ag.add_layer(g, ["Wave"])
    assert layer == 1 and ag.remove_layer(g5, 1)["layers"] == g["layers"]
    if engine.module() is not None:
        g["layers"][0]["weight"] = 1.0
        assert engine_reads(g) == ""
        assert engine_reads(g5) == ""


def make_character(project, graph=None):
    with_samples(project)
    character = {"model": HERO, "clips": LIBRARY, "clip": "Idle_Loop"}
    if graph:
        character["graph"] = graph
    entity = Entity(project.next_entity_id(), "Hero", None,
                    {"transform": {}, "character": character})
    project.put_entity(entity)
    return entity.id


def graph_of(project, entity_id):
    return project.entity(entity_id).components["character"].get("graph")


def drag(qtbot, view, a: QPointF, b: QPointF, shift=False) -> None:
    """A real left-button drag on the canvas, between scene points."""
    modifiers = Qt.KeyboardModifier.ShiftModifier if shift else Qt.KeyboardModifier.NoModifier
    pa, pb = view.mapFromScene(a), view.mapFromScene(b)
    qtbot.mousePress(view.viewport(), Qt.MouseButton.LeftButton, modifiers, pa)
    mid = QPoint((pa.x() + pb.x()) // 2, (pa.y() + pb.y()) // 2)
    qtbot.mouseMove(view.viewport(), mid)
    qtbot.mouseMove(view.viewport(), pb)
    qtbot.mouseRelease(view.viewport(), Qt.MouseButton.LeftButton, modifiers, pb)


@needs_engine
def test_the_dock_makes_a_graph_with_the_mouse(project, qtbot):
    from midgard_editor.docks.animation_editor import AnimationPanel
    entity_id = make_character(project)
    panel = AnimationPanel()
    qtbot.addWidget(panel)
    panel.resize(1200, 700)
    panel.show()
    panel.set_project(project)
    panel.show_entity(entity_id)
    assert panel.create_button.isVisible() and panel.graph() is None
    panel.create_graph()
    assert graph_of(project, entity_id)["layers"][0]["start"] == "Idle"
    # A blend state, added as the canvas's menu does; then a transition by Shift-drag.
    walk = panel.add_state((300.0, 0.0), blend=True)
    view = panel.canvas
    idle_item, walk_item = view.states["Idle"], view.states[walk]
    drag(qtbot, view, idle_item.centre(), walk_item.centre(), shift=True)
    lay = graph_of(project, entity_id)["layers"][0]
    assert lay["transitions"] == [{"from": "Idle", "to": walk,
                                   "when": [["speed", ">", 0.1]], "blend": 0.2}]
    assert panel.selection == ("transition", 0)
    # A state dragged: where it sits is kept (one step).
    count = project.undo_stack.count()
    before = view.states["Idle"].pos()
    drag(qtbot, view, view.states["Idle"].centre(), view.states["Idle"].centre() + QPointF(0, 90))
    assert project.undo_stack.count() == count + 1
    pos = graph_of(project, entity_id)["layers"][0]["states"]["Idle"]["pos"]
    assert pos[1] == pytest.approx(before.y() + 90, abs=2)
    # The blend's clips, as its settings set them.
    panel._blend_point(walk, 0, clip="Idle_Loop", at=0.0)
    panel._blend_point(walk, -1, clip="Walk_Loop")
    panel._blend_point(walk, 1, at=1.4)
    state = graph_of(project, entity_id)["layers"][0]["states"][walk]
    assert state["blend"] == "speed" and state["clips"] == [["Idle_Loop", 0.0],
                                                             ["Walk_Loop", 1.4]]
    # A trigger, a condition on it, and a layer for the upper body.
    trigger = panel.add_parameter(trigger=True)
    panel._condition(0, -1)
    panel._condition(0, 1, name=trigger)
    assert graph_of(project, entity_id)["layers"][0]["transitions"][0]["when"][1] == [trigger]
    panel.add_layer()
    assert panel.layer == 1
    panel._set_layer(mask="spine_02")
    graph = graph_of(project, entity_id)
    assert graph["layers"][1]["mask"] == "spine_02"
    assert engine_reads(graph) == "" and panel.problem.text() == ""
    # Delete removes the selected state, its transition with it.
    panel.layer_combo.setCurrentIndex(0)
    view.states[walk].setSelected(True)
    view.setFocus()
    qtbot.keyClick(view, Qt.Key.Key_Delete)
    lay = graph_of(project, entity_id)["layers"][0]
    assert walk not in lay["states"] and lay["transitions"] == []
    # All of it undoes, step by step, to no graph.
    while project.undo_stack.canUndo() and graph_of(project, entity_id) is not None:
        project.undo_stack.undo()
    assert graph_of(project, entity_id) is None
    assert panel.create_button.isVisible()


@needs_engine
def test_a_graph_the_engine_refuses_says_why(project, qtbot):
    from midgard_editor.docks.animation_editor import AnimationPanel
    g = ag.new_graph(["Idle_Loop"])
    g["layers"][0]["states"]["Idle"]["clip"] = "Moonwalk"
    entity_id = make_character(project, g)
    panel = AnimationPanel()
    qtbot.addWidget(panel)
    panel.set_project(project)
    panel.show_entity(entity_id)
    assert "Moonwalk" in panel.problem.text()
    panel.commit(ag.set_state(panel.graph(), 0, "Idle", {"blend": "speed", "clips": []}), "")
    assert "a blend needs 'clips'" in panel.problem.text()


@needs_engine
def test_the_preview_plays_the_graph(project, qtbot):
    from midgard_editor.docks.animation_editor import AnimationPanel
    g = ag.new_graph(["Idle_Loop"])
    g, walk = ag.add_state(g, 0, (200, 0), "Walk_Loop", name="Walk")
    g, _ = ag.add_transition(g, 0, "Idle", walk)  # (once speed > 0.1)
    entity_id = make_character(project, g)
    panel = AnimationPanel()
    qtbot.addWidget(panel)
    panel.set_project(project)
    panel.show_entity(entity_id)
    if panel._view is None:
        pytest.skip(f"no Vulkan here: {panel.preview_image.text()}")
    assert panel.preview_image.pixmap() is not None
    assert "Base: Idle" in panel.preview_state.text()
    first = panel.preview_image.pixmap().toImage()
    panel.set_preview_value("speed", 1.4)
    for _ in range(10):
        panel.tick(0.05)
    assert "Base: Walk" in panel.preview_state.text()
    assert panel.preview_image.pixmap().toImage() != first  # (it moved)
    panel.set_preview_value("speed", 0.0)  # (no way back from Walk: only a restart)
    assert "Base: Walk" in panel.preview_state.text()
    panel.restart_preview()
    assert "Base: Idle" in panel.preview_state.text()
    panel.play_button.setChecked(True)
    assert panel.timer.isActive()
    panel.set_project(None)  # (the preview goes with the project)
    assert panel._view is None and not panel.timer.isActive()
