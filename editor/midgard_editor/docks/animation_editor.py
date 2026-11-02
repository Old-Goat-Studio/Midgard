# SPDX-License-Identifier: Apache-2.0
"""The Animation editor (milestone 5.13): a character's animation graph, edited and previewed.

The graph (midgard/anim_graph.h; anim_graphs.py) belongs to the character selected in the
Scene: its layers one at a time, each drawn as states (boxes) and transitions (arrows) on a
canvas. Drag states about; Shift-drag from one state to another (or from Any State) for a
transition; right-click to add a state, make one the start or delete. The selected state,
transition or layer has its settings beside the canvas, and the graph's parameters and triggers
below them. Every change is one undoable step on the entity. The preview plays the graph through
the engine, with the parameters set by hand and the triggers fired by buttons."""
import contextlib
import copy
import json
import logging
import math

from PySide6.QtCore import QPointF, QRectF, Qt, QTimer, Signal
from PySide6.QtGui import QBrush, QColor, QFont, QImage, QPainter, QPainterPath, QPen, QPixmap
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFormLayout,
    QGraphicsItem,
    QGraphicsPathItem,
    QGraphicsScene,
    QGraphicsView,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMenu,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QSplitter,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

from .. import anim_graphs as ag
from .. import engine
from ..cook import mesh_asset_data
from ..project import Project
from ..scene_model import Entity
from ..undo import SetEntityCommand

log = logging.getLogger(__name__)

STATE_W, STATE_H = 150.0, 46.0
PREVIEW_SIZE = (300, 300)
PREVIEW_ID = "preview"


def _spin(low: float, high: float, value: float, step: float = 0.1,
          decimals: int = 3) -> QDoubleSpinBox:
    box = QDoubleSpinBox()
    box.setRange(low, high)
    box.setDecimals(decimals)
    box.setSingleStep(step)
    box.setValue(float(value))
    box.setKeyboardTracking(False)
    box.setMaximumWidth(96)
    return box


def _combo(items, current, *, data=None) -> QComboBox:
    combo = QComboBox()
    combo.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Fixed)  # (shrinks)
    combo.setMinimumWidth(60)
    for i, text in enumerate(items):
        combo.addItem(text, data[i] if data else text)
    index = combo.findData(current)
    if index < 0 and current not in (None, ""):
        combo.addItem(f"{current} (missing)", current)
        index = combo.count() - 1
    combo.setCurrentIndex(max(0, index))
    return combo


def _row(*widgets) -> QWidget:
    row = QWidget()
    layout = QHBoxLayout(row)
    layout.setContentsMargins(0, 0, 0, 0)
    for w in widgets:
        layout.addWidget(w)
    return row


def _remove(slot) -> QToolButton:
    button = QToolButton()
    button.setText("✕")
    button.setToolTip("Remove")
    button.clicked.connect(lambda _=False: slot())
    return button


# ---- the canvas ---------------------------------------------------------------------------------
class StateItem(QGraphicsItem):
    """A state (or the layer's Any State) as a box."""

    def __init__(self, view: "StateMachineView", name: str, subtitle: str, start: bool):
        super().__init__()
        self.view, self.name, self.subtitle, self.start = view, name, subtitle, start
        self.setFlags(QGraphicsItem.GraphicsItemFlag.ItemIsMovable |
                      QGraphicsItem.GraphicsItemFlag.ItemIsSelectable |
                      QGraphicsItem.GraphicsItemFlag.ItemSendsGeometryChanges)
        self.setZValue(1)

    @property
    def is_any(self) -> bool:
        return self.name == ag.ANY

    def boundingRect(self) -> QRectF:  # noqa: N802 (Qt's name)
        return QRectF(-2, -2, STATE_W + 4, STATE_H + 4)

    def centre(self) -> QPointF:
        return self.pos() + QPointF(STATE_W / 2, STATE_H / 2)

    def paint(self, painter: QPainter, option, widget=None) -> None:
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        fill = QColor("#3a3f4b") if not self.is_any else QColor("#4b3a4a")
        border = QColor("#6cc070") if self.start else QColor("#202020")
        if self.isSelected():
            border = QColor("#f0c040")
        painter.setPen(QPen(border, 3 if (self.start or self.isSelected()) else 1))
        painter.setBrush(QBrush(fill))
        painter.drawRoundedRect(QRectF(0, 0, STATE_W, STATE_H), 8, 8)
        painter.setPen(QColor("#ffffff"))
        title = QFont()
        title.setBold(True)
        painter.setFont(title)
        painter.drawText(QRectF(8, 4, STATE_W - 16, 20), Qt.AlignmentFlag.AlignLeft,
                         "Any State" if self.is_any else self.name)
        painter.setFont(QFont())
        painter.setPen(QColor("#b8c0cc"))
        subtitle = painter.fontMetrics().elidedText(self.subtitle, Qt.TextElideMode.ElideRight,
                                                    int(STATE_W - 16))
        painter.drawText(QRectF(8, 24, STATE_W - 16, 18), Qt.AlignmentFlag.AlignLeft, subtitle)

    def itemChange(self, change, value):  # noqa: N802 (Qt's name)
        if change == QGraphicsItem.GraphicsItemChange.ItemPositionHasChanged:
            self.view.redraw_links()
        return super().itemChange(change, value)


