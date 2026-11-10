# SPDX-License-Identifier: Apache-2.0
"""The Menu editor (milestone 6.0): screens for the game, laid out on a canvas.

On the left, the project's screens (New, Duplicate, Delete) and the chosen screen's elements,
panels holding their parts (Add, Delete, Move Up, Move Down: later ones draw on top). In the
middle, the canvas: the screen drawn by the engine exactly as the game draws it, at a desktop
size or a phone's (sideways or upright, clear of its notch). Click an element to choose it, drag
it to move it, drag its corner to resize it. On the right, the chosen element's settings (or the
screen's, with nothing chosen): its anchor, place and size in dp, text and look, and what a
button does. Under the canvas, what's wrong with the screen, if anything.

Every change is a step on the project's undo stack (typing in one field, or one drag, is one
step) and reaches a game playing under the editor at once; Show in Game shows the screen there.
"""
import copy
import json
from collections.abc import Callable
from typing import Any

from PySide6.QtCore import QPointF, QRectF, Qt, Signal
from PySide6.QtGui import QColor, QImage, QMouseEvent, QPainter, QPen
from PySide6.QtWidgets import (
    QCheckBox,
    QColorDialog,
    QComboBox,
    QDoubleSpinBox,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMenu,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QSplitter,
    QToolButton,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

from .. import engine, menus
from ..project import Project

ID_ROLE = Qt.ItemDataRole.UserRole
HANDLE = 8  # pixels: the resize handle at a chosen element's bottom right corner
_preview = None  # the engine's ScreenPreview: one for the editor (the engine runs one UI)


def screen_preview():
    """The engine's screen preview, made when first needed."""
    global _preview
    if _preview is None:
        _preview = engine.module().ScreenPreview()
    return _preview


def _spin(low: float, high: float, decimals: int = 0, suffix: str = "") -> QDoubleSpinBox:
    box = QDoubleSpinBox()
    box.setRange(low, high)
    box.setDecimals(decimals)
    box.setKeyboardTracking(False)
    if suffix:
        box.setSuffix(suffix)
    return box


class ColourButton(QWidget):
    """A colour (with its opacity) to choose, or none ("Default": the base style's)."""

    changed = Signal(object)  # [r, g, b, a] or None

    def __init__(self, title: str, can_clear: bool = True):
        super().__init__()
        self.title = title
        self.value: list[float] | None = None
        self.button = QPushButton()
        self.button.clicked.connect(self._choose)
        self.clear = QToolButton()
        self.clear.setText("Default")
        self.clear.setToolTip("Use the usual colour")
        self.clear.clicked.connect(lambda: self.changed.emit(None))
        self.clear.setVisible(can_clear)
        row = QHBoxLayout(self)
        row.setContentsMargins(0, 0, 0, 0)
        row.addWidget(self.button, 1)
        row.addWidget(self.clear)

    def set_value(self, value: list[float] | None) -> None:
        self.value = list(value) if value else None
        if self.value is None:
            self.button.setText("Default")
            self.button.setStyleSheet("")
            return
        c = QColor.fromRgbF(*[min(max(v, 0.0), 1.0) for v in (self.value + [1.0])[:4]])
        self.button.setText(f"{c.name()}  {round(c.alphaF() * 100)}%")
        text = "#000000" if c.lightnessF() > 0.5 else "#ffffff"
        self.button.setStyleSheet(f"background-color: {c.name()}; color: {text};")

    def _choose(self) -> None:
        start = QColor.fromRgbF(*((self.value or [0.5, 0.5, 0.5, 1.0]) + [1.0])[:4])
        chosen = QColorDialog.getColor(start, self, self.title,
                                       QColorDialog.ColorDialogOption.ShowAlphaChannel)
        if chosen.isValid():
            self.changed.emit([round(chosen.redF(), 3), round(chosen.greenF(), 3),
                               round(chosen.blueF(), 3), round(chosen.alphaF(), 3)])


class Canvas(QWidget):
    """The screen as the game draws it, scaled to fit, with the chosen element outlined: click
    to choose, drag to move, drag the corner to resize."""

    picked = Signal(str)                    # an element's id ("" for none)
    dragged = Signal(str, float, float, bool)  # id, dp across and down, resizing (True)
    dropped = Signal()                      # the drag ended

    def __init__(self):
        super().__init__()
        self.image: QImage | None = None
        self.boxes: dict[str, tuple[float, float, float, float]] = {}
        self.order: list[str] = []  # elements, drawn first to last
        self.chosen = ""
        self.dp = 1.0
        self.message = ""
        self._drag: tuple[str, QPointF, bool] | None = None
        self.setMinimumSize(320, 240)
        self.setMouseTracking(True)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)

    def _fit(self) -> tuple[float, QPointF]:
        if self.image is None or self.image.width() == 0:
            return 1.0, QPointF(0, 0)
        scale = min(self.width() / self.image.width(), self.height() / self.image.height())
        off = QPointF((self.width() - self.image.width() * scale) / 2,
                      (self.height() - self.image.height() * scale) / 2)
        return scale, off

    def _rect(self, element: str) -> QRectF | None:
        box = self.boxes.get(element)
        if box is None:
            return None
        scale, off = self._fit()
        return QRectF(off.x() + box[0] * scale, off.y() + box[1] * scale, box[2] * scale,
                      box[3] * scale)

    def element_at(self, point: QPointF) -> str:
        """The topmost element under a point (later elements draw on top)."""
        for element in reversed(self.order):
            r = self._rect(element)
            if r is not None and r.contains(point):
                return element
        return ""

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.fillRect(self.rect(), QColor(40, 44, 52))
        if self.image is None:
            p.setPen(QColor(200, 200, 200))
            p.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter | Qt.TextFlag.TextWordWrap,
                       self.message or "No screen chosen")
            return
        scale, off = self._fit()
        target = QRectF(off.x(), off.y(), self.image.width() * scale,
                        self.image.height() * scale)
        p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        p.drawImage(target, self.image)
        r = self._rect(self.chosen)
        if r is not None:
            p.setPen(QPen(QColor(255, 210, 122), 2, Qt.PenStyle.DashLine))
            p.drawRect(r)
            p.fillRect(QRectF(r.right() - HANDLE, r.bottom() - HANDLE, HANDLE * 2, HANDLE * 2),
                       QColor(255, 210, 122))

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() != Qt.MouseButton.LeftButton:
            return
        point = event.position()
        r = self._rect(self.chosen)
        resizing = r is not None and abs(point.x() - r.right()) <= HANDLE * 1.5 and \
            abs(point.y() - r.bottom()) <= HANDLE * 1.5
        element = self.chosen if resizing else self.element_at(point)
        if element != self.chosen:
            self.picked.emit(element)
        if element:
            self._drag = (element, point, resizing)

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        if self._drag is None:
            r = self._rect(self.chosen)
            near = r is not None and abs(event.position().x() - r.right()) <= HANDLE * 1.5 \
                and abs(event.position().y() - r.bottom()) <= HANDLE * 1.5
            self.setCursor(Qt.CursorShape.SizeFDiagCursor if near else Qt.CursorShape.ArrowCursor)
            return
        element, start, resizing = self._drag
        scale, _off = self._fit()
        per_dp = max(scale * self.dp, 1e-6)
        dx = (event.position().x() - start.x()) / per_dp
        dy = (event.position().y() - start.y()) / per_dp
        self.dragged.emit(element, round(dx), round(dy), resizing)

    def mouseReleaseEvent(self, _event: QMouseEvent) -> None:
        if self._drag is not None:
            self._drag = None
            self.dropped.emit()


