# SPDX-License-Identifier: Apache-2.0
"""The Terrain dock (milestone 5.9): sculpting and painting the scene's terrain.

Turn on Sculpt, choose a tool (Raise, Lower, Smooth, Flatten or Paint, with what to paint), its
radius and strength, then drag over the terrain in the viewport: each stroke is a terrain stamp
(an undoable step), drawn as you go, and the running game follows. While Sculpt is on, the left
mouse button sculpts; orbit with the right button, pan with the middle one or Shift + left.
Flatten levels to
the height where the stroke began. Clear All removes every stroke (undoable too).
"""
from typing import Any

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (
    QComboBox,
    QDoubleSpinBox,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from .. import terrain_stamps
from ..project import Project
from ..terrain_stamps import PAINTS, TOOLS, ClearStampsCommand


class TerrainPanel(QWidget):
    open_ground_textures = Signal()  # the World Types panel's Ground Textures, wanted

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.project: Project | None = None
        self.sculpt = QPushButton("Sculpt")
        self.sculpt.setCheckable(True)
        self.sculpt.setToolTip("Drag over the terrain in the viewport to sculpt or paint it (the "
                               "left button; orbit with the right one meanwhile, and pan with "
                               "the middle one or Shift + left)")
        self.tool = QComboBox()
        for key, (label, _unit, _default, _range) in TOOLS.items():
            self.tool.addItem(label, key)
        self.tool.currentIndexChanged.connect(lambda _i: self._tool_chosen())
        self.paint = QComboBox()
        for key, label in PAINTS.items():
            self.paint.addItem(label, key)
        self.radius = QDoubleSpinBox()
        self.radius.setRange(0.5, 500.0)
        self.radius.setValue(10.0)
        self.radius.setSuffix(" m")
        self.radius.setToolTip("How wide the brush is (its strength falls off to nothing there)")
        self.strength = QDoubleSpinBox()
        self.strength.setDecimals(2)
        self.strength_label = QLabel()
        self.status = QLabel()
        self.status.setWordWrap(True)
        self.clear = QPushButton("Clear All")
        self.clear.setToolTip("Remove every stroke: the terrain as generated again (undoable)")
        self.clear.clicked.connect(self.clear_all)

        form = QFormLayout()
        form.addRow("Tool", self.tool)
        form.addRow("Paint", self.paint)
        form.addRow("Radius", self.radius)
        form.addRow(self.strength_label, self.strength)
        self.form = form
        buttons = QHBoxLayout()
        buttons.addWidget(self.sculpt)
        buttons.addStretch()
        buttons.addWidget(self.clear)
        layout = QVBoxLayout(self)
        layout.addLayout(buttons)
        layout.addLayout(form)
        layout.addWidget(self.status)
        # The ground's picture textures are the world type's (6.0b): a way there from here.
        self.textures = QPushButton("Ground Textures…")
        self.textures.setToolTip("Picture textures for grass, sand, rock, snow and each biome: "
                                 "the scene's world type's (the World Types panel's Ground "
                                 "Textures tab)")
        self.textures.clicked.connect(self.open_ground_textures)
        textures_row = QHBoxLayout()
        textures_row.addWidget(self.textures)
        textures_row.addStretch()
        layout.addLayout(textures_row)
        layout.addStretch()
        self._tool_chosen()
        self.set_project(None)

    def set_project(self, project: Project | None) -> None:
        if self.project is not None:
            self.project.terrain_changed.disconnect(self._refresh)
            self.project.scene_settings_changed.disconnect(self._refresh)
        self.project = project
        if project is not None:
            project.terrain_changed.connect(self._refresh)
            project.scene_settings_changed.connect(self._refresh)
        self._refresh()

    def has_terrain(self) -> bool:
        if self.project is None:
            return False
        settings = self.project.scene_settings()
        return bool(settings.get("body")) and settings.get("terrain") is not None

    def _refresh(self, *_args) -> None:
        on = self.has_terrain()
        for w in (self.sculpt, self.tool, self.paint, self.radius, self.strength, self.clear):
            w.setEnabled(on)
        if not on:
            self.sculpt.setChecked(False)
            self.status.setText("No terrain in this scene: turn it on in World ▸ World Shape… "
                                "(bounded areas and cylinders).")
            return
        count = len(terrain_stamps.load(self.project))
        self.status.setText(f"{count} stroke{'s' if count != 1 else ''}. "
                            + ("Drag over the terrain in the viewport."
                               if self.sculpt.isChecked() else "Turn on Sculpt to draw."))
        self.clear.setEnabled(count > 0)

    def _tool_chosen(self) -> None:
        key = self.tool.currentData()
        _label, unit, default, (low, high) = TOOLS[key]
        self.strength.setRange(low, high)
        self.strength.setSingleStep(0.5 if unit == "metres" else 0.05)
        self.strength.setValue(default)
        self.strength_label.setText("Height (m)" if unit == "metres" else "Strength")
        self.form.setRowVisible(self.paint, key == "paint")

    def brush(self) -> dict[str, Any] | None:
        """The brush the viewport sculpts with, or None while Sculpt is off."""
        if not self.sculpt.isChecked() or not self.has_terrain():
            return None
        brush: dict[str, Any] = {"tool": self.tool.currentData(),
                                 "radius": round(self.radius.value(), 2),
                                 "strength": round(self.strength.value(), 3)}
        if brush["tool"] == "paint":
            brush["paint"] = self.paint.currentData()
        return brush

    def clear_all(self) -> None:
        if self.project is not None and terrain_stamps.load(self.project):
            self.project.undo_stack.push(ClearStampsCommand(self.project))


__all__ = ["TerrainPanel", "Qt"]
