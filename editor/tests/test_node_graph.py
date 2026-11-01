# SPDX-License-Identifier: Apache-2.0
"""The shared node-graph widget (milestone 5.11): the model's changes and their rules (no loops,
links go with their nodes, variadic inputs), every change one undoable step, and the view driven
by the mouse (link, move, pick a link up, delete, the menu)."""
from PySide6.QtCore import QPoint, QPointF, Qt
from PySide6.QtGui import QUndoStack
from PySide6.QtTest import QTest

from midgard_editor.node_graph import (
    HEADER,
    ROW,
    GraphModel,
    NodeGraphView,
    NodeSpec,
    edit,
)

SPECS = {
    "number": NodeSpec("number", "Number", (), params={"value": 1}),
    "add": NodeSpec("add", "Add", ("in",), variadic=True, category="Maths"),
    "lerp": NodeSpec("lerp", "Blend", ("a", "b", "t"), category="Maths"),
}


def test_the_model_links_refuses_loops_and_drops_links_with_their_nodes():
    m = GraphModel(SPECS)
    a = m.add_node("number", (0, 0))
    b = m.add_node("add", (200, 0))
    c = m.add_node("lerp", (400, 0))
    assert m.output == a  # the first node, until another is chosen
    assert m.set_input(b, 0, a) and m.set_input(b, 1, 4.0)
    assert m.set_input(c, 0, b)
    assert not m.set_input(b, 2, c)  # c uses b: a loop
    assert not m.set_input(b, 2, b)  # itself
    assert not m.set_input(b, 2, "nope")
    # A variadic node shows one more slot than it has filled.
    assert m.slots(b) == 3 and m.slot_name(b, 2) == "in 3"
    assert m.slots(c) == 3
    m.set_output(c)
    m.remove_nodes([b])
    assert m.nodes[c]["inputs"][0] is None  # the link went with it
    assert m.output == c
    m.remove_nodes([c])
    assert m.output is None


def test_every_change_is_one_undoable_step_and_moves_merge():
    m = GraphModel(SPECS)
    stack = QUndoStack()
    a = edit(m, stack, "Add", lambda g: g.add_node("number", (0, 0)))
    b = edit(m, stack, "Add", lambda g: g.add_node("add", (200, 0)))
    edit(m, stack, "Link", lambda g: g.set_input(b, 0, a))
    for x in (10, 20, 30):  # one drag
        edit(m, stack, "Move", lambda g, x=x: g.move({a: (x, 5)}), moved=frozenset({a}))
    assert stack.count() == 4
    assert m.nodes[a]["pos"] == [30, 5]
    stack.undo()
    assert m.nodes[a]["pos"] == [0, 0]  # the whole drag
    stack.undo()
    assert m.nodes[b]["inputs"] == [] or m.nodes[b]["inputs"][0] is None
    stack.redo()
    assert m.nodes[b]["inputs"][0] == a
    stack.undo()
    stack.undo()
    stack.undo()
    assert m.nodes == {}
    edit(m, stack, "Nothing", lambda g: None)  # no change: no step
    assert stack.index() == 0


def view_with_two_nodes(qtbot):
    m = GraphModel(SPECS)
    stack = QUndoStack()
    view = NodeGraphView(m, stack)
    qtbot.addWidget(view)
    view.resize(900, 500)
    view.show()
    a = m.add_node("number", (0, 0))
    b = m.add_node("lerp", (300, 0))
    view.centerOn(QPointF(235, 40))
    return m, stack, view, a, b


def at(view, scene_point: QPointF) -> QPoint:
    return view.mapFromScene(scene_point)


def test_dragging_from_an_output_to_an_input_links_them(qtbot):
    m, stack, view, a, b = view_with_two_nodes(qtbot)
    src = view.items_by_id[a].mapToScene(view.items_by_id[a].output_pos())
    dst = view.items_by_id[b].mapToScene(view.items_by_id[b].input_pos(1))
    port = view.viewport()
    QTest.mousePress(port, Qt.MouseButton.LeftButton, pos=at(view, src))
    QTest.mouseMove(port, at(view, (src + dst) / 2))
    QTest.mouseMove(port, at(view, dst))
    QTest.mouseRelease(port, Qt.MouseButton.LeftButton, pos=at(view, dst))
    assert m.nodes[b]["inputs"][1] == a
    assert len(view.links) == 1
    assert stack.count() == 1
    stack.undo()
    assert m.nodes[b]["inputs"][1] is None
    stack.redo()
    # Picking the link up at its input and dropping it on another input moves it.
    dst2 = view.items_by_id[b].mapToScene(view.items_by_id[b].input_pos(2))
    dst = view.items_by_id[b].mapToScene(view.items_by_id[b].input_pos(1))
    QTest.mousePress(port, Qt.MouseButton.LeftButton, pos=at(view, dst))
    QTest.mouseMove(port, at(view, dst2))
    QTest.mouseRelease(port, Qt.MouseButton.LeftButton, pos=at(view, dst2))
    assert m.nodes[b]["inputs"][1] is None and m.nodes[b]["inputs"][2] == a
    assert stack.count() == 2  # one step
    stack.undo()
    assert m.nodes[b]["inputs"][1] == a


def test_dragging_a_node_moves_it_in_one_step_and_delete_removes_it(qtbot):
    m, stack, view, a, b = view_with_two_nodes(qtbot)
    title = view.items_by_id[b].mapToScene(QPointF(60, HEADER * 0.5))
    port = view.viewport()
    QTest.mousePress(port, Qt.MouseButton.LeftButton, pos=at(view, title))
    for dx in (10, 25, 40):
        QTest.mouseMove(port, at(view, title + QPointF(dx, 15)))
    QTest.mouseRelease(port, Qt.MouseButton.LeftButton, pos=at(view, title + QPointF(40, 15)))
    x, y = m.nodes[b]["pos"]
    assert abs(x - 340) < 3 and abs(y - 15) < 3
    assert stack.count() == 1
    assert view.selected() == [b]
    QTest.keyClick(view, Qt.Key.Key_Delete)
    assert b not in m.nodes
    stack.undo()
    assert b in m.nodes


def test_the_menu_adds_nodes_and_makes_one_the_output(qtbot):
    m, stack, view, a, b = view_with_two_nodes(qtbot)
    menu = view.context_menu(QPointF(0, 300))
    add = menu.actions()[0].menu()
    maths = next(x.menu() for x in add.actions() if x.text() == "Maths")
    next(x for x in maths.actions() if x.text() == "Blend").trigger()
    added = [n for n in m.nodes if n not in (a, b)]
    assert len(added) == 1 and m.nodes[added[0]]["pos"] == [0.0, 300.0]
    on_node = view.context_menu(view.items_by_id[b].mapToScene(QPointF(50, HEADER + ROW)))
    next(x for x in on_node.actions() if x.text() == "Make Output").trigger()
    assert m.output == b
    stack.undo()
    assert m.output == a
