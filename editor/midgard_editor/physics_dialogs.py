# SPDX-License-Identifier: Apache-2.0
"""World ▸ World Shape… (the scene's body: which way is up and how gravity pulls) and
World ▸ Physics Layers… (the project's collision layers and which of them pass through each
other). Both are undoable."""
import json
from typing import Any

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QDoubleSpinBox,
    QFormLayout,
    QGridLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from . import engine
from .project import PHYSICS_LAYERS, Project
from .undo import SetPhysicsLayersCommand, SetSceneSettingsCommand

# Shapes and the sizes each needs, with the roadmap's limits (§7, "Size and precision"; the
# engine checks the same in midgard/body.h).
SHAPES: dict[str, tuple[str, dict[str, tuple[float, float, float]]]] = {
    "": ("Flat ground (no shape)", {}),
    "bounded_area": ("Bounded area", {"width": (1, 200_000, 2_000), "length": (1, 200_000, 2_000)}),
    "flat_cylinder": ("Flat-wrap cylinder", {"circumference": (6_000, 32_000, 6_000),
                                             "length": (1, 200_000, 2_000)}),
    "curved_cylinder": ("Inside a cylinder", {"radius": (1_000, 5_000, 1_000),
                                              "length": (5_000, 50_000, 5_000)}),
    "sphere_outside": ("Planet (outside a sphere)", {"radius": (1_000, 20_000, 1_000)}),
    "sphere_inside": ("Hollow world (inside a sphere)", {"radius": (1_000, 20_000, 1_000)}),
}


def _spin(minimum: float, maximum: float, value: float, suffix: str) -> QDoubleSpinBox:
    box = QDoubleSpinBox()
    box.setRange(minimum, maximum)
    box.setDecimals(1)
    box.setSingleStep(max(1.0, (maximum - minimum) / 100))
    box.setValue(value)
    box.setSuffix(suffix)
    return box