class MenuEditor(QWidget):
    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.project: Project | None = None
        self._filling = False
        self._drag_start: list[dict] | None = None  # every screen, as the drag began
        self._drag_base: dict | None = None  # the element as the drag began
        self.show_in_game: Callable[[str], None] | None = None  # set while a game plays
        self._dragging: tuple[str | None, dict | None] = (None, None)
        self._gui_images: dict[str, str] = {}

        # ---- left: screens and elements ----
        self.screen_list = QListWidget()
        self.screen_list.currentItemChanged.connect(lambda *_: self._screen_chosen())
        self.new_button = QPushButton("New")
        self.new_button.setToolTip("A new screen: a menu with a title and a Close button")
        self.new_button.clicked.connect(lambda: self.new_screen())
        self.duplicate_button = QPushButton("Duplicate")
        self.duplicate_button.clicked.connect(self.duplicate_screen)
        self.delete_button = QPushButton("Delete")
        self.delete_button.clicked.connect(self.delete_screen)
        screen_buttons = QHBoxLayout()
        for b in (self.new_button, self.duplicate_button, self.delete_button):
            screen_buttons.addWidget(b)

        self.tree = QTreeWidget()
        self.tree.setHeaderHidden(True)
        self.tree.currentItemChanged.connect(lambda *_: self._element_chosen())
        self.add_button = QToolButton()
        self.add_button.setText("Add")
        self.add_button.setPopupMode(QToolButton.ToolButtonPopupMode.InstantPopup)
        add_menu = QMenu(self.add_button)
        for kind, label in menus.KINDS.items():
            add_menu.addAction(label, lambda k=kind: self.add_element(k))
        self.add_button.setMenu(add_menu)
        self.add_button.setToolTip("Add a panel, text, button, slider or check box (inside the "
                                   "chosen panel)")
        self.remove_button = QPushButton("Delete")
        self.remove_button.clicked.connect(self.delete_element)
        self.up_button = QPushButton("Up")
        self.up_button.setToolTip("Earlier: drawn under the others")
        self.up_button.clicked.connect(lambda: self.move_element(-1))
        self.down_button = QPushButton("Down")
        self.down_button.setToolTip("Later: drawn on top of the others")
        self.down_button.clicked.connect(lambda: self.move_element(1))
        element_buttons = QHBoxLayout()
        element_buttons.addWidget(self.add_button)
        for b in (self.remove_button, self.up_button, self.down_button):
            element_buttons.addWidget(b)

        left = QWidget()
        left_layout = QVBoxLayout(left)
        left_layout.setContentsMargins(0, 0, 0, 0)
        left_layout.addWidget(QLabel("<b>Screens</b>"))
        left_layout.addWidget(self.screen_list, 1)
        left_layout.addLayout(screen_buttons)
        left_layout.addWidget(QLabel("<b>Elements</b>"))
        left_layout.addWidget(self.tree, 2)
        left_layout.addLayout(element_buttons)

        # ---- middle: the canvas ----
        self.size_box = QComboBox()
        for label, *_rest in menus.PREVIEWS:
            self.size_box.addItem(label)
        self.size_box.currentIndexChanged.connect(lambda _i: self.render())
        self.show_button = QPushButton("Show in Game")
        self.show_button.setToolTip("Shows this screen in the game playing now (Play first)")
        self.show_button.clicked.connect(self._show_in_game)
        top = QHBoxLayout()
        top.addWidget(QLabel("Preview"))
        top.addWidget(self.size_box)
        top.addStretch(1)
        top.addWidget(self.show_button)
        self.canvas = Canvas()
        self.canvas.picked.connect(self.choose_element)
        self.canvas.dragged.connect(self._dragged)
        self.canvas.dropped.connect(self._dropped)
        self.problems = QLabel()
        self.problems.setWordWrap(True)
        self.problems.setStyleSheet("color: #d08030;")
        middle = QWidget()
        middle_layout = QVBoxLayout(middle)
        middle_layout.setContentsMargins(0, 0, 0, 0)
        middle_layout.addLayout(top)
        middle_layout.addWidget(self.canvas, 1)
        middle_layout.addWidget(self.problems)

        # ---- right: settings ----
        self.form = QFormLayout()
        self._build_form()
        form_widget = QWidget()
        form_widget.setLayout(self.form)
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setWidget(form_widget)

        split = QSplitter(Qt.Orientation.Horizontal)
        split.addWidget(left)
        split.addWidget(middle)
        split.addWidget(scroll)
        split.setStretchFactor(1, 1)
        split.setSizes([220, 640, 300])
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        intro = QLabel("Screens for your game: menus, the title screen, pause, game over. "
                       "Scripts show them with <code>midgard.ui.show</code> and hear their "
                       "buttons with <code>midgard.ui.on_click</code>.")
        intro.setWordWrap(True)
        layout.addWidget(intro)
        layout.addWidget(split, 1)
        self.set_project(None)

    # ---- the settings form ---------------------------------------------------------------------
    def _build_form(self) -> None:
        f = self.form
        # The screen's (nothing chosen).
        self.screen_name = QLineEdit()
        self.screen_name.editingFinished.connect(self._rename_screen)
        self.screen_id = QLabel()
        self.screen_id.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.modal = QCheckBox("Takes the player's input")
        self.modal.setToolTip("A menu: the game's controls wait and the mouse is free while it "
                              "shows (untick for an overlay the player plays under)")
        self.modal.toggled.connect(lambda on: self._set_screen("modal", on, "takes input"))
        self.pauses = QCheckBox("Pauses the game")
        self.pauses.setToolTip("Everything stands still while it shows (a pause menu)")
        self.pauses.toggled.connect(lambda on: self._set_screen("pauses", on, "pauses"))
        self.background = ColourButton("Background")
        self.background.changed.connect(
            lambda c: self._set_screen("background", c, "background"))
        # An element's.
        self.element_id = QLineEdit()
        self.element_id.setToolTip("Its name for scripts: midgard.ui.on_click(screen, this, …)")
        self.element_id.editingFinished.connect(self._rename_element)
        self.kind_label = QLabel()
        self.parent_box = QComboBox()
        self.parent_box.setToolTip("The panel it is inside (it moves with it), or the screen")
        self.parent_box.activated.connect(lambda _i: self._set_parent())
        self.anchor = QComboBox()
        for key, label in menus.ANCHORS.items():
            self.anchor.addItem(label, key)
        self.anchor.setToolTip("Where on its panel (or the screen) it hangs: it keeps its place "
                               "from there on every screen size. Fill stretches it, X and Y "
                               "then its margins")
        self.anchor.activated.connect(lambda _i: self._set("anchor", self.anchor.currentData()))
        self.x = _spin(-4000, 4000, suffix=" dp")
        self.y = _spin(-4000, 4000, suffix=" dp")
        self.w = _spin(0, 4000, suffix=" dp")
        self.h = _spin(0, 4000, suffix=" dp")
        for key, box in (("x", self.x), ("y", self.y), ("w", self.w), ("h", self.h)):
            box.valueChanged.connect(lambda v, k=key: self._set(k, round(v, 1)))
        self.x.setToolTip("Across from the anchor (right is more)")
        self.y.setToolTip("Down from the anchor (down is more)")
        self.text = QLineEdit()
        self.text.editingFinished.connect(lambda: self._set("text", self.text.text()))
        self.font_size = _spin(0, 200, suffix=" dp")
        self.font_size.setSpecialValueText("Default")
        self.font_size.valueChanged.connect(lambda v: self._set_style("font_size", v or None))
        self.align = QComboBox()
        for key, label in (("", "Default"), ("left", "Left"), ("center", "Middle"),
                           ("right", "Right")):
            self.align.addItem(label, key)
        self.align.activated.connect(
            lambda _i: self._set_style("align", self.align.currentData() or None))
        self.bold = QCheckBox("Bold")
        self.bold.toggled.connect(lambda on: self._set_style("bold", on or None))
        self.colour = ColourButton("Text colour")
        self.colour.changed.connect(lambda c: self._set_style("color", c))
        self.fill = ColourButton("Background")
        self.fill.changed.connect(lambda c: self._set_style("background", c))
        self.border = ColourButton("Border")
        self.border.changed.connect(lambda c: self._set_style("border", c))
        self.border_width = _spin(0, 40, 1, " dp")
        self.border_width.setSpecialValueText("Default")
        self.border_width.valueChanged.connect(
            lambda v: self._set_style("border_width", v or None))
        self.radius = _spin(0, 400, 1, " dp")
        self.radius.setSpecialValueText("Default")
        self.radius.valueChanged.connect(lambda v: self._set_style("radius", v or None))
        self.action = QComboBox()
        for key, label in menus.ACTIONS.items():
            self.action.addItem(label, key)
        self.action.setToolTip("What clicking it does. Scripts hear every click as well "
                               "(midgard.ui.on_click)")
        self.action.activated.connect(lambda _i: self._set_action())
        self.target = QComboBox()
        self.target.setToolTip("The screen it opens, on top of this one (closing that one "
                               "comes back here)")
        self.target.activated.connect(lambda _i: self._set_action())
        self.low = _spin(-1e6, 1e6, 2)
        self.high = _spin(-1e6, 1e6, 2)
        self.step = _spin(0, 1e6, 3)
        self.step.setSpecialValueText("Smooth")
        self.value = _spin(-1e6, 1e6, 2)
        for key, box in (("min", self.low), ("max", self.high), ("step", self.step),
                         ("value", self.value)):
            box.valueChanged.connect(lambda v, k=key: self._set(k, round(v, 4)))
        # Pictures from the Asset Library's GUI section: an element's, and its states'.
        self.pictures: dict[str, QComboBox] = {}
        for slot in ("image", "hover", "pressed", "disabled", "unchecked", "checked", "track",
                     "pointer", "screen_image"):
            box = QComboBox()
            box.setToolTip("A picture from the Asset Library's GUI section, stretched to fit "
                           "(add pictures there: Assets ▸ Asset Library, GUI). Left empty, a "
                           "state's picture is its normal one")
            box.activated.connect(lambda _i, s=slot: self._set_picture(s))
            self.pictures[slot] = box
        self.image = self.pictures["image"]
        self.enabled = QCheckBox("Can be used")
        self.enabled.setToolTip("Untick for a control the player can't use until a script "
                                "enables it (midgard.ui.set_enabled): it shows its disabled "
                                "picture, or greyed")
        self.enabled.toggled.connect(lambda on: self._set("enabled", None if on else False))
        self.group = QLineEdit()
        self.group.setToolTip("Options with the same group are one choice: choosing one "
                              "unchooses the others")
        self.group.editingFinished.connect(lambda: self._set("group",
                                                             self.group.text().strip() or None))
        self.ticked = QCheckBox("Ticked to start with")
        self.ticked.toggled.connect(lambda on: self._set("value", on))
        self.rows: dict[str, QWidget] = {}
        for label, widget, key in (
                ("Name", self.screen_name, "screen_name"), ("Id", self.screen_id, "screen_id"),
                ("", self.modal, "modal"), ("", self.pauses, "pauses"),
                ("Background", self.background, "background"),
                ("Background picture", self.pictures["screen_image"], "screen_image"),
                ("Name", self.element_id, "id"), ("Kind", self.kind_label, "kind"),
                ("Image", self.image, "image"),
                ("Hover picture", self.pictures["hover"], "hover"),
                ("Pressed picture", self.pictures["pressed"], "pressed"),
                ("Disabled picture", self.pictures["disabled"], "disabled"),
                ("Unchecked picture", self.pictures["unchecked"], "unchecked"),
                ("Checked picture", self.pictures["checked"], "checked"),
                ("Background picture", self.pictures["track"], "track"),
                ("Pointer picture", self.pictures["pointer"], "pointer"),
                ("", self.enabled, "enabled"), ("Group", self.group, "group"),
                ("Inside", self.parent_box, "parent"), ("Anchor", self.anchor, "anchor"),
                ("X", self.x, "x"), ("Y", self.y, "y"), ("Width", self.w, "w"),
                ("Height", self.h, "h"), ("Text", self.text, "text"),
                ("Text size", self.font_size, "font_size"), ("Align", self.align, "align"),
                ("", self.bold, "bold"), ("Text colour", self.colour, "color"),
                ("Fill", self.fill, "fill"), ("Border", self.border, "border"),
                ("Border width", self.border_width, "border_width"),
                ("Corners", self.radius, "radius"), ("Does", self.action, "action"),
                ("Screen", self.target, "target"), ("Least", self.low, "min"),
                ("Most", self.high, "max"), ("Step", self.step, "step"),
                ("Starts at", self.value, "value"), ("", self.ticked, "ticked")):
            f.addRow(label, widget)
            self.rows[key] = widget

    # ---- the project ---------------------------------------------------------------------------
    def set_project(self, project: Project | None) -> None:
        if self.project is not None:
            self.project.screens_changed.disconnect(self._changed)
        self.project = project
        if project is not None:
            project.screens_changed.connect(self._changed)
        self.setEnabled(project is not None and menus.has_table(project))
        self.refresh()

    def _changed(self, *_args) -> None:
        if self._drag_start is None:
            self.refresh()

    def all(self) -> list[dict[str, Any]]:
        return menus.load_all(self.project) if self.project is not None else []

    def chosen_screen_id(self) -> str | None:
        item = self.screen_list.currentItem()
        return item.data(ID_ROLE) if item is not None else None

    def chosen_screen(self) -> dict[str, Any] | None:
        sid = self.chosen_screen_id()
        return next((s for s in self.all() if s["id"] == sid), None)

    def chosen_element_id(self) -> str:
        item = self.tree.currentItem()
        return item.data(0, ID_ROLE) if item is not None else ""

    def chosen_element(self) -> dict[str, Any] | None:
        s = self.chosen_screen()
        eid = self.chosen_element_id()
        if s is None or not eid:
            return None
        return next((e for e in s["screen"].get("elements", []) if e.get("id") == eid), None)

    def refresh(self) -> None:
        chosen = self.chosen_screen_id()
        self.screen_list.blockSignals(True)
        self.screen_list.clear()
        for s in self.all():
            item = QListWidgetItem(s["name"])
            item.setData(ID_ROLE, s["id"])
            self.screen_list.addItem(item)
            if s["id"] == chosen:
                self.screen_list.setCurrentItem(item)
        if self.screen_list.currentItem() is None and self.screen_list.count():
            self.screen_list.setCurrentRow(0)
        self.screen_list.blockSignals(False)
        self._fill_tree()

    def choose_screen(self, screen_id: str) -> None:
        for i in range(self.screen_list.count()):
            if self.screen_list.item(i).data(ID_ROLE) == screen_id:
                self.screen_list.setCurrentRow(i)
                return

    def _screen_chosen(self) -> None:
        self.tree.setCurrentItem(None)
        self._fill_tree()

    def _fill_tree(self) -> None:
        s = self.chosen_screen()
        chosen = self.chosen_element_id()
        self.tree.blockSignals(True)
        self.tree.clear()
        if s is not None:
            items: dict[str, QTreeWidgetItem] = {}
            for e in s["screen"].get("elements", []):
                label = f"{e.get('id')}  ({menus.KINDS.get(e.get('kind', 'text'), '?').lower()})"
                parent = items.get(e.get("parent", ""))
                item = QTreeWidgetItem(parent if parent is not None else self.tree, [label])
                item.setData(0, ID_ROLE, e.get("id"))
                items[e.get("id")] = item
                if e.get("id") == chosen:
                    self.tree.setCurrentItem(item)
            self.tree.expandAll()
        self.tree.blockSignals(False)
        self._show()

    def choose_element(self, element: str) -> None:
        if not element:
            self.tree.setCurrentItem(None)
            self._show()
            return
        found = self.tree.findItems("*", Qt.MatchFlag.MatchWildcard | Qt.MatchFlag.MatchRecursive)
        for item in found:
            if item.data(0, ID_ROLE) == element:
                self.tree.setCurrentItem(item)
                return

    def _element_chosen(self) -> None:
        self._show()

    def _show(self) -> None:
        s = self.chosen_screen()
        e = self.chosen_element()
        has_screen = s is not None
        for b in (self.duplicate_button, self.delete_button, self.add_button, self.show_button):
            b.setEnabled(has_screen)
        for b in (self.remove_button, self.up_button, self.down_button):
            b.setEnabled(e is not None)
        self._gui_images = menus.gui_images(self.project) if self.project is not None else {}
        self._filling = True
        try:
            screen_rows = {"screen_name", "screen_id", "modal", "pauses", "background",
                           "screen_image"}
            kind = e.get("kind", "text") if e else ""
            slots = [slot for slot, _label in menus.PICTURES.get(kind, [("image", "Image")])]
            element_rows = {"id", "kind", "parent", "anchor", "x", "y", "w", "h", *slots}
            if kind in ("button", "slider", "check", "option"):
                element_rows |= {"enabled"}
            if kind == "option":
                element_rows |= {"group", "ticked"}
            if kind in ("text", "button", "check", "option"):
                element_rows |= {"text", "font_size", "align", "bold", "color"}
            if kind in ("panel", "text", "button"):
                element_rows |= {"fill", "border", "border_width", "radius"}
            if kind == "button":
                element_rows |= {"action"}
                if e.get("action", {}).get("do") == "open":
                    element_rows |= {"target"}
            if kind == "slider":
                element_rows |= {"min", "max", "step", "value"}
            if kind == "check":
                element_rows |= {"ticked"}
            if kind == "option":
                self.ticked.setText("Chosen to start with")
            elif kind == "check":
                self.ticked.setText("Ticked to start with")
            shown = (element_rows if e else screen_rows) if has_screen else set()
            for key, widget in self.rows.items():
                self.form.setRowVisible(widget, key in shown)
            if s is not None and e is None:
                screen = s["screen"]
                self.screen_name.setText(s["name"])
                self.screen_id.setText(f'<code>midgard.ui.show("{s["id"]}")</code>')
                self.modal.setChecked(screen.get("modal", True) is not False)
                self.pauses.setChecked(bool(screen.get("pauses", False)))
                self.background.set_value(screen.get("background"))
                self._fill_picture("screen_image", screen.get("image", ""))
            if e is not None:
                self.element_id.setText(e.get("id", ""))
                self.kind_label.setText(menus.KINDS.get(kind, kind))
                self.parent_box.clear()
                self.parent_box.addItem("The screen", "")
                for other in s["screen"].get("elements", []):
                    if other.get("id") == e.get("id"):
                        break
                    if other.get("kind") == "panel":
                        self.parent_box.addItem(other["id"], other["id"])
                self.parent_box.setCurrentIndex(max(0, self.parent_box.findData(
                    e.get("parent", ""))))
                self.anchor.setCurrentIndex(max(0, self.anchor.findData(
                    e.get("anchor", "top_left"))))
                for key, box in (("x", self.x), ("y", self.y), ("w", self.w), ("h", self.h)):
                    box.setValue(float(e.get(key, {"w": 100, "h": 40}.get(key, 0))))
                fill = e.get("anchor") == "fill"
                self.form.labelForField(self.x).setText("Margin, sides" if fill else "X")
                self.form.labelForField(self.y).setText("Margin, top and bottom" if fill
                                                        else "Y")
                self.form.setRowVisible(self.w, "w" in shown and not fill)
                self.form.setRowVisible(self.h, "h" in shown and not fill)
                style = e.get("style", {})
                labels = dict(menus.PICTURES.get(kind, [("image", "Image")]))
                for slot in slots:
                    self.form.labelForField(self.pictures[slot]).setText(labels[slot])
                    self._fill_picture(slot, e.get("image", "") if slot == "image"
                                       else e.get("images", {}).get(slot, ""))
                self.enabled.setChecked(e.get("enabled", True) is not False)
                self.group.setText(e.get("group", ""))
                self.text.setText(e.get("text", ""))
                self.font_size.setValue(float(style.get("font_size") or 0))
                self.align.setCurrentIndex(max(0, self.align.findData(style.get("align", ""))))
                self.bold.setChecked(bool(style.get("bold")))
                self.colour.set_value(style.get("color"))
                self.fill.set_value(style.get("background"))
                self.border.set_value(style.get("border"))
                self.border_width.setValue(float(style.get("border_width") or 0))
                self.radius.setValue(float(style.get("radius") or 0))
                action = e.get("action", {})
                self.action.setCurrentIndex(max(0, self.action.findData(action.get("do",
                                                                                   "none"))))
                self.target.clear()
                for other in self.all():
                    if other["id"] != s["id"]:
                        self.target.addItem(other["name"], other["id"])
                self.target.setCurrentIndex(max(0, self.target.findData(action.get("screen",
                                                                                   ""))))
                for key, box in (("min", self.low), ("max", self.high), ("step", self.step)):
                    box.setValue(float(e.get(key, {"max": 1}.get(key, 0))))
                self.value.setValue(float(e.get("value", e.get("min", 0)))
                                    if kind == "slider" else 0.0)
                self.ticked.setChecked(bool(e.get("value")) if kind in ("check", "option")
                                       else False)
        finally:
            self._filling = False
        self.render()

    # ---- the canvas ----------------------------------------------------------------------------
    def preview_size(self) -> tuple[int, int, float, tuple]:
        _label, w, h, scale, safe = menus.PREVIEWS[max(0, self.size_box.currentIndex())]
        return w, h, scale, safe

    def render(self, screen: dict | None = None) -> None:
        """Draws the chosen screen (or `screen`, a drag's) on the canvas, as the game would."""
        s = self.chosen_screen()
        if screen is None and s is not None:
            screen = s["screen"]
        self.canvas.chosen = self.chosen_element_id()
        if screen is None:
            self.canvas.image = None
            self.canvas.message = "Click New to make a screen." if self.isEnabled() else ""
            self.problems.clear()
            self.canvas.update()
            return
        found = menus.problems_of(screen, {o["id"] for o in self.all()})
        w, h, scale, safe = self.preview_size()
        try:
            images = menus.image_files(self.project, [{"screen": screen}])
            pixels, boxes, checks = screen_preview().render(json.dumps(screen), w, h, scale,
                                                         list(safe), scale > 1.0, images)
            self.canvas.image = QImage(pixels, w, h, QImage.Format.Format_RGBA8888).copy()
            self.canvas.boxes = {k: tuple(v) for k, v in boxes.items()}
            self.canvas.order = [e.get("id") for e in screen.get("elements", [])]
            self.canvas.dp = scale
            found += [c.split(": ", 2)[-1] for c in checks]
        except (ValueError, RuntimeError, ImportError) as e:
            self.canvas.image = None
            self.canvas.message = f"The screen can't be drawn: {e}"
        self.problems.setText("\n".join(found))
        self.canvas.update()

    def _dragged(self, element: str, dx: float, dy: float, resizing: bool) -> None:
        """A drag on the canvas: the element moves (or its corner does) by so many dp; drawn
        as it goes, one undo step when it ends."""
        s = self.chosen_screen()
        if s is None:
            return
        if self._drag_start is None:
            self._drag_start = copy.deepcopy(self.all())
            self._drag_base = copy.deepcopy(next(
                e for e in s["screen"]["elements"] if e["id"] == element))
        base = self._drag_base
        screen = copy.deepcopy(next(o["screen"] for o in self._drag_start
                                    if o["id"] == s["id"]))
        e = next(x for x in screen["elements"] if x["id"] == element)
        anchor = base.get("anchor", "top_left")
        if resizing and anchor != "fill":
            across = 0.0 if anchor.endswith("left") else 1.0 if anchor.endswith("right") else 0.5
            down = 0.0 if anchor.startswith("top") else 1.0 if anchor.startswith("bottom") \
                else 0.5
            e["w"] = max(8, base.get("w", 100) + dx)
            e["h"] = max(8, base.get("h", 40) + dy)
            # (the top left corner stays put, whatever the anchor)
            e["x"] = base.get("x", 0) + across * (e["w"] - base.get("w", 100))
            e["y"] = base.get("y", 0) + down * (e["h"] - base.get("h", 40))
        elif not resizing and anchor != "fill":
            e["x"] = base.get("x", 0) + dx
            e["y"] = base.get("y", 0) + dy
        self._dragging = (s["id"], screen)
        self.render(screen)

    def _dropped(self) -> None:
        if self._drag_start is None:
            return
        start, (sid, screen) = self._drag_start, getattr(self, "_dragging", (None, None))
        self._drag_start = self._drag_base = None
        if sid is None:
            return
        new = copy.deepcopy(start)
        for o in new:
            if o["id"] == sid:
                o["screen"] = screen
        if new != start:
            self._apply(new, f"Move '{self.chosen_element_id()}'", start=start)
        self._dragging = (None, None)

    def _show_in_game(self) -> None:
        sid = self.chosen_screen_id()
        if sid and self.show_in_game is not None:
            self.show_in_game(sid)

    # ---- changes -------------------------------------------------------------------------------
    def _apply(self, new: list[dict[str, Any]], label: str, merge_key: str = "",
               start: list[dict] | None = None) -> None:
        self.project.undo_stack.push(menus.SetScreensCommand(
            self.project, start if start is not None else self.all(), new, label, merge_key))

    def _edit_screen(self, change, label: str, merge_key: str = "") -> None:
        sid = self.chosen_screen_id()
        if self.project is None or sid is None or self._filling:
            return
        new = copy.deepcopy(self.all())
        for s in new:
            if s["id"] == sid:
                change(s)
        self._apply(new, label, merge_key)

    def _edit_element(self, change, label: str, merge_key: str = "") -> None:
        eid = self.chosen_element_id()
        if not eid:
            return

        def apply(s: dict) -> None:
            for e in s["screen"].get("elements", []):
                if e.get("id") == eid:
                    change(e)
        self._edit_screen(apply, label, merge_key)

    def new_screen(self, name: str = "") -> str | None:
        if self.project is None:
            return None
        existing = self.all()
        names = {s["name"].lower() for s in existing}
        if not name:
            name, n = "Menu", 2
            while name.lower() in names:
                name, n = f"Menu {n}", n + 1
        sid = menus.new_id({s["id"] for s in existing}, name)
        self._apply(existing + [{"id": sid, "name": name,
                                 "screen": copy.deepcopy(menus.NEW_SCREEN)}],
                    f"New screen '{name}'")
        self.choose_screen(sid)
        return sid

    def duplicate_screen(self) -> None:
        s = self.chosen_screen()
        if s is None:
            return
        existing = self.all()
        name = f"{s['name']} copy"
        sid = menus.new_id({o["id"] for o in existing}, name)
        self._apply(existing + [{"id": sid, "name": name, "screen": copy.deepcopy(s["screen"])}],
                    f"Duplicate screen '{s['name']}'")
        self.choose_screen(sid)

    def delete_screen(self) -> None:
        s = self.chosen_screen()
        if s is None:
            return
        row = self.screen_list.currentRow()
        self._apply([o for o in self.all() if o["id"] != s["id"]], f"Delete screen '{s['name']}'")
        if self.screen_list.count():
            self.screen_list.setCurrentRow(min(row, self.screen_list.count() - 1))

    def _rename_screen(self) -> None:
        s = self.chosen_screen()
        name = self.screen_name.text().strip()
        if self._filling or s is None or not name or name == s["name"]:
            if s is not None and not name:
                self.screen_name.setText(s["name"])
            return

        def change(o: dict) -> None:
            o["name"] = name
        self._edit_screen(change, f"Rename screen '{s['name']}' to '{name}'")

    def _set_screen(self, key: str, value: Any, words: str) -> None:
        def change(o: dict) -> None:
            if value is None:
                o["screen"].pop(key, None)
            else:
                o["screen"][key] = value
        self._edit_screen(change, f"Set the screen's {words}")

    def add_element(self, kind: str) -> str | None:
        s = self.chosen_screen()
        if s is None:
            return None
        eid = menus.element_id(s["screen"], kind)
        chosen = self.chosen_element()
        element = {"id": eid, **copy.deepcopy(menus.NEW_ELEMENT[kind])}
        if chosen is not None and chosen.get("kind") == "panel":
            element["parent"] = chosen["id"]
        elif chosen is not None and chosen.get("parent"):
            element["parent"] = chosen["parent"]

        def change(o: dict) -> None:
            o["screen"].setdefault("elements", []).append(element)
        self._edit_screen(change, f"Add {menus.KINDS[kind].lower()} '{eid}'")
        self.choose_element(eid)
        return eid

    def delete_element(self) -> None:
        s = self.chosen_screen()
        eid = self.chosen_element_id()
        if s is None or not eid:
            return
        gone = menus.descendants(s["screen"], eid)

        def change(o: dict) -> None:
            o["screen"]["elements"] = [e for e in o["screen"].get("elements", [])
                                       if e.get("id") not in gone]
        self._edit_screen(change, f"Delete '{eid}'")

    def move_element(self, step: int) -> None:
        """Earlier or later among its panel's (or the screen's) elements; a panel takes its
        parts with it."""
        s = self.chosen_screen()
        eid = self.chosen_element_id()
        if s is None or not eid:
            return
        elements = s["screen"].get("elements", [])
        me = next(e for e in elements if e["id"] == eid)
        siblings = [e["id"] for e in elements if e.get("parent", "") == me.get("parent", "")]
        i = siblings.index(eid)
        if not 0 <= i + step < len(siblings):
            return
        siblings[i], siblings[i + step] = siblings[i + step], siblings[i]
        parent = me.get("parent", "")

        def ordered(under: str) -> list[dict]:
            # (each element followed by everything inside it: a panel always before its parts)
            kids = [e for e in elements if e.get("parent", "") == under]
            if under == parent:
                kids.sort(key=lambda e: siblings.index(e["id"]))
            out = []
            for k in kids:
                out += [k, *ordered(k["id"])]
            return out
        order = ordered("")
        order += [e for e in elements if e not in order]  # (any whose panel is missing)

        def change(o: dict) -> None:
            o["screen"]["elements"] = order
        self._edit_screen(change, f"Move '{eid}' {'down' if step > 0 else 'up'}")
        self.choose_element(eid)

    def _rename_element(self) -> None:
        s = self.chosen_screen()
        old = self.chosen_element_id()
        new = self.element_id.text().strip()
        if self._filling or s is None or not old or not new or new == old:
            if not new:
                self.element_id.setText(old)
            return
        if any(e.get("id") == new for e in s["screen"].get("elements", [])):
            self.element_id.setText(old)
            return

        def change(o: dict) -> None:
            for e in o["screen"].get("elements", []):
                if e.get("id") == old:
                    e["id"] = new
                if e.get("parent") == old:
                    e["parent"] = new
        self._edit_screen(change, f"Rename '{old}' to '{new}'")
        self.choose_element(new)

    def _set(self, key: str, value: Any) -> None:
        if self._filling:
            return
        eid = self.chosen_element_id()

        def change(e: dict) -> None:
            if value is None:
                e.pop(key, None)
            else:
                e[key] = int(value) if isinstance(value, float) and value.is_integer() else value
        self._edit_element(change, f"Set '{eid}'s {key}", merge_key=f"{eid}:{key}")

    def _set_style(self, key: str, value: Any) -> None:
        if self._filling:
            return
        eid = self.chosen_element_id()

        def change(e: dict) -> None:
            style = e.setdefault("style", {})
            if value is None:
                style.pop(key, None)
            else:
                style[key] = value
            if not style:
                e.pop("style")
        self._edit_element(change, f"Set '{eid}'s {key.replace('_', ' ')}",
                           merge_key=f"{eid}:style:{key}")

    def _fill_picture(self, slot: str, chosen: str) -> None:
        box = self.pictures[slot]
        box.clear()
        box.addItem("None", "")
        for ref, label in sorted(self._gui_images.items(), key=lambda kv: kv[1].lower()):
            box.addItem(label, ref)
        if chosen and box.findData(chosen) < 0:
            box.addItem(f"(missing) {chosen[3:]}", chosen)
        box.setCurrentIndex(max(0, box.findData(chosen)))

    def _set_picture(self, slot: str) -> None:
        """A picture chosen: the element's (its normal one), a state's, or the screen's."""
        if self._filling:
            return
        ref = self.pictures[slot].currentData() or None
        if slot == "screen_image":
            self._set_screen("image", ref, "picture")
            return
        if slot == "image":
            self._set("image", ref)
            return
        eid = self.chosen_element_id()

        def change(e: dict) -> None:
            images = e.setdefault("images", {})
            if ref is None:
                images.pop(slot, None)
            else:
                images[slot] = ref
            if not images:
                e.pop("images")
        self._edit_element(change, f"Set '{eid}'s {slot} picture")

    def _set_parent(self) -> None:
        if self._filling:
            return
        parent = self.parent_box.currentData() or ""
        eid = self.chosen_element_id()

        def change(e: dict) -> None:
            if parent:
                e["parent"] = parent
            else:
                e.pop("parent", None)
        self._edit_element(change, f"Put '{eid}' in {parent or 'the screen'}")

    def _set_action(self) -> None:
        if self._filling:
            return
        do = self.action.currentData()
        target = self.target.currentData() if do == "open" else None
        if do == "open" and not target:
            target = next((o["id"] for o in self.all() if o["id"] != self.chosen_screen_id()),
                          None)
        eid = self.chosen_element_id()

        def change(e: dict) -> None:
            e["action"] = {"do": do, **({"screen": target} if target else {})}
        self._edit_element(change, f"Set what '{eid}' does")
