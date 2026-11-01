# SPDX-License-Identifier: Apache-2.0
"""The shared node-graph widget (milestone 5.11; the Animation editor of 5.13 uses it too).

A graph is nodes, each of a kind its editor describes (NodeSpec: a title and named inputs, one
output), with a place on the canvas, its inputs (another node's id, a number, or nothing) and
its own parameters, plus the node whose value is the graph's (`output`):

    {"nodes": {"hills": {"kind": "noise", "pos": [0, 0], "inputs": [], "params": {...}},
               "height": {"kind": "add", "pos": [220, 40], "inputs": [8, "hills"], ...}},
     "output": "height"}

GraphModel holds it and changes it; every change through the view is one undoable step (a
snapshot before and after, so undo can't drift). NodeGraphView draws it: drag nodes about, drag
from a node's output to another's input to link them (from a linked input to move the link),
rubber-band select, Delete to remove, wheel to zoom, middle-drag to pan, right-click to add a
node or make one the output. Unlinked inputs show their number; double-click one to type it."""
import copy
import re
from dataclasses import dataclass, field
from typing import Any

from PySide6.QtCore import QObject, QPointF, QRectF, Qt, Signal
from PySide6.QtGui import QAction, QColor, QFont, QPainter, QPainterPath, QPen, QUndoCommand
from PySide6.QtWidgets import (
    QGraphicsItem,
    QGraphicsPathItem,
    QGraphicsScene,
    QGraphicsView,
    QInputDialog,
    QMenu,
    QStyleOptionGraphicsItem,
    QWidget,
)

NODE_WIDTH = 170.0
HEADER = 24.0
ROW = 20.0
PORT_RADIUS = 5.0


@dataclass(frozen=True)
class NodeSpec:
    """A kind of node: its title, its inputs' names (a variadic one takes two or more: one more
    empty slot is always shown), and where it sits in the Add menu."""
    kind: str
    title: str
    inputs: tuple[str, ...] = ()
    variadic: bool = False
    category: str = ""
    params: dict[str, Any] = field(default_factory=dict)  # their starting values


class GraphModel(QObject):
    """A graph (see the module's docstring), and the changes made to it."""
    changed = Signal()

    def __init__(self, specs: dict[str, NodeSpec], graph: dict | None = None):
        super().__init__()
        self.specs = specs
        self.graph: dict[str, Any] = {"nodes": {}, "output": None}
        if graph:
            self.restore(graph)

    # ---- reading ---------------------------------------------------------------------------
    @property
    def nodes(self) -> dict[str, dict]:
        return self.graph["nodes"]

    @property
    def output(self) -> str | None:
        return self.graph["output"]

    def snapshot(self) -> dict:
        return copy.deepcopy(self.graph)

    def slots(self, node_id: str) -> int:
        """Input slots shown: the spec's, and for a variadic node one more than it has filled."""
        node = self.nodes[node_id]
        spec = self.specs[node["kind"]]
        if spec.variadic:
            filled = len(node["inputs"])
            while filled and node["inputs"][filled - 1] is None:
                filled -= 1
            return max(2, filled + 1)
        return len(spec.inputs)

    def slot_name(self, node_id: str, index: int) -> str:
        spec = self.specs[self.nodes[node_id]["kind"]]
        if spec.variadic:
            return f"{spec.inputs[0] if spec.inputs else 'in'} {index + 1}"
        return spec.inputs[index]

    def depends_on(self, node_id: str, other: str) -> bool:
        """Whether node_id's value uses other's (or is it)."""
        stack, seen = [node_id], set()
        while stack:
            n = stack.pop()
            if n == other:
                return True
            if n in seen or n not in self.nodes:
                continue
            seen.add(n)
            stack.extend(i for i in self.nodes[n]["inputs"] if isinstance(i, str))
        return False

    # ---- changing (each emits changed) -----------------------------------------------------
    def restore(self, graph: dict) -> None:
        self.graph = copy.deepcopy(graph)
        self.graph.setdefault("nodes", {})
        self.graph.setdefault("output", None)
        self.changed.emit()

    def new_id(self, kind: str) -> str:
        base = re.sub(r"[^a-z0-9_]", "_", kind.lower()) or "node"
        n = 1
        while f"{base}{n}" in self.nodes:
            n += 1
        return f"{base}{n}"

    def add_node(self, kind: str, pos: tuple[float, float], node_id: str | None = None,
                 params: dict | None = None) -> str:
        spec = self.specs[kind]
        node_id = node_id or self.new_id(kind)
        self.nodes[node_id] = {"kind": kind, "pos": [float(pos[0]), float(pos[1])],
                               "inputs": [None] * (0 if spec.variadic else len(spec.inputs)),
                               "params": copy.deepcopy(params if params is not None
                                                       else spec.params)}
        if self.output is None:
            self.graph["output"] = node_id
        self.changed.emit()
        return node_id

    def remove_nodes(self, node_ids) -> None:
        gone = set(node_ids)
        for node_id in gone:
            self.nodes.pop(node_id, None)
        for node in self.nodes.values():  # links from them go too
            node["inputs"] = [None if isinstance(i, str) and i in gone else i
                              for i in node["inputs"]]
        if self.output in gone:
            self.graph["output"] = None
        self.changed.emit()

    def move(self, positions: dict[str, tuple[float, float]]) -> None:
        for node_id, (x, y) in positions.items():
            if node_id in self.nodes:
                self.nodes[node_id]["pos"] = [float(x), float(y)]
        self.changed.emit()

    def set_input(self, node_id: str, index: int, value) -> bool:
        """Link (another node's id), a number, or None. False (and no change) for a link that
        would make a loop."""
        if isinstance(value, str) and (value not in self.nodes
                                       or self.depends_on(value, node_id)):
            return False
        inputs = self.nodes[node_id]["inputs"]
        while len(inputs) <= index:
            inputs.append(None)
        inputs[index] = value
        if self.specs[self.nodes[node_id]["kind"]].variadic:  # no trailing empties
            while inputs and inputs[-1] is None:
                inputs.pop()
        self.changed.emit()
        return True

    def set_params(self, node_id: str, params: dict) -> None:
        self.nodes[node_id]["params"] = copy.deepcopy(params)
        self.changed.emit()

    def set_output(self, node_id: str) -> None:
        self.graph["output"] = node_id
        self.changed.emit()


