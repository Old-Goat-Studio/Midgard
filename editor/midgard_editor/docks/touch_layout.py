# SPDX-License-Identifier: Apache-2.0
"""The Input editor's Touch Layout (milestone 6.1 step 3): the on-screen buttons a phone shows.

On the left a phone, held sideways or upright, with the stick where it rests (bottom left: a
thumb anywhere on the left half moves it), the right half for dragging to look, and the buttons
where they sit: drag one to move it (one undo step a drag). On the right the buttons, and the
chosen one's label, size, whether a tap turns it on and off (Run), and when it shows: always, in
a context (Down while flying), or when the game has a pause menu (Menu opens it). Add Button
makes one for one of the game's actions and binds it; Remove takes one away (and its
bindings); Midgard's Layout puts Midgard's own back. Places are dp in from the safe area's
bottom right; held upright, each button has a place of its own.
"""
import copy
from typing import Any

from PySide6.QtCore import QPointF, QRectF, Qt, Signal
from PySide6.QtGui import QColor, QFont, QPainter, QPen
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMenu,
    QPushButton,
    QSpinBox,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

from .. import input_actions as ia
from .. import menus
from ..project import Project

SIDEWAYS = (844.0, 390.0)  # dp: a phone held sideways (the safe area's)
UPRIGHT = (390.0, 844.0)
STICK = {"sideways": (110.0, 110.0), "upright": (96.0, 110.0)}  # rests in from bottom left
STICK_RADIUS = 60.0


class PhoneCanvas(QWidget):
    """The phone, its stick and its buttons; buttons dragged to move them."""
    chosen = Signal(str)          # a button's id ("" none)
    moved = Signal(str, float, float)  # a drag ended: the button, its new place (dp)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.buttons: list[dict[str, Any]] = []
        self.upright = False
        self.selected = ""
        self._drag: tuple[str, QPointF, float, float] | None = None
        self.setMinimumSize(420, 300)
        self.setMouseTracking(True)

    def size_dp(self) -> tuple[float, float]:
        return UPRIGHT if self.upright else SIDEWAYS

    def frame(self) -> tuple[QRectF, float]:
        """The phone's screen on the canvas, and pixels a dp."""
        w, h = self.size_dp()
        scale = min((self.width() - 20) / w, (self.height() - 20) / h)
        rect = QRectF((self.width() - w * scale) / 2, (self.height() - h * scale) / 2,
                      w * scale, h * scale)
        return rect, scale

    def place(self, b: dict[str, Any]) -> tuple[float, float, float]:
        """A button's x, y (dp in from bottom right) and radius, for the way it's held."""
        if self.upright:
            n = b.get("narrow") or [b["x"], b["y"], b["r"]]
            return float(n[0]), float(n[1]), float(n[2])
        return float(b["x"]), float(b["y"]), float(b["r"])

    def centre(self, b: dict[str, Any]) -> tuple[QPointF, float]:
        rect, scale = self.frame()
        x, y, r = self.place(b)
        return QPointF(rect.right() - x * scale, rect.bottom() - y * scale), r * scale

    def button_at(self, p: QPointF) -> str:
        for b in reversed(self.buttons):
            c, r = self.centre(b)
            if (p - c).manhattanLength() <= r * 1.4 and \
                    (p.x() - c.x()) ** 2 + (p.y() - c.y()) ** 2 <= r * r:
                return b["id"]
        return ""

    def paintEvent(self, _e) -> None:  # noqa: N802 (Qt's name)
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        rect, scale = self.frame()
        painter.fillRect(self.rect(), QColor(40, 42, 48))
        painter.setPen(QPen(QColor(15, 15, 18), 6))
        painter.setBrush(QColor(70, 92, 120))
        painter.drawRoundedRect(rect.adjusted(-6, -6, 6, 6), 14, 14)
        painter.fillRect(rect, QColor(96, 128, 96))
        # The right half: dragging looks.
        painter.fillRect(QRectF(rect.center().x(), rect.top(), rect.width() / 2, rect.height()),
                         QColor(255, 255, 255, 18))
        painter.setPen(QColor(255, 255, 255, 110))
        painter.drawText(QRectF(rect.center().x(), rect.top() + 6, rect.width() / 2, 20),
                         Qt.AlignmentFlag.AlignCenter, "drag here to look")
        # The stick, resting.
        sx, sy = STICK["upright" if self.upright else "sideways"]
        stick = QPointF(rect.left() + sx * scale, rect.bottom() - sy * scale)
        painter.setPen(QPen(QColor(255, 255, 255, 140), 2))
        painter.setBrush(QColor(16, 20, 28, 70))
        painter.drawEllipse(stick, STICK_RADIUS * scale, STICK_RADIUS * scale)
        painter.setBrush(QColor(255, 255, 255, 80))
        painter.drawEllipse(stick, 26 * scale, 26 * scale)
        font = QFont(self.font())
        for b in self.buttons:
            c, r = self.centre(b)
            always = b.get("shows", "always") == "always"
            pen = QPen(QColor(255, 210, 122) if b["id"] == self.selected
                       else QColor(255, 255, 255, 200), 3 if b["id"] == self.selected else 2)
            if not always:
                pen.setStyle(Qt.PenStyle.DashLine)  # (shows only sometimes)
            painter.setPen(pen)
            painter.setBrush(QColor(16, 20, 28, 150))
            painter.drawEllipse(c, r, r)
            font.setPixelSize(max(9, int(r * 0.42)))
            painter.setFont(font)
            painter.setPen(QColor(255, 255, 255))
            painter.drawText(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r),
                             Qt.AlignmentFlag.AlignCenter, b.get("label", b["id"]))

    def mousePressEvent(self, e) -> None:  # noqa: N802 (Qt's name)
        found = self.button_at(e.position())
        self.selected = found
        self.chosen.emit(found)
        if found:
            b = next(b for b in self.buttons if b["id"] == found)
            x, y, _r = self.place(b)
            self._drag = (found, e.position(), x, y)
        self.update()

    def mouseMoveEvent(self, e) -> None:  # noqa: N802 (Qt's name)
        if self._drag is None:
            return
        found, start, x0, y0 = self._drag
        _rect, scale = self.frame()
        d = (e.position() - start) / scale
        w, h = self.size_dp()
        x = min(max(x0 - d.x(), 10.0), w - 10.0)
        y = min(max(y0 - d.y(), 10.0), h - 10.0)
        for b in self.buttons:
            if b["id"] == found:
                if self.upright:
                    n = list(b.get("narrow") or [b["x"], b["y"], b["r"]])
                    b["narrow"] = [round(x, 1), round(y, 1), n[2]]
                else:
                    b["x"], b["y"] = round(x, 1), round(y, 1)
        self.update()

    def mouseReleaseEvent(self, e) -> None:  # noqa: N802 (Qt's name)
        if self._drag is None:
            return
        found, start, _x0, _y0 = self._drag
        self._drag = None
        if (e.position() - start).manhattanLength() < 2:
            return  # (a click: chosen, not moved)
        b = next(b for b in self.buttons if b["id"] == found)
        x, y, _r = self.place(b)
        self.moved.emit(found, x, y)


