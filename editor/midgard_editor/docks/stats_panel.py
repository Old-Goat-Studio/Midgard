# SPDX-License-Identifier: Apache-2.0
"""The Stats panel (milestone 6.0): the player's stats, scores and counters.

A list on the left (New, Delete, Move Up, Move Down), and the chosen one's settings on the
right: its name, its kind, and its numbers. A stat (Health, Stamina) has a least, a most, a
start, and how fast it refills by itself after how long without dropping; a score only a least
and a start; a counter a least, a start and a target. What's wrong with one is said under it.

Every change is a step on the project's undo stack (typing in one field is one step), and
reaches a game playing under the editor at once (stats.put), its values kept.
"""
import copy
from typing import Any

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QSplitter,
    QVBoxLayout,
    QWidget,
)

from .. import stats
from ..project import Project

ID_ROLE = Qt.ItemDataRole.UserRole
BIG = 1.0e9


def _spin(decimals: int = 1, low: float = -BIG, high: float = BIG,
          suffix: str = "") -> QDoubleSpinBox:
    box = QDoubleSpinBox()
    box.setRange(low, high)
    box.setDecimals(decimals)
    box.setKeyboardTracking(False)
    if suffix:
        box.setSuffix(suffix)
    return box


class StatsPanel(QWidget):
    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.project: Project | None = None
        self._filling = False

        self.list = QListWidget()
        self.list.currentItemChanged.connect(lambda *_: self._show())
        self.new_button = QPushButton("New")
        self.new_button.setToolTip("A new stat (mana, hunger), score or counter (coins, keys)")
        self.new_button.clicked.connect(lambda: self.new_stat())
        self.delete_button = QPushButton("Delete")
        self.delete_button.clicked.connect(self.delete_stat)
        self.up_button = QPushButton("Move Up")
        self.up_button.clicked.connect(lambda: self.move(-1))
        self.down_button = QPushButton("Move Down")
        self.down_button.clicked.connect(lambda: self.move(1))
        buttons = QHBoxLayout()
        for b in (self.new_button, self.delete_button, self.up_button, self.down_button):
            buttons.addWidget(b)
        left = QWidget()
        left_layout = QVBoxLayout(left)
        left_layout.setContentsMargins(0, 0, 0, 0)
        left_layout.addWidget(self.list, 1)
        left_layout.addLayout(buttons)

        self.name = QLineEdit()
        self.name.editingFinished.connect(self._rename)
        self.kind = QComboBox()
        for key, label in stats.KINDS.items():
            self.kind.addItem(label, key)
        self.kind.setToolTip("A stat stays between its least and most and can refill; a score "
                             "is a whole number of points; a counter counts up to a target")
        self.kind.activated.connect(lambda _i: self._set_kind())
        self.min = _spin()
        self.max = _spin()
        self.start = _spin()
        self.regen = _spin(2, suffix=" a second")
        self.regen.setToolTip("How much it refills by itself each second (negative: it drains; "
                              "0: it doesn't change by itself)")
        self.delay = _spin(1, 0.0, 3600.0, " s")
        self.delay.setToolTip("How long after it last went down before it starts to refill")
        for key, box in self._numbers().items():
            box.valueChanged.connect(lambda v, k=key: self._set_number(k, v))
        self.on_hud = QCheckBox("Show on the HUD")
        self.on_hud.setToolTip("A stat shows as a bar, a score or counter as a number, at the "
                               "top of the screen while the game plays")
        self.on_hud.toggled.connect(self._set_hud)
        self.id_label = QLabel()
        self.id_label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.problems = QLabel()
        self.problems.setWordWrap(True)
        self.problems.setStyleSheet("color: #d08030;")
        self.form = QFormLayout()
        self.form.addRow("Name", self.name)
        self.form.addRow("Kind", self.kind)
        self.form.addRow("Least", self.min)
        self.form.addRow("Most", self.max)
        self.form.addRow("Starts at", self.start)
        self.form.addRow("Refills", self.regen)
        self.form.addRow("After", self.delay)
        self.form.addRow("", self.on_hud)
        self.form.addRow("In scripts", self.id_label)
        right = QWidget()
        right_layout = QVBoxLayout(right)
        right_layout.addLayout(self.form)
        right_layout.addWidget(self.problems)
        right_layout.addStretch(1)
        self.editor = right

        split = QSplitter(Qt.Orientation.Horizontal)
        split.addWidget(left)
        split.addWidget(right)
        split.setStretchFactor(1, 1)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        intro = QLabel("The player's stats, scores and counters. Scripts use them as "
                       "<code>midgard.stats</code>; the HUD shows them.")
        intro.setWordWrap(True)
        layout.addWidget(intro)
        layout.addWidget(split, 1)
        self.set_project(None)

    def _numbers(self) -> dict[str, QDoubleSpinBox]:
        return {"min": self.min, "max": self.max, "start": self.start, "regen": self.regen,
                "delay": self.delay}

    # ---- the project -------------------------------------------------------------------------
    def set_project(self, project: Project | None) -> None:
        if self.project is not None:
            self.project.stats_changed.disconnect(self._changed)
        self.project = project
        if project is not None:
            project.stats_changed.connect(self._changed)
        self.setEnabled(project is not None and stats.has_table(project))
        self.refresh()

    def _changed(self, *_args) -> None:
        self.refresh()

    def all(self) -> list[dict[str, Any]]:
        return stats.load_all(self.project) if self.project is not None else []

    def refresh(self) -> None:
        chosen = self.chosen_id()
        self.list.blockSignals(True)
        self.list.clear()
        for s in self.all():
            label = s["name"]
            kind = s["stat"].get("kind", "stat")
            if kind != "stat":
                label += f"  ({stats.KINDS.get(kind, kind).lower()})"
            item = QListWidgetItem(label)
            item.setData(ID_ROLE, s["id"])
            if stats.problems_of(s["stat"]):
                item.setForeground(Qt.GlobalColor.darkYellow)
            self.list.addItem(item)
            if s["id"] == chosen:
                self.list.setCurrentItem(item)
        if self.list.currentItem() is None and self.list.count():
            self.list.setCurrentRow(0)
        self.list.blockSignals(False)
        self._show()

    def chosen_id(self) -> str | None:
        item = self.list.currentItem()
        return item.data(ID_ROLE) if item is not None else None

    def chosen(self) -> dict[str, Any] | None:
        sid = self.chosen_id()
        return next((s for s in self.all() if s["id"] == sid), None)

    def choose(self, stat_id: str) -> None:
        for i in range(self.list.count()):
            if self.list.item(i).data(ID_ROLE) == stat_id:
                self.list.setCurrentRow(i)
                return

    def _show(self) -> None:
        s = self.chosen()
        row = self.list.currentRow()
        self.editor.setEnabled(s is not None)
        self.delete_button.setEnabled(s is not None)
        self.up_button.setEnabled(s is not None and row > 0)
        self.down_button.setEnabled(s is not None and row < self.list.count() - 1)
        self._filling = True
        try:
            if s is None:
                self.name.clear()
                self.id_label.clear()
                self.problems.clear()
                return
            stat = s["stat"]
            kind = stat.get("kind", "stat")
            self.name.setText(s["name"])
            self.kind.setCurrentIndex(max(0, self.kind.findData(kind)))
            whole = kind != "stat"
            for key, box in self._numbers().items():
                box.setDecimals(0 if whole and key in ("min", "max", "start") else
                                (2 if key == "regen" else 1))
                box.setValue(stats.number(stat, key))
            # What each kind has.
            self.form.setRowVisible(self.max, kind != "score")
            self.form.setRowVisible(self.regen, kind == "stat")
            self.form.setRowVisible(self.delay, kind == "stat")
            self.form.labelForField(self.max).setText("Most" if kind == "stat" else
                                                      "Target (0: none)")
            self.on_hud.setChecked(stat.get("hud", True) is not False)
            self.id_label.setText(f'<code>midgard.stats.get("{s["id"]}")</code>')
            found = stats.problems_of(stat)
            self.problems.setText("\n".join(f"This {stats.KINDS.get(kind, 'stat').lower()} "
                                            f"{p}." for p in found))
        finally:
            self._filling = False

    # ---- changes -------------------------------------------------------------------------------
    def _apply(self, new: list[dict[str, Any]], label: str, merge_key: str = "") -> None:
        self.project.undo_stack.push(
            stats.SetStatsCommand(self.project, self.all(), new, label, merge_key))

    def _edit_chosen(self, change, label: str, merge_key: str = "") -> None:
        sid = self.chosen_id()
        if self.project is None or sid is None:
            return
        new = copy.deepcopy(self.all())
        for s in new:
            if s["id"] == sid:
                change(s)
        self._apply(new, label, merge_key)

    def new_stat(self, name: str = "", kind: str = "stat") -> str | None:
        if self.project is None:
            return None
        existing = self.all()
        names = {s["name"].lower() for s in existing}
        if not name:
            name, n = "New Stat", 2
            while name.lower() in names:
                name, n = f"New Stat {n}", n + 1
        sid = stats.new_id(existing, name)
        self._apply(existing + [{"id": sid, "name": name, "stat": copy.deepcopy(stats.NEW[kind])}],
                    f"New stat '{name}'")
        self.choose(sid)
        self.name.setFocus()
        self.name.selectAll()
        return sid

    def delete_stat(self) -> None:
        s = self.chosen()
        if s is None:
            return
        row = self.list.currentRow()
        self._apply([o for o in self.all() if o["id"] != s["id"]], f"Delete stat '{s['name']}'")
        if self.list.count():
            self.list.setCurrentRow(min(row, self.list.count() - 1))

    def move(self, step: int) -> None:
        sid = self.chosen_id()
        order = self.all()
        i = next((n for n, s in enumerate(order) if s["id"] == sid), None)
        if i is None or not 0 <= i + step < len(order):
            return
        order[i], order[i + step] = order[i + step], order[i]
        self._apply(order, f"Move stat '{order[i + step]['name']}'")
        self.choose(sid)

    def _rename(self) -> None:
        s = self.chosen()
        name = self.name.text().strip()
        if self._filling or s is None or not name or name == s["name"]:
            if s is not None and not name:
                self.name.setText(s["name"])
            return

        def change(stat: dict) -> None:
            stat["name"] = name
        self._edit_chosen(change, f"Rename stat '{s['name']}' to '{name}'")

    def _set_kind(self) -> None:
        s = self.chosen()
        kind = self.kind.currentData()
        if self._filling or s is None or kind == s["stat"].get("kind", "stat"):
            return

        def change(stat: dict) -> None:
            hud = stat["stat"].get("hud")
            stat["stat"] = copy.deepcopy(stats.NEW[kind])
            if hud is False:
                stat["stat"]["hud"] = False
        self._edit_chosen(change, f"Make '{s['name']}' a {stats.KINDS[kind].lower()}")

    def _set_hud(self, on: bool) -> None:
        s = self.chosen()
        if self._filling or s is None or (s["stat"].get("hud", True) is not False) == on:
            return

        def change(stat: dict) -> None:
            if on:
                stat["stat"].pop("hud", None)
            else:
                stat["stat"]["hud"] = False
        self._edit_chosen(change, f"{'Show' if on else 'Hide'} '{s['name']}' on the HUD")

    def _set_number(self, key: str, value: float) -> None:
        s = self.chosen()
        if self._filling or s is None:
            return
        if s["stat"].get("kind", "stat") != "stat" and key in ("min", "max", "start"):
            value = round(value)

        def change(stat: dict) -> None:
            stat["stat"][key] = int(value) if float(value).is_integer() else value
        names = {"min": "least", "max": "most", "start": "start", "regen": "refill",
                 "delay": "refill delay"}
        self._edit_chosen(change, f"Set {s['name']}'s {names[key]}",
                          merge_key=f"{s['id']}:{key}")