class TransitionItem(QGraphicsPathItem):
    """A transition: an arrow from one state to another."""

    def __init__(self, index: int, src: str, dst: str):
        super().__init__()
        self.index, self.src, self.dst = index, src, dst
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsSelectable)
        self.setZValue(0)

    def shape(self) -> QPainterPath:
        from PySide6.QtGui import QPainterPathStroker
        stroker = QPainterPathStroker()
        stroker.setWidth(10)
        return stroker.createStroke(self.path())

    def paint(self, painter: QPainter, option, widget=None) -> None:
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        colour = QColor("#f0c040") if self.isSelected() else QColor("#9aa4b4")
        painter.setPen(QPen(colour, 3 if self.isSelected() else 2))
        painter.setBrush(QBrush(colour))
        painter.drawPath(self.path())


class StateMachineView(QGraphicsView):
    """One layer's states and transitions. It changes nothing itself: it asks (its signals) and
    the panel makes the change, and shows it again."""
    selection_changed = Signal(str, object)  # "state" and its name, "transition" and its index,
    # or "" and None
    moved = Signal(dict)  # {state name (or "*"): (x, y)}
    connect_requested = Signal(str, str)  # from, to
    add_requested = Signal(float, float, bool)  # x, y, a blend state
    delete_requested = Signal(list, list)  # states, transition indices
    start_requested = Signal(str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setScene(QGraphicsScene(self))
        self.setRenderHint(QPainter.RenderHint.Antialiasing)
        self.setDragMode(QGraphicsView.DragMode.RubberBandDrag)
        self.setBackgroundBrush(QColor("#25282e"))
        self.states: dict[str, StateItem] = {}
        self.links: list[TransitionItem] = []
        self._linking: StateItem | None = None
        self._rubber_line: QGraphicsPathItem | None = None
        self._pressed_at: dict[str, QPointF] = {}
        self._quiet = False
        self.scene().selectionChanged.connect(self._selected)

    # ---- showing a layer --------------------------------------------------------------------
    def show_layer(self, layer: dict, select: tuple[str, object] = ("", None)) -> None:
        self._quiet = True
        self.scene().clear()
        self.states, self.links = {}, []
        any_pos = layer.get("any_pos", [-220, -120])
        self._add_state(ag.ANY, "transitions from any state", False, any_pos)
        for name, state in layer.get("states", {}).items():
            if state.get("blend") is not None:
                subtitle = f"blend by {state.get('blend')}"
            else:
                subtitle = state.get("clip") or "(no clip)"
            self._add_state(name, subtitle, name == layer.get("start"), state.get("pos", [0, 0]))
        for i, t in enumerate(layer.get("transitions", [])):
            src = t.get("from") or ag.ANY
            if src in self.states and t.get("to") in self.states:
                item = TransitionItem(i, src, t["to"])
                self.scene().addItem(item)
                self.links.append(item)
        self.redraw_links()
        kind, key = select
        if kind == "state" and key in self.states:
            self.states[key].setSelected(True)
        elif kind == "transition":
            for link in self.links:
                if link.index == key:
                    link.setSelected(True)
        self._quiet = False

    def _add_state(self, name: str, subtitle: str, start: bool, pos) -> None:
        item = StateItem(self, name, subtitle, start)
        item.setPos(float(pos[0]), float(pos[1]))
        self.scene().addItem(item)
        self.states[name] = item

    def redraw_links(self) -> None:
        pairs = {(link.src, link.dst) for link in self.links}
        for link in self.links:
            a, b = self.states[link.src].centre(), self.states[link.dst].centre()
            if link.src == link.dst:  # a state to itself: a loop over it
                path = QPainterPath(a + QPointF(-20, -STATE_H / 2))
                path.cubicTo(a + QPointF(-30, -90), a + QPointF(30, -90),
                             a + QPointF(20, -STATE_H / 2))
                link.setPath(path)
                continue
            d = b - a
            length = math.hypot(d.x(), d.y()) or 1.0
            ux, uy = d.x() / length, d.y() / length
            nx, ny = -uy, ux
            # Both ways between two states: side by side.
            offset = 8.0 if (link.dst, link.src) in pairs else 0.0
            a = a + QPointF(nx * offset, ny * offset)
            b = b + QPointF(nx * offset, ny * offset)
            # From and to the boxes' edges.
            ta = min(abs(STATE_W / 2 / ux) if ux else 1e9, abs(STATE_H / 2 / uy) if uy else 1e9)
            start = a + QPointF(ux * ta, uy * ta)
            end = b - QPointF(ux * ta, uy * ta)
            path = QPainterPath(start)
            path.lineTo(end)
            # The arrowhead, at the middle (so arrows both ways stay apart).
            mid = (start + end) / 2
            tip = mid + QPointF(ux * 7, uy * 7)
            path.moveTo(tip)
            path.lineTo(mid + QPointF(-ux * 7 + nx * 6, -uy * 7 + ny * 6))
            path.lineTo(mid + QPointF(-ux * 7 - nx * 6, -uy * 7 - ny * 6))
            path.closeSubpath()
            link.setPath(path)

    def fit(self) -> None:
        """Room round the states, and them in the middle (smaller if they don't fit; never
        larger)."""
        items = self.scene().itemsBoundingRect()
        self.setSceneRect(items.adjusted(-400, -300, 400, 300))
        view = self.viewport().size()
        scale = min(1.0, view.width() / (items.width() + 60), view.height() / (items.height() + 60))
        self.resetTransform()
        self.scale(scale, scale)
        self.centerOn(items.center())

    # ---- selection --------------------------------------------------------------------------
    def selected(self) -> tuple[list[str], list[int]]:
        states = [i.name for i in self.scene().selectedItems()
                  if isinstance(i, StateItem) and not i.is_any]
        links = [i.index for i in self.scene().selectedItems() if isinstance(i, TransitionItem)]
        return states, links

    def _selected(self) -> None:
        if self._quiet:
            return
        items = self.scene().selectedItems()
        if len(items) == 1 and isinstance(items[0], StateItem) and not items[0].is_any:
            self.selection_changed.emit("state", items[0].name)
        elif len(items) == 1 and isinstance(items[0], TransitionItem):
            self.selection_changed.emit("transition", items[0].index)
        else:
            self.selection_changed.emit("", None)

    # ---- the mouse --------------------------------------------------------------------------
    def _state_at(self, pos) -> StateItem | None:
        for item in self.items(pos):
            if isinstance(item, StateItem):
                return item
        return None

    def mousePressEvent(self, e) -> None:  # noqa: N802 (Qt's name)
        item = self._state_at(e.position().toPoint())
        if (e.button() == Qt.MouseButton.LeftButton and item is not None and
                e.modifiers() & Qt.KeyboardModifier.ShiftModifier):
            self._linking = item  # a transition from it, wherever it is dropped
            self._rubber_line = QGraphicsPathItem()
            self._rubber_line.setPen(QPen(QColor("#f0c040"), 2, Qt.PenStyle.DashLine))
            self.scene().addItem(self._rubber_line)
            return
        super().mousePressEvent(e)
        self._pressed_at = {i.name: QPointF(i.pos()) for i in self.states.values()}

    def mouseMoveEvent(self, e) -> None:  # noqa: N802 (Qt's name)
        if self._linking is not None:
            path = QPainterPath(self._linking.centre())
            path.lineTo(self.mapToScene(e.position().toPoint()))
            self._rubber_line.setPath(path)
            return
        super().mouseMoveEvent(e)

    def mouseReleaseEvent(self, e) -> None:  # noqa: N802 (Qt's name)
        if self._linking is not None:
            src = self._linking
            self._linking = None
            self.scene().removeItem(self._rubber_line)
            self._rubber_line = None
            dst = self._state_at(e.position().toPoint())
            if dst is not None and not dst.is_any:
                self.connect_requested.emit(src.name, dst.name)
            return
        super().mouseReleaseEvent(e)
        moved = {name: (i.pos().x(), i.pos().y()) for name, i in self.states.items()
                 if name in self._pressed_at and i.pos() != self._pressed_at[name]}
        self._pressed_at = {}
        if moved:
            self.moved.emit(moved)

    def resizeEvent(self, e) -> None:  # noqa: N802 (Qt's name)
        super().resizeEvent(e)
        self.fit()

    def wheelEvent(self, e) -> None:  # noqa: N802 (Qt's name)
        factor = 1.15 if e.angleDelta().y() > 0 else 1 / 1.15
        self.scale(factor, factor)

    def keyPressEvent(self, e) -> None:  # noqa: N802 (Qt's name)
        if e.key() in (Qt.Key.Key_Delete, Qt.Key.Key_Backspace):
            states, links = self.selected()
            if states or links:
                self.delete_requested.emit(states, links)
            return
        super().keyPressEvent(e)

    def contextMenuEvent(self, e) -> None:  # noqa: N802 (Qt's name)
        self.context_menu(self.mapToScene(e.pos()), self._state_at(e.pos())).exec(e.globalPos())

    def context_menu(self, at: QPointF, item: StateItem | None) -> QMenu:
        menu = QMenu(self)
        if item is None:
            x, y = at.x() - STATE_W / 2, at.y() - STATE_H / 2
            menu.addAction("Add Clip State").triggered.connect(
                lambda: self.add_requested.emit(x, y, False))
            menu.addAction("Add Blend State").triggered.connect(
                lambda: self.add_requested.emit(x, y, True))
            return menu
        to = menu.addMenu("Add Transition To")
        for name in self.states:
            if name != ag.ANY and (name != item.name or not item.is_any):
                to.addAction(name).triggered.connect(
                    lambda _=False, n=name: self.connect_requested.emit(item.name, n))
        if not item.is_any:
            menu.addAction("Make Start State").triggered.connect(
                lambda: self.start_requested.emit(item.name))
            menu.addAction("Delete").triggered.connect(
                lambda: self.delete_requested.emit([item.name], []))
        return menu


# ---- the panel ----------------------------------------------------------------------------------
class AnimationPanel(QWidget):
    """The Animation editor dock."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.project: Project | None = None
        self.entity_id = ""
        self.layer = 0
        self.selection: tuple[str, object] = ("", None)
        self.preview_values: dict[str, float] = {}
        self._view = None  # the preview's midgard.Viewport
        self._preview_key = ""
        self._sent: set[str] = set()

        outer = QVBoxLayout(self)
        top = QHBoxLayout()
        self.title = QLabel()
        title_font = QFont()
        title_font.setBold(True)
        self.title.setFont(title_font)
        top.addWidget(self.title, 1)
        self.create_button = QPushButton("Create Graph")
        self.create_button.setToolTip("Give the character an animation graph (it plays one clip "
                                      "without one)")
        self.create_button.clicked.connect(self.create_graph)
        top.addWidget(self.create_button)
        self.remove_button = QPushButton("Remove Graph")
        self.remove_button.clicked.connect(self.remove_graph)
        top.addWidget(self.remove_button)
        outer.addLayout(top)

        layers = QHBoxLayout()
        layers.addWidget(QLabel("Layer"))
        self.layer_combo = QComboBox()
        self.layer_combo.currentIndexChanged.connect(self._layer_chosen)
        layers.addWidget(self.layer_combo, 1)
        self.add_layer_button = QPushButton("Add Layer")
        self.add_layer_button.clicked.connect(self.add_layer)
        layers.addWidget(self.add_layer_button)
        self.remove_layer_button = QPushButton("Remove Layer")
        self.remove_layer_button.clicked.connect(self.remove_layer)
        layers.addWidget(self.remove_layer_button)
        self.root_motion = QCheckBox("Root motion")
        self.root_motion.setToolTip("The character moves as its clips move their hips")
        self.root_motion.toggled.connect(self._root_motion)
        layers.addWidget(self.root_motion)
        outer.addLayout(layers)

        split = QSplitter(Qt.Orientation.Horizontal)
        self.canvas = StateMachineView()
        self.canvas.selection_changed.connect(self._selection)
        self.canvas.moved.connect(self._moved)
        self.canvas.connect_requested.connect(self.add_transition)
        self.canvas.add_requested.connect(lambda x, y, blend: self.add_state((x, y), blend))
        self.canvas.delete_requested.connect(self.delete)
        self.canvas.start_requested.connect(self.make_start)
        split.addWidget(self.canvas)

        side = QWidget()
        side_layout = QVBoxLayout(side)
        side_layout.setContentsMargins(0, 0, 0, 0)
        self.settings_area = QScrollArea()
        self.settings_area.setWidgetResizable(True)
        self.settings_area.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        side_layout.addWidget(self.settings_area, 3)
        self.parameters_area = QScrollArea()
        self.parameters_area.setWidgetResizable(True)
        self.parameters_area.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        side_layout.addWidget(self.parameters_area, 2)
        split.addWidget(side)

        preview = QWidget()
        pv = QVBoxLayout(preview)
        pv.setContentsMargins(0, 0, 0, 0)
        self.preview_image = QLabel("No preview")
        self.preview_image.setMinimumSize(*PREVIEW_SIZE)
        self.preview_image.setAlignment(Qt.AlignmentFlag.AlignCenter)
        pv.addWidget(self.preview_image)
        buttons = QHBoxLayout()
        self.play_button = QPushButton("Play")
        self.play_button.setCheckable(True)
        self.play_button.toggled.connect(self._play)
        buttons.addWidget(self.play_button)
        restart = QPushButton("Restart")
        restart.clicked.connect(self.restart_preview)
        buttons.addWidget(restart)
        pv.addLayout(buttons)
        self.preview_state = QLabel()
        self.preview_state.setWordWrap(True)
        pv.addWidget(self.preview_state)
        self.preview_controls = QWidget()
        self.preview_form = QFormLayout(self.preview_controls)
        pv.addWidget(self.preview_controls)
        pv.addStretch(1)
        split.addWidget(preview)
        split.setSizes([460, 420, 300])
        outer.addWidget(split, 1)

        self.problem = QLabel()
        self.problem.setWordWrap(True)
        self.problem.setStyleSheet("color: #e06c60")
        outer.addWidget(self.problem)

        self.timer = QTimer(self)
        self.timer.setInterval(33)
        self.timer.timeout.connect(lambda: self.tick(0.033))
        self.refresh()

    # ---- what it edits ----------------------------------------------------------------------
    def set_project(self, project: Project | None) -> None:
        if self.project is not None:
            with contextlib.suppress(RuntimeError, TypeError):
                self.project.entities_changed.disconnect(self._entities_changed)
        self.project = project
        self.entity_id = ""
        self._drop_preview()
        if project is not None:
            project.entities_changed.connect(self._entities_changed)
        self.refresh()

    def show_entity(self, entity_id: str) -> None:
        if entity_id == self.entity_id:
            return
        self.entity_id = entity_id or ""
        self.layer = 0
        self.selection = ("", None)
        self.refresh()

    def entity(self) -> Entity | None:
        if self.project is None or not self.entity_id:
            return None
        return self.project.entity(self.entity_id)

    def character(self) -> dict | None:
        entity = self.entity()
        return entity.components.get("character") if entity else None

    def graph(self) -> dict | None:
        c = self.character()
        return c.get("graph") if c else None

    def clips(self) -> list[str]:
        c = self.character()
        return ag.clips_of(self.project, c) if c and self.project else []

    def joints(self) -> list[str]:
        c = self.character()
        info = ag.skinned_info(self.project, c.get("model", "")) if c and self.project else None
        return info["joints"] if info else []

    def commit(self, graph: dict | None, label: str) -> None:
        """One undoable step: the character's graph (none: removed)."""
        entity = self.entity()
        if entity is None or "character" not in entity.components:
            return
        new = copy.deepcopy(entity)
        character = new.components["character"]
        if graph is None:
            character.pop("graph", None)
        else:
            character["graph"] = graph
        if new.components == entity.components:
            return
        self.project.undo_stack.push(SetEntityCommand(self.project, new, label))

    def _entities_changed(self, entity_id: str) -> None:
        if entity_id in ("", self.entity_id):
            self.refresh()

    # ---- showing it -------------------------------------------------------------------------
    def refresh(self) -> None:
        character = self.character()
        graph = character.get("graph") if character else None
        has_entity = character is not None
        if self.project is None:
            self.title.setText("No project open")
        elif self.entity() is None:
            self.title.setText("Select a character in the Scene")
        elif not has_entity:
            self.title.setText(f"{self.entity().name} isn't a character")
        else:
            self.title.setText(f"{self.entity().name}: "
                               f"{'animation graph' if graph else 'plays one clip (no graph)'}")
        self.create_button.setVisible(has_entity and graph is None)
        self.remove_button.setVisible(graph is not None)
        for w in (self.layer_combo, self.add_layer_button, self.remove_layer_button,
                  self.root_motion, self.canvas):
            w.setEnabled(graph is not None)
        self.layer_combo.blockSignals(True)
        self.layer_combo.clear()
        if graph:
            self.layer = min(self.layer, len(graph["layers"]) - 1)
            for lay in graph["layers"]:
                self.layer_combo.addItem(lay.get("name") or "(unnamed)")
            self.layer_combo.setCurrentIndex(self.layer)
        self.layer_combo.blockSignals(False)
        self.remove_layer_button.setEnabled(bool(graph) and len(graph["layers"]) > 1)
        self.root_motion.blockSignals(True)
        self.root_motion.setChecked(bool(graph and graph.get("root_motion")))
        self.root_motion.blockSignals(False)
        if graph:
            self._check_selection(graph)
            self.canvas.show_layer(graph["layers"][self.layer], self.selection)
            self.canvas.fit()
        else:
            self.canvas.scene().clear()
            self.selection = ("", None)
        self._build_settings(graph)
        self._build_parameters(graph)
        self.problem.setText(ag.problem(self.project, character) if character and graph
                             else "")
        self._update_preview()

    def _check_selection(self, graph: dict) -> None:
        kind, key = self.selection
        lay = graph["layers"][self.layer]
        if (kind == "state" and key not in lay["states"]) or \
                (kind == "transition" and not 0 <= key < len(lay.get("transitions", []))):
            self.selection = ("", None)

    def _layer_chosen(self, index: int) -> None:
        if index >= 0 and index != self.layer:
            self.layer = index
            self.selection = ("", None)
            self.refresh()

    def _selection(self, kind: str, key) -> None:
        self.selection = (kind, key)
        self._build_settings(self.graph())

    # ---- graph-wide -------------------------------------------------------------------------
    def create_graph(self) -> None:
        if self.character() is not None and self.graph() is None:
            self.commit(ag.new_graph(self.clips()), "animation graph")

    def remove_graph(self) -> None:
        if self.graph() is not None:
            self.commit(None, "animation graph removed")

    def _root_motion(self, on: bool) -> None:
        graph = self.graph()
        if graph is not None:
            g = copy.deepcopy(graph)
            g["root_motion"] = on
            self.commit(g, "root motion")

    def add_layer(self) -> None:
        graph = self.graph()
        if graph is not None:
            g, self.layer = ag.add_layer(graph, self.clips())
            self.selection = ("", None)
            self.commit(g, "animation layer")

    def remove_layer(self) -> None:
        graph = self.graph()
        if graph is not None and len(graph["layers"]) > 1:
            g = ag.remove_layer(graph, self.layer)
            self.layer = max(0, self.layer - 1)
            self.commit(g, "animation layer removed")

    # ---- states and transitions -------------------------------------------------------------
    def add_state(self, pos: tuple[float, float], blend: bool = False) -> str:
        graph = self.graph()
        if graph is None:
            return ""
        clips = self.clips()
        g, name = ag.add_state(graph, self.layer, pos, clips[0] if clips else "",
                               "speed" if blend else None, "Blend" if blend else "State")
        self.selection = ("state", name)
        self.commit(g, "animation state")
        return name

    def add_transition(self, src: str, dst: str) -> int:
        graph = self.graph()
        if graph is None:
            return -1
        g, index = ag.add_transition(graph, self.layer, src, dst)
        self.selection = ("transition", index)
        self.commit(g, "animation transition")
        return index

    def delete(self, states: list, transitions: list) -> None:
        graph = self.graph()
        if graph is None:
            return
        g = ag.remove_transitions(graph, self.layer, transitions)
        g = ag.remove_states(g, self.layer, states)
        self.selection = ("", None)
        self.commit(g, "animation states removed" if states else "animation transition removed")

    def make_start(self, name: str) -> None:
        graph = self.graph()
        if graph is not None:
            self.commit(ag.set_start(graph, self.layer, name), "start state")

    def _moved(self, positions: dict) -> None:
        graph = self.graph()
        if graph is not None:
            self.commit(ag.move_states(graph, self.layer, positions), "animation states moved")

    # ---- the settings beside the canvas -----------------------------------------------------
    def _build_settings(self, graph: dict | None) -> None:
        page = QWidget()
        form = QFormLayout(page)
        self.settings_form = form
        if graph:
            kind, key = self.selection
            if kind == "state":
                self._state_settings(form, graph, key)
            elif kind == "transition":
                self._transition_settings(form, graph, key)
            else:
                self._layer_settings(form, graph)
        self.settings_area.setWidget(page)

    def _layer_settings(self, form: QFormLayout, graph: dict) -> None:
        lay = graph["layers"][self.layer]
        form.addRow(QLabel("<b>Layer</b>"))
        name = QLineEdit(lay.get("name", ""))
        name.editingFinished.connect(lambda: self._set_layer(name=name.text().strip()))
        form.addRow("Name", name)
        if self.layer > 0:
            joints = self.joints()
            mask = _combo(["(the whole body)", *joints], lay.get("mask", ""),
                          data=["", *joints])
            mask.setToolTip("The joint this layer moves, and every joint under it")
            mask.currentIndexChanged.connect(lambda _i: self._set_layer(mask=mask.currentData()))
            form.addRow("Moves", mask)
            self._number_or_parameter(form, "Weight", graph, lay.get("weight", 1.0), 0.0, 1.0,
                                      lambda v: self._set_layer(weight=v))
        form.addRow(QLabel("Right-click the canvas to add a state; Shift-drag from one state to "
                           "another for a transition."))

    def _set_layer(self, **values) -> None:
        graph = self.graph()
        if graph is not None:
            self.commit(ag.set_layer(graph, self.layer, **values), "animation layer")

    def _number_or_parameter(self, form, label, graph, value, low, high, apply) -> None:
        """A number, or one of the graph's parameters."""
        params = ag.numbers(graph)
        choice = _combo(["(a number)", *params], value if isinstance(value, str) else "",
                        data=["", *params])
        number = _spin(low, high, value if not isinstance(value, str) else 1.0)
        number.setEnabled(not isinstance(value, str))
        choice.currentIndexChanged.connect(
            lambda _i: apply(choice.currentData() or number.value()))
        number.valueChanged.connect(lambda v: apply(round(v, 3)))
        form.addRow(label, _row(number, choice))

    def _state_settings(self, form: QFormLayout, graph: dict, name: str) -> None:
        lay = graph["layers"][self.layer]
        state = lay["states"][name]
        clips = self.clips()
        form.addRow(QLabel(f"<b>State</b>{' (start)' if lay.get('start') == name else ''}"))
        name_edit = QLineEdit(name)
        name_edit.editingFinished.connect(lambda: self._rename_state(name, name_edit.text()))
        form.addRow("Name", name_edit)
        blending = state.get("blend") is not None
        kind = _combo(["A clip", "A blend of clips"], "blend" if blending else "clip",
                      data=["clip", "blend"])
        kind.currentIndexChanged.connect(lambda _i: self._state_kind(name, kind.currentData()))
        form.addRow("Plays", kind)
        if not blending:
            clip = _combo(clips, state.get("clip", ""))
            clip.currentIndexChanged.connect(
                lambda _i: self._set_state(name, clip=clip.currentData()))
            form.addRow("Clip", clip)
        else:
            by = _combo(ag.numbers(graph), state.get("blend", "speed"))
            by.currentIndexChanged.connect(lambda _i: self._set_state(name, blend=by.currentData()))
            form.addRow("Blended by", by)
            for i, (c, at) in enumerate(state.get("clips", [])):
                clip = _combo(clips, c)
                where = _spin(-1000, 1000, at)
                clip.currentIndexChanged.connect(
                    lambda _i, i=i, w=clip: self._blend_point(name, i, clip=w.currentData()))
                where.valueChanged.connect(
                    lambda v, i=i: self._blend_point(name, i, at=round(v, 3)))
                form.addRow(f"Clip {i + 1}", _row(clip, where,
                                                  _remove(lambda i=i: self._blend_point(
                                                      name, i, remove=True))))
            add = QPushButton("Add Clip")
            add.clicked.connect(lambda: self._blend_point(name, -1, clip=clips[0] if clips
                                                          else ""))
            form.addRow("", add)
        self._number_or_parameter(form, "Speed", graph, state.get("speed", 1.0), 0.0, 10.0,
                                  lambda v: self._set_state(name, speed=v))
        loop = QCheckBox("Loops")
        loop.setChecked(state.get("loop", True))
        loop.toggled.connect(lambda on: self._set_state(name, loop=on))
        form.addRow("", loop)
        for i, (point, event) in enumerate(state.get("events", [])):
            at = _spin(0.0, 1.0, point, 0.05)
            text = QLineEdit(event)
            at.valueChanged.connect(lambda v, i=i: self._event(name, i, point=round(v, 3)))
            text.editingFinished.connect(lambda i=i, t=text: self._event(name, i, text=t.text()))
            form.addRow(f"Event {i + 1}", _row(at, text, _remove(
                lambda i=i: self._event(name, i, remove=True))))
        add_event = QPushButton("Add Event")
        add_event.setToolTip("A point in the state's cycle (0 to 1) at which the character's "
                             "script hears on_animation_event")
        add_event.clicked.connect(lambda: self._event(name, -1, text="event"))
        form.addRow("", add_event)
        if lay.get("start") != name:
            start = QPushButton("Make Start State")
            start.clicked.connect(lambda: self.make_start(name))
            form.addRow("", start)

    def _rename_state(self, old: str, new: str) -> None:
        graph = self.graph()
        if graph is None:
            return
        g = ag.rename_state(graph, self.layer, old, new)
        if g != graph:
            self.selection = ("state", new.strip())
            self.commit(g, "animation state renamed")

    def _set_state(self, name: str, **values) -> None:
        graph = self.graph()
        if graph is None or name not in graph["layers"][self.layer]["states"]:
            return
        state = copy.deepcopy(graph["layers"][self.layer]["states"][name])
        state.update(values)
        self.commit(ag.set_state(graph, self.layer, name, state), "animation state")

    def _state_kind(self, name: str, kind: str) -> None:
        graph = self.graph()
        if graph is None:
            return
        state = copy.deepcopy(graph["layers"][self.layer]["states"][name])
        if kind == "blend" and state.get("blend") is None:
            clip = state.pop("clip", "")
            state.update({"blend": "speed", "clips": [[clip, 0.0]] if clip else []})
        elif kind == "clip" and state.get("blend") is not None:
            points = state.pop("clips", [])
            state.pop("blend")
            state["clip"] = points[0][0] if points else ""
        self.commit(ag.set_state(graph, self.layer, name, state), "animation state")

    def _blend_point(self, name: str, index: int, clip=None, at=None, remove=False) -> None:
        graph = self.graph()
        if graph is None:
            return
        state = copy.deepcopy(graph["layers"][self.layer]["states"][name])
        points = state.setdefault("clips", [])
        if index < 0:
            last = points[-1][1] if points else -1.0
            points.append([clip or "", round(last + 1.0, 3)])
        elif remove:
            del points[index]
        else:
            if clip is not None:
                points[index][0] = clip
            if at is not None:
                points[index][1] = at
        self.commit(ag.set_state(graph, self.layer, name, state), "animation blend")

    def _event(self, name: str, index: int, point=None, text=None, remove=False) -> None:
        graph = self.graph()
        if graph is None:
            return
        state = copy.deepcopy(graph["layers"][self.layer]["states"][name])
        events = state.setdefault("events", [])
        if index < 0:
            events.append([0.0, text or "event"])
        elif remove:
            del events[index]
        else:
            if point is not None:
                events[index][0] = point
            if text is not None and text.strip():
                events[index][1] = text.strip()
        if not events:
            state.pop("events")
        self.commit(ag.set_state(graph, self.layer, name, state), "animation event")

    def _transition_settings(self, form: QFormLayout, graph: dict, index: int) -> None:
        t = graph["layers"][self.layer]["transitions"][index]
        src = "Any State" if t.get("from", ag.ANY) == ag.ANY else t.get("from")
        form.addRow(QLabel(f"<b>Transition</b> {src} → {t.get('to')}"))
        triggers = set(graph.get("triggers", []))
        for i, cond in enumerate(t.get("when", [])):
            param = _combo(ag.names(graph), cond[0])
            op = _combo(ag.OPS, cond[1] if len(cond) == 3 else "")
            value = _spin(-1e6, 1e6, float(cond[2]) if len(cond) == 3 else 0.0)
            is_trigger = cond[0] in triggers
            op.setVisible(not is_trigger)
            value.setVisible(not is_trigger)
            param.currentIndexChanged.connect(
                lambda _i, i=i, w=param: self._condition(index, i, name=w.currentData()))
            op.currentIndexChanged.connect(
                lambda _i, i=i, w=op: self._condition(index, i, op=w.currentData()))
            value.valueChanged.connect(lambda v, i=i: self._condition(index, i, value=round(v, 4)))
            form.addRow("When" if i == 0 else "and", _row(param, op, value, _remove(
                lambda i=i: self._condition(index, i, remove=True))))
        add = QPushButton("Add Condition")
        add.clicked.connect(lambda: self._condition(index, -1))
        form.addRow("", add)
        blend = _spin(0.0, 10.0, t.get("blend", 0.2), 0.05)
        blend.setSuffix(" s")
        blend.valueChanged.connect(lambda v: self._set_transition(index, blend=round(v, 3)))
        form.addRow("Cross-fade", blend)
        after_on = QCheckBox("Only after")
        after_on.setChecked("after" in t)
        after = _spin(0.0, 100.0, t.get("after", 1.0), 0.25)
        after.setSuffix(" cycles")
        after.setEnabled("after" in t)
        after_on.toggled.connect(lambda on: self._set_transition(
            index, after=round(after.value(), 3) if on else None))
        after.valueChanged.connect(lambda v: self._set_transition(index, after=round(v, 3)))
        form.addRow(after_on, after)
        remove = QPushButton("Delete Transition")
        remove.clicked.connect(lambda: self.delete([], [index]))
        form.addRow("", remove)

    def _set_transition(self, index: int, **values) -> None:
        graph = self.graph()
        if graph is None:
            return
        t = copy.deepcopy(graph["layers"][self.layer]["transitions"][index])
        for k, v in values.items():
            if v is None:
                t.pop(k, None)
            else:
                t[k] = v
        self.commit(ag.set_transition(graph, self.layer, index, t), "animation transition")

    def _condition(self, index: int, i: int, name=None, op=None, value=None,
                   remove=False) -> None:
        graph = self.graph()
        if graph is None:
            return
        t = copy.deepcopy(graph["layers"][self.layer]["transitions"][index])
        when = t.setdefault("when", [])
        triggers = set(graph.get("triggers", []))
        if i < 0:
            when.append(["speed", ">", 0.1])
        elif remove:
            del when[i]
        else:
            cond = list(when[i])
            if name is not None:
                cond = [name] if name in triggers else [name, *(cond[1:] or [">", 0.0])]
            if op is not None and len(cond) == 3:
                cond[1] = op
            if value is not None and len(cond) == 3:
                cond[2] = value
            when[i] = cond
        self.commit(ag.set_transition(graph, self.layer, index, t), "animation condition")

    # ---- parameters -------------------------------------------------------------------------
    def _build_parameters(self, graph: dict | None) -> None:
        page = QWidget()
        form = QFormLayout(page)
        if graph:
            heading = QLabel("<b>Parameters</b> (speed and vertical_speed: from how the "
                             "character moves)")
            heading.setWordWrap(True)
            form.addRow(heading)
            for name, value in graph.get("parameters", {}).items():
                self._parameter_row(form, graph, name, value)
            for name in graph.get("triggers", []):
                self._parameter_row(form, graph, name, None)
            add = QPushButton("Add Parameter")
            add.clicked.connect(lambda: self.add_parameter(False))
            add_trigger = QPushButton("Add Trigger")
            add_trigger.clicked.connect(lambda: self.add_parameter(True))
            form.addRow(_row(add, add_trigger))
        self.parameters_area.setWidget(page)

    def _parameter_row(self, form: QFormLayout, graph: dict, name: str, value) -> None:
        edit = QLineEdit(name)
        edit.editingFinished.connect(lambda: self._rename_parameter(name, edit.text()))
        widgets = [edit]
        if value is None:
            widgets.append(QLabel("trigger"))
        else:
            start = _spin(-1e6, 1e6, value)
            start.setToolTip("Its value when the graph starts")
            start.valueChanged.connect(lambda v: self._parameter_value(name, round(v, 4)))
            widgets.append(start)
        remove = _remove(lambda: self._remove_parameter(name))
        used = ag.uses(graph, name)
        remove.setEnabled(not used)
        if used:
            remove.setToolTip(f"Used in {used} place{'s' if used > 1 else ''}")
        widgets.append(remove)
        form.addRow(_row(*widgets))

    def add_parameter(self, trigger: bool) -> str:
        graph = self.graph()
        if graph is None:
            return ""
        g, name = ag.add_parameter(graph, trigger)
        self.commit(g, "animation trigger" if trigger else "animation parameter")
        return name

    def _rename_parameter(self, old: str, new: str) -> None:
        graph = self.graph()
        if graph is not None:
            g = ag.rename_parameter(graph, old, new)
            if g != graph:
                self.commit(g, "animation parameter renamed")

    def _parameter_value(self, name: str, value: float) -> None:
        graph = self.graph()
        if graph is not None:
            self.commit(ag.set_parameter(graph, name, value), "animation parameter")

    def _remove_parameter(self, name: str) -> None:
        graph = self.graph()
        if graph is not None:
            self.commit(ag.remove_parameter(graph, name), "animation parameter removed")

    # ---- the preview ------------------------------------------------------------------------
    def _drop_preview(self) -> None:
        self.timer.stop()
        if self._view is not None:
            with contextlib.suppress(RuntimeError):
                self._view.detach()
        self._view = None
        self._sent = set()
        self._preview_key = ""

    def _update_preview(self) -> None:
        """The character in the preview as it is now (its graph from the start if it
        changed), and the controls for its parameters."""
        character = self.character()
        self._build_preview_controls(character.get("graph") if character else None)
        midgard = engine.module()
        if character is None or midgard is None or not character.get("model"):
            self.preview_image.setText("No preview")
            self.preview_state.setText("")
            self.play_button.setChecked(False)
            return
        try:
            if self._view is None:
                self._view = midgard.Viewport()
                self._view.attach_offscreen(*PREVIEW_SIZE)
                self._view.set_settings(json.dumps({"sky": [0.16, 0.18, 0.22]}))
                self._view.put_entity(json.dumps({"id": "sun", "name": "Sun", "components": {
                    "transform": {"rotation": [-40, 30, 0]}, "light": {"type": "directional"}}}))
        except RuntimeError as e:  # no Vulkan driver
            self._view = None
            self.preview_image.setText(f"No preview: {e}")
            return
        for asset in (character.get("model"), character.get("clips")):
            if asset and asset not in self._sent:
                data = mesh_asset_data(self.project, asset)
                if data is not None:
                    try:
                        self._view.set_mesh(asset, data)
                    except RuntimeError as e:
                        log.warning("Animation preview: %s: %s", asset, e)
                self._sent.add(asset)
        entity = self.entity()
        components = {"transform": {}, "character": character}
        if "material" in entity.components:
            components["material"] = entity.components["material"]
        key = json.dumps(components, sort_keys=True)
        if key != self._preview_key:
            self._preview_key = key
            self._view.put_entity(json.dumps({"id": PREVIEW_ID, "name": "Preview",
                                              "components": components}))
            self._view.set_camera([0.0, 1.2, 3.6], [0.0, 0.9, 0.0], 40.0)
        self.tick(0.0)

    def _build_preview_controls(self, graph: dict | None) -> None:
        while self.preview_form.rowCount():
            self.preview_form.removeRow(0)
        if not graph:
            return
        for name in ag.numbers(graph):
            value = self.preview_values.get(name, graph.get("parameters", {}).get(name, 0.0))
            self.preview_values[name] = value
            box = _spin(-1e4, 1e4, value, 0.1, 2)
            box.setKeyboardTracking(True)
            box.valueChanged.connect(lambda v, n=name: self.set_preview_value(n, v))
            self.preview_form.addRow(name, box)
        for name in graph.get("triggers", []):
            fire = QPushButton(f"Fire {name}")
            fire.clicked.connect(lambda _=False, n=name: self.fire_preview_trigger(n))
            self.preview_form.addRow("", fire)

    def set_preview_value(self, name: str, value: float) -> None:
        self.preview_values[name] = value
        if not self.play_button.isChecked():
            self.tick(0.0)

    def fire_preview_trigger(self, name: str) -> None:
        if self._view is not None:
            with contextlib.suppress(ValueError):
                self._view.trigger_animation(PREVIEW_ID, name)

    def restart_preview(self) -> None:
        if self._view is not None:
            with contextlib.suppress(ValueError):
                self._view.restart_animation(PREVIEW_ID)
            self.tick(0.0)

    def _play(self, on: bool) -> None:
        self.play_button.setText("Pause" if on else "Play")
        if on and self._view is not None:
            self.timer.start()
        else:
            self.timer.stop()

    def tick(self, dt: float) -> None:
        """The preview moved on by dt, with its parameters as set, and drawn."""
        if self._view is None:
            return
        graph = self.graph()
        if graph:
            for name, value in self.preview_values.items():
                with contextlib.suppress(ValueError):
                    self._view.set_animation(PREVIEW_ID, name, float(value))
        try:
            self._view.animate(dt)
            self._view.render()
            pixels = self._view.read_pixels()
        except RuntimeError as e:
            self.preview_image.setText(f"No preview: {e}")
            return
        image = QImage(pixels, *PREVIEW_SIZE, QImage.Format.Format_RGBA8888).copy()
        self.preview_image.setPixmap(QPixmap.fromImage(image))
        if graph:
            states = []
            for i, lay in enumerate(graph["layers"]):
                with contextlib.suppress(ValueError):
                    states.append(f"{lay.get('name')}: {self._view.animation_state(PREVIEW_ID, i)}")
            self.preview_state.setText("   ".join(states))
        else:
            self.preview_state.setText("")

    def closeEvent(self, e) -> None:  # noqa: N802 (Qt's name)
        self._drop_preview()
        super().closeEvent(e)