class TouchLayoutEditor(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.project: Project | None = None
        self.layout_now: list[dict[str, Any]] = []
        self._filling = False
        outer = QHBoxLayout(self)
        left = QVBoxLayout()
        held = QHBoxLayout()
        held.addWidget(QLabel("Held"))
        self.held = QComboBox()
        self.held.addItem("Sideways", False)
        self.held.addItem("Upright", True)
        self.held.currentIndexChanged.connect(lambda _i: self._held_changed())
        held.addWidget(self.held)
        held.addStretch(1)
        left.addLayout(held)
        self.canvas = PhoneCanvas()
        self.canvas.chosen.connect(self.choose)
        self.canvas.moved.connect(self._moved)
        left.addWidget(self.canvas, 1)
        note = QLabel("Drag a button to move it. The stick (bottom left) and dragging to look "
                      "(the right half) are always there. Dashed: it shows only sometimes.")
        note.setWordWrap(True)
        left.addWidget(note)
        outer.addLayout(left, 3)

        right = QVBoxLayout()
        self.list = QListWidget()
        self.list.currentRowChanged.connect(lambda _r: self._list_chosen())
        right.addWidget(self.list, 1)
        row = QHBoxLayout()
        self.add_button = QToolButton()
        self.add_button.setText("Add Button")
        self.add_button.setPopupMode(QToolButton.ToolButtonPopupMode.InstantPopup)
        self.add_menu = QMenu(self.add_button)
        self.add_menu.aboutToShow.connect(self._fill_add_menu)
        self.add_button.setMenu(self.add_menu)
        row.addWidget(self.add_button)
        self.remove_button = QPushButton("Remove")
        self.remove_button.clicked.connect(lambda: self.remove())
        row.addWidget(self.remove_button)
        right.addLayout(row)
        form = QFormLayout()
        self.label = QLineEdit()
        self.label.editingFinished.connect(lambda: self._set("label", self.label.text().strip()))
        form.addRow("Label", self.label)
        self.size = QSpinBox()
        self.size.setRange(24, 240)
        self.size.setSuffix(" dp across")
        self.size.setToolTip("How big it is (a fingertip is about 50 dp)")
        self.size.editingFinished.connect(lambda: self._set_size(self.size.value()))
        form.addRow("Size", self.size)
        self.toggles = QCheckBox("A tap turns it on, another off (as Run)")
        self.toggles.toggled.connect(lambda on: self._set("toggles", on))
        form.addRow("", self.toggles)
        self.shows = QComboBox()
        for key, label in ia.TOUCH_SHOWS.items():
            self.shows.addItem(label, key)
        self.shows.activated.connect(lambda _i: self._set("shows", self.shows.currentData()))
        form.addRow("Shows", self.shows)
        self.does = QLabel()
        self.does.setWordWrap(True)
        form.addRow("Does", self.does)
        right.addLayout(form)
        self.reset_button = QPushButton("Midgard's Layout")
        self.reset_button.setToolTip("Midgard's own buttons and places again")
        self.reset_button.clicked.connect(lambda: self.reset())
        right.addWidget(self.reset_button)
        self.problems = QLabel()
        self.problems.setWordWrap(True)
        self.problems.setStyleSheet("color: #c0392b")
        right.addWidget(self.problems)
        outer.addLayout(right, 2)

    # ---- the project ---------------------------------------------------------------------------
    def set_project(self, project: Project | None) -> None:
        if self.project is not None:
            self.project.game_settings_changed.disconnect(self.reload)
            self.project.input_changed.disconnect(self._input_changed)
        self.project = project
        if project is not None:
            project.game_settings_changed.connect(self.reload)
            project.input_changed.connect(self._input_changed)
        self.reload()

    def _input_changed(self, _id: str) -> None:
        self.show_button()

    def reload(self) -> None:
        if self._filling:
            return
        self.layout_now = ia.touch_layout(self.project) if self.project is not None else []
        keep = self.canvas.selected
        self.canvas.buttons = copy.deepcopy(self.layout_now)
        self.list.blockSignals(True)
        self.list.clear()
        for b in self.layout_now:
            item = QListWidgetItem(b.get("label", b["id"]))
            item.setData(Qt.ItemDataRole.UserRole, b["id"])
            self.list.addItem(item)
        self.list.blockSignals(False)
        self.choose(keep if any(b["id"] == keep for b in self.layout_now) else "")
        self.problems.setText("\n".join(ia.touch_problems(self.project))
                              if self.project is not None else "")

    def current(self) -> dict[str, Any] | None:
        return next((b for b in self.layout_now if b["id"] == self.canvas.selected), None)

    def choose(self, button_id: str) -> None:
        self.canvas.selected = button_id
        self.canvas.update()
        self.list.blockSignals(True)
        for i in range(self.list.count()):
            if self.list.item(i).data(Qt.ItemDataRole.UserRole) == button_id:
                self.list.setCurrentRow(i)
        if not button_id:
            self.list.setCurrentRow(-1)
        self.list.blockSignals(False)
        self.show_button()

    def _list_chosen(self) -> None:
        item = self.list.currentItem()
        self.choose(item.data(Qt.ItemDataRole.UserRole) if item else "")

    def show_button(self) -> None:
        b = self.current()
        for w in (self.label, self.size, self.toggles, self.shows, self.remove_button):
            w.setEnabled(b is not None)
        self._filling = True
        try:
            if b is None:
                self.label.clear()
                self.does.clear()
                return
            self.label.setText(b.get("label", b["id"]))
            _x, _y, r = self.canvas.place(b)
            self.size.setValue(int(round(2 * r)))
            self.toggles.setChecked(bool(b.get("toggles")))
            self.shows.setCurrentIndex(max(0, self.shows.findData(b.get("shows", "always"))))
            doers = [a["name"] for a in ia.load_all(self.project) for x in a["bindings"]
                     if x["device"] == "touch" and x["control"] == b["id"]] \
                if self.project is not None else []
            self.does.setText(", ".join(doers) if doers else
                              "(no action: Menu opens the pause menu)"
                              if b.get("shows") == "pause_menu" else "(nothing yet)")
        finally:
            self._filling = False

    def _held_changed(self) -> None:
        self.canvas.upright = bool(self.held.currentData())
        self.canvas.update()
        self.show_button()

    # ---- changes (each one undo step) ---------------------------------------------------------
    def _save(self, layout: list[dict[str, Any]], text: str) -> None:
        if self.project is None:
            return
        settings = menus.game_settings(self.project)
        settings["touch"] = {"buttons": layout}
        self.project.undo_stack.push(menus.SetGameSettingsCommand(self.project, settings, text))

    def _changed(self, button_id: str, change, text: str) -> None:
        layout = copy.deepcopy(self.layout_now)
        for b in layout:
            if b["id"] == button_id:
                change(b)
        if layout != self.layout_now:
            self._save(layout, text)

    def _moved(self, button_id: str, x: float, y: float) -> None:
        upright = self.canvas.upright

        def move(b):
            if upright:
                n = list(b.get("narrow") or [b["x"], b["y"], b["r"]])
                b["narrow"] = [round(x, 1), round(y, 1), n[2]]
            else:
                b["x"], b["y"] = round(x, 1), round(y, 1)
        self._changed(button_id, move, f"Move the {button_id} touch button")

    def _set(self, key: str, value) -> None:
        b = self.current()
        if self._filling or b is None or b.get(key) == value or (key == "label" and not value):
            return
        self._changed(b["id"], lambda x: x.__setitem__(key, value),
                      f"Change the {b['id']} touch button")

    def _set_size(self, size: int) -> None:
        b = self.current()
        if self._filling or b is None:
            return
        r = round(size / 2.0, 1)
        upright = self.canvas.upright

        def resize(x):
            if upright:
                n = list(x.get("narrow") or [x["x"], x["y"], x["r"]])
                x["narrow"] = [n[0], n[1], r]
            else:
                x["r"] = r
        self._changed(b["id"], resize, f"Resize the {b['id']} touch button")

    def _fill_add_menu(self) -> None:
        self.add_menu.clear()
        if self.project is None:
            return
        have = {b["id"] for b in self.layout_now}
        bound = {(a["id"], x["control"]) for a in ia.load_all(self.project)
                 for x in a["bindings"] if x["device"] == "touch"}
        for a in ia.load_all(self.project):
            if a["kind"] != "button" or a["id"] in ("menu",):
                continue
            act = self.add_menu.addAction(a["name"], lambda i=a["id"]: self.add_for(i))
            act.setEnabled(a["id"] not in have and not any(x == a["id"] for x, _c in bound))

    def add_for(self, action_id: str) -> str | None:
        """A button for an action, bound to it (one undo step)."""
        if self.project is None:
            return None
        action = ia.load(self.project, action_id)
        if action is None:
            return None
        taken = {b["id"] for b in self.layout_now}
        button_id, n = action_id, 2
        while button_id in taken:
            button_id, n = f"{action_id}_{n}", n + 1
        button = {"id": button_id, "label": action["name"][:12], "x": 280, "y": 96, "r": 30,
                  "narrow": [250, 330, 28], "toggles": False,
                  "shows": action["context"] if action["context"] in ia.TOUCH_SHOWS
                  else "always"}
        stack = self.project.undo_stack
        stack.beginMacro(f"Add a touch button for '{action['name']}'")
        try:
            self._save(copy.deepcopy(self.layout_now) + [button],
                       f"Add a touch button for '{action['name']}'")
            new = copy.deepcopy(action)
            new["bindings"].append({"device": "touch", "control": button_id, "scale": 1.0})
            stack.push(ia.SetActionCommand(self.project, action, new,
                                           f"Bind the touch button to '{action['name']}'"))
        finally:
            stack.endMacro()
        self.reload()
        self.choose(button_id)
        return button_id

    def remove(self) -> bool:
        """The chosen button gone, and actions' bindings to it (one undo step)."""
        b = self.current()
        if self.project is None or b is None:
            return False
        stack = self.project.undo_stack
        stack.beginMacro(f"Remove the {b.get('label', b['id'])} touch button")
        try:
            self._save([x for x in copy.deepcopy(self.layout_now) if x["id"] != b["id"]],
                       "Remove a touch button")
            for a in ia.load_all(self.project):
                kept = [x for x in a["bindings"]
                        if not (x["device"] == "touch" and x["control"] == b["id"])]
                if len(kept) != len(a["bindings"]):
                    stack.push(ia.SetActionCommand(self.project, a, {**a, "bindings": kept},
                                                   "Unbind the touch button"))
        finally:
            stack.endMacro()
        self.reload()
        return True

    def reset(self) -> None:
        if self.project is None:
            return
        settings = menus.game_settings(self.project)
        if "touch" not in settings:
            return
        settings.pop("touch")
        self.project.undo_stack.push(menus.SetGameSettingsCommand(
            self.project, settings, "Midgard's touch layout again"))
