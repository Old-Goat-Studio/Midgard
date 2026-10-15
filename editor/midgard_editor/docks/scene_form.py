# SPDX-License-Identifier: Apache-2.0
"""Inspector form for a scene's settings: sky and ambient light colours (undoable)."""
from PySide6.QtGui import QColor
from PySide6.QtWidgets import QColorDialog, QFormLayout, QLabel, QPushButton, QWidget

from ..project import Project
from ..undo import SetSceneSettingsCommand
from .entity_form import style_color_button

FIELDS = {"sky": "Sky", "ambient": "Ambient light"}


class SceneSettingsForm(QWidget):
    def __init__(self, project: Project, scene_id: str, parent: QWidget | None = None):
        super().__init__(parent)
        self.project, self.scene_id = project, scene_id
        form = QFormLayout(self)
        form.addRow(QLabel("<b>Main scene</b>"))
        self.buttons: dict[str, QPushButton] = {}
        for key, label in FIELDS.items():
            button = QPushButton()
            button.clicked.connect(lambda _checked=False, k=key: self._pick(k))
            form.addRow(label, button)
            self.buttons[key] = button
        project.scene_changed.connect(self._changed)
        self.refresh()

    def _changed(self, scene_id: str) -> None:
        if scene_id == self.scene_id:
            self.refresh()

    def refresh(self) -> None:
        settings = self.project.scene_settings(self.scene_id)
        for key, button in self.buttons.items():
            style_color_button(button, settings[key])

    def _pick(self, key: str) -> None:
        rgb = self.project.scene_settings(self.scene_id)[key]
        chosen = QColorDialog.getColor(QColor.fromRgbF(*rgb[:3]), self, FIELDS[key])
        if chosen.isValid():
            self.set_color(key, [chosen.redF(), chosen.greenF(), chosen.blueF()])

    def set_color(self, key: str, rgb: list[float]) -> None:
        settings = self.project.scene_settings(self.scene_id)
        settings[key] = [round(c, 4) for c in rgb[:3]]
        self.project.undo_stack.push(SetSceneSettingsCommand(
            self.project, settings, FIELDS[key].lower(), self.scene_id))
