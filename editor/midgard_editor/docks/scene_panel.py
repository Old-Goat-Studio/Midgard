# SPDX-License-Identifier: Apache-2.0
"""The Scene panel: the entities in the main scene, with Add Cube and Delete."""
from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (
    QHBoxLayout,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QStackedLayout,
    QVBoxLayout,
    QWidget,
)

from ..project import Project
from ..scene_model import Entity
from ..undo import AddEntityCommand, DeleteEntityCommand

ICONS = {"player": "🧍", "camera": "🎥", "light": "☀", "mesh": "■"}


def describe(entity: Entity) -> str:
    kinds = [ICONS[k] for k in ICONS if k in entity.components]
    return f"{' '.join(kinds) or '·'}  {entity.name}"


class ScenePanel(QWidget):
    entity_selected = Signal(object)  # entity id (str) or None

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.project: Project | None = None
        self.list = QListWidget()
        self.list.currentItemChanged.connect(self._selected)
        self.add_cube = QPushButton("Add Cube")
        self.add_cube.clicked.connect(self.add_cube_entity)
        self.delete = QPushButton("Delete")
        self.delete.clicked.connect(self.delete_selected)
        buttons = QHBoxLayout()
        buttons.addWidget(self.add_cube)
        buttons.addWidget(self.delete)
        buttons.addStretch()

        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(4, 4, 4, 4)
        layout.addLayout(buttons)
        layout.addWidget(self.list)
        self.empty = QLabel("No project open.")
        self.stack = QStackedLayout(self)
        self.stack.addWidget(self.empty)
        self.stack.addWidget(page)

    def set_project(self, project: Project | None) -> None:
        self.project = project
        if project is None:
            self.stack.setCurrentWidget(self.empty)
            return
        project.entities_changed.connect(self.refresh)
        self.stack.setCurrentIndex(1)
        self.refresh()

    def refresh(self, *_args) -> None:
        if self.project is None:
            return
        current = self.selected_id()
        self.list.blockSignals(True)
        self.list.clear()
        for entity in self.project.entities():
            item = QListWidgetItem(describe(entity))
            item.setData(Qt.ItemDataRole.UserRole, entity.id)
            self.list.addItem(item)
            if entity.id == current:
                self.list.setCurrentItem(item)
        self.list.blockSignals(False)
        if self.selected_id() != current:
            self.entity_selected.emit(self.selected_id())
        self.delete.setEnabled(self.selected_id() is not None)

    def selected_id(self) -> str | None:
        item = self.list.currentItem()
        return item.data(Qt.ItemDataRole.UserRole) if item else None

    def select(self, entity_id: str) -> None:
        for row in range(self.list.count()):
            if self.list.item(row).data(Qt.ItemDataRole.UserRole) == entity_id:
                self.list.setCurrentRow(row)

    def _selected(self, *_args) -> None:
        self.delete.setEnabled(self.selected_id() is not None)
        self.entity_selected.emit(self.selected_id())

    def add_cube_entity(self) -> None:
        if self.project is None:
            return
        count = sum(1 for e in self.project.entities() if e.name.startswith("Cube"))
        entity = Entity(self.project.next_entity_id(), f"Cube {count + 1}" if count else "Cube",
                        None, {"transform": {"position": [1.5 * count, 0.5, 0.0]},
                               "mesh": {"asset": "prim_cube"},
                               "material": {"color": [0.8, 0.8, 0.8, 1.0]}})
        self.project.undo_stack.push(AddEntityCommand(self.project, entity))
        self.select(entity.id)

    def delete_selected(self) -> None:
        if self.project is None or (entity_id := self.selected_id()) is None:
            return
        entity = self.project.entity(entity_id)
        if entity is not None:
            self.project.undo_stack.push(DeleteEntityCommand(self.project, entity))