class WorldShapeDialog(QDialog):
    """The scene's world shape. The engine's own check (through _midgard, when it's built)
    refuses sizes it can't use before anything is saved."""

    def __init__(self, project: Project, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWindowTitle("World Shape")
        self.project = project
        body: dict[str, Any] = project.scene_settings().get("body") or {}
        layout = QVBoxLayout(self)
        intro = QLabel("Which way is up, and how gravity pulls. Applies the next time you "
                       "press Play.")
        intro.setWordWrap(True)
        layout.addWidget(intro)
        self.form = QFormLayout()
        layout.addLayout(self.form)
        self.shape = QComboBox()
        for key, (label, _sizes) in SHAPES.items():
            self.shape.addItem(label, key)
        self.form.addRow("Shape", self.shape)
        self.sizes: dict[str, QDoubleSpinBox] = {}
        for _key, (_label, sizes) in SHAPES.items():
            for name, (low, high, default) in sizes.items():
                if name not in self.sizes:
                    self.sizes[name] = _spin(low, high, float(body.get(name, default)), " m")
                    self.form.addRow(name.capitalize(), self.sizes[name])
        self.gravity = _spin(0.1, 50.0, float(body.get("gravity", 9.81)), " m/s²")
        self.form.addRow("Gravity", self.gravity)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok
                                   | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        self.shape.currentIndexChanged.connect(self._shape_chosen)
        self.shape.setCurrentIndex(max(self.shape.findData(body.get("shape", "")), 0))
        self._shape_chosen()

    def _shape_chosen(self) -> None:
        key = self.shape.currentData()
        needed = SHAPES[key][1]
        for name, box in self.sizes.items():
            self.form.setRowVisible(box, name in needed)
            if name in needed:
                low, high, _default = needed[name]
                box.setRange(low, high)
        self.form.setRowVisible(self.gravity, bool(key))

    def body(self) -> dict[str, Any] | None:
        """The body as the scene settings keep it, or None for flat ground."""
        key = self.shape.currentData()
        if not key:
            return None
        body: dict[str, Any] = {"id": "world", "shape": key,
                                "gravity": round(self.gravity.value(), 3)}
        for name in SHAPES[key][1]:
            body[name] = round(self.sizes[name].value(), 1)
        return body

    def accept(self) -> None:
        body = self.body()
        midgard = engine.module()
        if body is not None and midgard is not None:
            try:
                midgard.Body(json.dumps(body))
            except ValueError as e:
                QMessageBox.warning(self, "World Shape", str(e))
                return
        settings = self.project.scene_settings()
        if body is None:
            settings.pop("body", None)
        else:
            settings["body"] = body
        if settings != self.project.scene_settings():
            self.project.undo_stack.push(
                SetSceneSettingsCommand(self.project, settings, "world shape"))
        super().accept()


class PhysicsLayersDialog(QDialog):
    """Names for the 16 collision layers, and a grid of the pairs that collide (ticked)."""

    def __init__(self, project: Project, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWindowTitle("Physics Layers")
        self.project = project
        layout = QVBoxLayout(self)
        intro = QLabel("A rigid body is on one layer. Untick a pair to let those layers pass "
                       "through each other. Applies the next time you press Play.")
        intro.setWordWrap(True)
        layout.addWidget(intro)
        names = project.physics_layers()
        grid = QGridLayout()
        self.names: list[QLineEdit] = []
        for i in range(PHYSICS_LAYERS):
            edit = QLineEdit(names[i])
            edit.setPlaceholderText("(not used)" if i else "default")
            edit.editingFinished.connect(self._fill_table)
            grid.addWidget(QLabel(str(i)), i % 8, (i // 8) * 2)
            grid.addWidget(edit, i % 8, (i // 8) * 2 + 1)
            self.names.append(edit)
        layout.addLayout(grid)
        self.table = QTableWidget()
        layout.addWidget(self.table)
        self._ignored = project.physics_ignored()
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok
                                   | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        self._fill_table()

    def used(self) -> list[int]:
        return [i for i, e in enumerate(self.names) if e.text().strip() or i == 0]

    def _read_table(self) -> None:
        layers = getattr(self, "_shown", [])
        for r, a in enumerate(layers):
            for c, b in enumerate(layers):
                item = self.table.item(r, c)
                if item is None or c < r:
                    continue
                pair = (min(a, b), max(a, b))
                if item.checkState() == Qt.CheckState.Checked:
                    self._ignored.discard(pair)
                else:
                    self._ignored.add(pair)

    def _fill_table(self) -> None:
        self._read_table()
        self._shown = self.used()
        labels = [self.names[i].text().strip() or "default" for i in self._shown]
        self.table.clear()
        self.table.setRowCount(len(self._shown))
        self.table.setColumnCount(len(self._shown))
        self.table.setHorizontalHeaderLabels(labels)
        self.table.setVerticalHeaderLabels(labels)
        for r, a in enumerate(self._shown):
            for c, b in enumerate(self._shown):
                if c < r:
                    continue  # the grid is symmetric: each pair once
                item = QTableWidgetItem()
                item.setFlags(Qt.ItemFlag.ItemIsUserCheckable | Qt.ItemFlag.ItemIsEnabled)
                collides = (min(a, b), max(a, b)) not in self._ignored
                item.setCheckState(Qt.CheckState.Checked if collides else Qt.CheckState.Unchecked)
                item.setToolTip(f"{labels[r]} and {labels[c]} collide")
                self.table.setItem(r, c, item)
        self.table.resizeColumnsToContents()

    def accept(self) -> None:
        self._read_table()
        names = [e.text().strip() for e in self.names]
        used = [n for n in names if n]
        if len(set(used)) != len(used):
            QMessageBox.warning(self, "Physics Layers", "Each layer needs its own name.")
            return
        ignored = {p for p in self._ignored if names[p[0]] or p[0] == 0}
        ignored = {p for p in ignored if names[p[1]] or p[1] == 0}
        if (names, ignored) != (self.project.physics_layers(), self.project.physics_ignored()):
            self.project.undo_stack.push(SetPhysicsLayersCommand(self.project, names, ignored))
        super().accept()