class GraphCommand(QUndoCommand):
    """One change to a graph: what it was before and after (moves of the same nodes merge)."""
    MOVE_ID = 7311

    def __init__(self, model: GraphModel, before: dict, label: str, moved: frozenset = None):
        super().__init__(label)
        self.model, self.before, self.after = model, before, model.snapshot()
        self.moved = moved
        self._first = True

    def id(self) -> int:
        return self.MOVE_ID if self.moved else -1

    def mergeWith(self, other: QUndoCommand) -> bool:  # noqa: N802 (Qt's name)
        if not isinstance(other, GraphCommand) or other.moved != self.moved:
            return False
        self.after = other.after
        return True

    def redo(self) -> None:
        if self._first:  # the change is already made
            self._first = False
            return
        self.model.restore(self.after)

    def undo(self) -> None:
        self.model.restore(self.before)


def edit(model: GraphModel, stack, label: str, change, moved: frozenset = None):
    """Make a change (a function of the model) as one undoable step."""
    before = model.snapshot()
    result = change(model)
    if model.graph != before and stack is not None:
        stack.push(GraphCommand(model, before, label, moved))
    return result


class NodeItem(QGraphicsItem):
    """A node: a titled box, its inputs down the left, its output at the top right."""

    def __init__(self, view: "NodeGraphView", node_id: str):
        super().__init__()
        self.view, self.node_id = view, node_id
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsMovable)
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsSelectable)
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemSendsGeometryChanges)
        node = view.model.nodes[node_id]
        self.setPos(QPointF(*node["pos"]))

    def height(self) -> float:
        return HEADER + ROW * max(self.view.model.slots(self.node_id), 1) + 6.0

    def boundingRect(self) -> QRectF:  # noqa: N802 (Qt's name)
        r = PORT_RADIUS + 2.0
        return QRectF(-r, -r, NODE_WIDTH + 2 * r, self.height() + 2 * r)

    def input_pos(self, index: int) -> QPointF:  # in the item's coordinates
        return QPointF(0.0, HEADER + ROW * (index + 0.5))

    def output_pos(self) -> QPointF:
        return QPointF(NODE_WIDTH, HEADER * 0.5)

    def port_at(self, scene_pos: QPointF) -> tuple[str, int] | None:
        """("out", 0) or ("in", index) under a scene point, if one is."""
        p = self.mapFromScene(scene_pos)
        reach = PORT_RADIUS + 4.0
        if (p - self.output_pos()).manhattanLength() <= reach * 1.5:
            return ("out", 0)
        for i in range(self.view.model.slots(self.node_id)):
            if (p - self.input_pos(i)).manhattanLength() <= reach * 1.5:
                return ("in", i)
        return None

    def input_at(self, scene_pos: QPointF) -> int | None:
        """The input row under a scene point (on its label or value, not only its port)."""
        p = self.mapFromScene(scene_pos)
        if 0.0 <= p.x() <= NODE_WIDTH and p.y() >= HEADER:
            i = int((p.y() - HEADER) // ROW)
            if i < self.view.model.slots(self.node_id):
                return i
        return None

    def paint(self, painter: QPainter, option: QStyleOptionGraphicsItem,
              widget: QWidget | None = None) -> None:
        model = self.view.model
        node = model.nodes.get(self.node_id)
        if node is None:
            return
        spec = model.specs[node["kind"]]
        pal = self.view.palette()
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        box = QRectF(0.0, 0.0, NODE_WIDTH, self.height())
        selected = self.isSelected()
        painter.setPen(QPen(pal.highlight().color() if selected else pal.mid().color(),
                            2.0 if selected else 1.0))
        painter.setBrush(pal.base())
        painter.drawRoundedRect(box, 5.0, 5.0)
        header = QRectF(0.0, 0.0, NODE_WIDTH, HEADER)
        tint = QColor(pal.highlight().color() if model.output == self.node_id
                      else pal.button().color())
        painter.setBrush(tint)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.drawRoundedRect(header, 5.0, 5.0)
        painter.setPen(pal.highlightedText().color() if model.output == self.node_id
                       else pal.buttonText().color())
        bold = QFont(painter.font())
        bold.setBold(True)
        painter.setFont(bold)
        painter.drawText(header.adjusted(8, 0, -14, 0), Qt.AlignmentFlag.AlignVCenter,
                         spec.title)
        painter.setFont(QFont(painter.font().family(), painter.font().pointSize()))
        # Inputs: a port each, its name, and its number when not linked.
        for i in range(model.slots(self.node_id)):
            value = node["inputs"][i] if i < len(node["inputs"]) else None
            row = QRectF(10.0, HEADER + ROW * i, NODE_WIDTH - 16.0, ROW)
            painter.setPen(pal.text().color())
            painter.drawText(row, Qt.AlignmentFlag.AlignVCenter, model.slot_name(self.node_id, i))
            if isinstance(value, (int, float)) and not isinstance(value, bool):
                painter.drawText(row, Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignRight,
                                 f"{value:g}")
            linked = isinstance(value, str)
            painter.setPen(QPen(pal.text().color(), 1.0))
            painter.setBrush(pal.highlight() if linked else pal.base())
            painter.drawEllipse(self.input_pos(i), PORT_RADIUS, PORT_RADIUS)
        painter.setBrush(pal.highlight())
        painter.drawEllipse(self.output_pos(), PORT_RADIUS, PORT_RADIUS)

    def itemChange(self, change, value):  # noqa: N802 (Qt's name)
        if change == QGraphicsItem.GraphicsItemChange.ItemPositionHasChanged:
            self.view.redraw_links()
        return super().itemChange(change, value)


class NodeGraphView(QGraphicsView):
    """Draws a GraphModel and edits it (each edit one step on `undo_stack`)."""
    selection_changed = Signal(list)  # node ids
    edit_requested = Signal(str, int)  # node id, input: a number to type (default: a dialog)

    def __init__(self, model: GraphModel, undo_stack=None, parent: QWidget | None = None):
        super().__init__(parent)
        self.model, self.undo_stack = model, undo_stack
        self.setScene(QGraphicsScene(self))
        self.scene().setSceneRect(-5000, -5000, 10000, 10000)
        self.setRenderHint(QPainter.RenderHint.Antialiasing)
        self.setDragMode(QGraphicsView.DragMode.RubberBandDrag)
        self.setTransformationAnchor(QGraphicsView.ViewportAnchor.AnchorUnderMouse)
        self.items_by_id: dict[str, NodeItem] = {}
        self.links: list[QGraphicsPathItem] = []
        self._drag: dict | None = None  # a link being dragged
        self._pressed_at: dict[str, tuple[float, float]] = {}
        self._panning: QPointF | None = None
        self.scene().selectionChanged.connect(self._selection)
        self.model.changed.connect(self.rebuild)
        self.rebuild()

    # ---- building ---------------------------------------------------------------------------
    def rebuild(self) -> None:
        selected = set(self.selected())
        self.scene().blockSignals(True)
        self.scene().clear()
        self.items_by_id.clear()
        self.links = []
        for node_id in self.model.nodes:
            item = NodeItem(self, node_id)
            self.scene().addItem(item)
            self.items_by_id[node_id] = item
            item.setSelected(node_id in selected)
        self.scene().blockSignals(False)
        self.redraw_links()
        if selected != set(self.selected()):
            self._selection()

    def redraw_links(self) -> None:
        for link in self.links:
            self.scene().removeItem(link)
        self.links = []
        pen = QPen(self.palette().text().color(), 1.6)
        for node_id, node in self.model.nodes.items():
            dst = self.items_by_id.get(node_id)
            for i, value in enumerate(node["inputs"]):
                src = self.items_by_id.get(value) if isinstance(value, str) else None
                if src is None or dst is None:
                    continue
                link = self.scene().addPath(self.curve(src.mapToScene(src.output_pos()),
                                                       dst.mapToScene(dst.input_pos(i))), pen)
                link.setZValue(-1)
                self.links.append(link)

    @staticmethod
    def curve(a: QPointF, b: QPointF) -> QPainterPath:
        path = QPainterPath(a)
        bend = max(40.0, abs(b.x() - a.x()) * 0.5)
        path.cubicTo(a + QPointF(bend, 0), b - QPointF(bend, 0), b)
        return path

    def selected(self) -> list[str]:
        return [i.node_id for i in self.scene().selectedItems() if isinstance(i, NodeItem)]

    def _selection(self) -> None:
        self.selection_changed.emit(self.selected())

    def fit(self) -> None:
        if self.items_by_id:
            self.fitInView(self.scene().itemsBoundingRect().adjusted(-40, -40, 40, 40),
                           Qt.AspectRatioMode.KeepAspectRatio)

    # ---- editing (each one undoable step) ---------------------------------------------------
    def add_node(self, kind: str, pos: QPointF) -> str:
        return edit(self.model, self.undo_stack, f"Add {self.model.specs[kind].title}",
                    lambda m: m.add_node(kind, (pos.x(), pos.y())))

    def remove_selected(self) -> None:
        ids = self.selected()
        if ids:
            edit(self.model, self.undo_stack, "Delete nodes", lambda m: m.remove_nodes(ids))

    def link(self, src: str, dst: str, index: int) -> bool:
        return edit(self.model, self.undo_stack, "Link nodes",
                    lambda m: m.set_input(dst, index, src))

    def unlink(self, dst: str, index: int) -> None:
        edit(self.model, self.undo_stack, "Unlink nodes", lambda m: m.set_input(dst, index, None))

    def set_value(self, dst: str, index: int, value: float) -> None:
        edit(self.model, self.undo_stack, "Change a value",
             lambda m: m.set_input(dst, index, float(value)))

    def make_output(self, node_id: str) -> None:
        edit(self.model, self.undo_stack, "Choose the output", lambda m: m.set_output(node_id))

    # ---- the mouse ----------------------------------------------------------------------------
    def _node_at(self, scene_pos: QPointF) -> NodeItem | None:
        for item in self.scene().items(scene_pos):
            if isinstance(item, NodeItem):
                return item
        return None

    def mousePressEvent(self, e) -> None:  # noqa: N802 (Qt's name)
        if e.button() == Qt.MouseButton.MiddleButton:
            self._panning = e.position()
            self.setCursor(Qt.CursorShape.ClosedHandCursor)
            return
        if e.button() == Qt.MouseButton.LeftButton:
            p = self.mapToScene(e.position().toPoint())
            item = self._node_at(p)
            port = item.port_at(p) if item else None
            if port and port[0] == "out":  # a new link from this output
                self._drag = {"from": item.node_id, "line": self.scene().addPath(
                    QPainterPath(), QPen(self.palette().highlight().color(), 2.0))}
                return
            if port and port[0] == "in":
                value = self.model.nodes[item.node_id]["inputs"]
                value = value[port[1]] if port[1] < len(value) else None
                if isinstance(value, str):  # pick the link up from here, to move it
                    before = self.model.snapshot()
                    self.model.set_input(item.node_id, port[1], None)
                    self._drag = {"from": value, "before": before, "line": self.scene().addPath(
                        QPainterPath(), QPen(self.palette().highlight().color(), 2.0))}
                    return
        super().mousePressEvent(e)
        self._pressed_at = {i: tuple(self.model.nodes[i]["pos"]) for i in self.selected()}

    def mouseMoveEvent(self, e) -> None:  # noqa: N802 (Qt's name)
        if self._panning is not None:
            delta = e.position() - self._panning
            self._panning = e.position()
            self.horizontalScrollBar().setValue(self.horizontalScrollBar().value() - int(delta.x()))
            self.verticalScrollBar().setValue(self.verticalScrollBar().value() - int(delta.y()))
            return
        if self._drag is not None:
            src = self.items_by_id.get(self._drag["from"])
            if src is not None:
                self._drag["line"].setPath(self.curve(src.mapToScene(src.output_pos()),
                                                      self.mapToScene(e.position().toPoint())))
            return
        super().mouseMoveEvent(e)

    def mouseReleaseEvent(self, e) -> None:  # noqa: N802 (Qt's name)
        if self._panning is not None and e.button() == Qt.MouseButton.MiddleButton:
            self._panning = None
            self.unsetCursor()
            return
        if self._drag is not None:
            drag, self._drag = self._drag, None
            self.scene().removeItem(drag["line"])
            p = self.mapToScene(e.position().toPoint())
            item = self._node_at(p)
            index = None
            if item is not None and item.node_id != drag["from"]:
                port = item.port_at(p)
                index = port[1] if port and port[0] == "in" else item.input_at(p)
            before = drag.get("before") or self.model.snapshot()
            if index is not None:
                self.model.set_input(item.node_id, index, drag["from"])  # (a loop: refused)
            if self.model.graph != before and self.undo_stack is not None:
                self.undo_stack.push(GraphCommand(self.model, before, "Link nodes"))
            return
        super().mouseReleaseEvent(e)
        moved = {i: (self.items_by_id[i].pos().x(), self.items_by_id[i].pos().y())
                 for i in self._pressed_at if i in self.items_by_id}
        if any(tuple(self.model.nodes[i]["pos"]) != pos for i, pos in moved.items()):
            edit(self.model, self.undo_stack, "Move nodes", lambda m: m.move(moved),
                 moved=frozenset(moved))
        self._pressed_at = {}

    def mouseDoubleClickEvent(self, e) -> None:  # noqa: N802 (Qt's name)
        p = self.mapToScene(e.position().toPoint())
        item = self._node_at(p)
        index = item.input_at(p) if item else None
        if item is not None and index is not None:
            value = self.model.nodes[item.node_id]["inputs"]
            value = value[index] if index < len(value) else None
            if not isinstance(value, str):
                if self.receivers(self.edit_requested) > 0:
                    self.edit_requested.emit(item.node_id, index)
                else:
                    number, ok = QInputDialog.getDouble(
                        self, "Value", self.model.slot_name(item.node_id, index),
                        float(value or 0.0), -1e9, 1e9, 4)
                    if ok:
                        self.set_value(item.node_id, index, number)
                return
        super().mouseDoubleClickEvent(e)

    def wheelEvent(self, e) -> None:  # noqa: N802 (Qt's name)
        factor = 1.15 if e.angleDelta().y() > 0 else 1 / 1.15
        scale = self.transform().m11() * factor
        if 0.2 <= scale <= 3.0:
            self.scale(factor, factor)

    def keyPressEvent(self, e) -> None:  # noqa: N802 (Qt's name)
        if e.key() in (Qt.Key.Key_Delete, Qt.Key.Key_Backspace):
            self.remove_selected()
            return
        super().keyPressEvent(e)

    def contextMenuEvent(self, e) -> None:  # noqa: N802 (Qt's name)
        p = self.mapToScene(e.pos())
        menu = self.context_menu(p)
        menu.exec(e.globalPos())

    def context_menu(self, scene_pos: QPointF) -> QMenu:
        """Add a node here; or, on a node, make it the output or delete it."""
        menu = QMenu(self)
        item = self._node_at(scene_pos)
        if item is not None:
            output = QAction("Make Output", menu)
            output.triggered.connect(lambda: self.make_output(item.node_id))
            menu.addAction(output)
            delete = QAction("Delete", menu)

            def remove(node_id=item.node_id):
                edit(self.model, self.undo_stack, "Delete nodes",
                     lambda m: m.remove_nodes([node_id]))
            delete.triggered.connect(remove)
            menu.addAction(delete)
            return menu
        add = menu.addMenu("Add Node")
        by_category: dict[str, list[NodeSpec]] = {}
        for spec in self.model.specs.values():
            by_category.setdefault(spec.category, []).append(spec)
        for category in sorted(by_category):
            target = add.addMenu(category) if category else add
            for spec in sorted(by_category[category], key=lambda s: s.title):
                action = QAction(spec.title, target)
                action.triggered.connect(lambda _=False, k=spec.kind: self.add_node(k, scene_pos))
                target.addAction(action)
        return menu
