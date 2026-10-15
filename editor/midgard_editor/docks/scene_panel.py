# SPDX-License-Identifier: Apache-2.0
"""The Scene panel: the main scene (its settings) and its entities, with Add and Delete."""
from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (
    QHBoxLayout,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QMenu,
    QPushButton,
    QStackedLayout,
    QVBoxLayout,
    QWidget,
)

from ..meshes import PRIMITIVES
from ..project import Project
from ..scene_model import MAIN_SCENE, Entity
from ..undo import AddEntityCommand, DeleteEntityCommand

ICONS = {"player": "🧍", "camera": "🎥", "light": "☀", "mesh": "■"}
SCENE_PREFIX = "scene:"  # the list's first row is the scene itself


def describe(entity: Entity) -> str:
    kinds = [ICONS[k] for k in ICONS if k in entity.components]
    return f"{' '.join(kinds) or '·'}  {entity.name}"


class ScenePanel(QWidget):
    entity_selected = Signal(object)  # entity id (str) or None
    scene_selected = Signal(str)  # the scene row was chosen: show its settings

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.project: Project | None = None
        self.list = QListWidget()
        self.list.currentItemChanged.connect(self._selected)
        self.add = QPushButton("Add")
        add_menu = QMenu(self.add)
        for asset, label in PRIMITIVES.items():
            add_menu.addAction(label, lambda a=asset: self.add_primitive(a))
        self.add.setMenu(add_menu)
        self.delete = QPushButton("Delete")
        self.delete.clicked.connect(self.delete_selected)
        buttons = QHBoxLayout()
        buttons.addWidget(self.add)
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
        scene_item = QListWidgetItem("⛰  Main scene")
        scene_item.setData(Qt.ItemDataRole.UserRole, SCENE_PREFIX + MAIN_SCENE)
        scene_item.setToolTip("The scene's settings: sky and ambient light")
        self.list.addItem(scene_item)
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
        """The selected entity's id (None for nothing, or for the scene row)."""
        item = self.list.currentItem()
        key = item.data(Qt.ItemDataRole.UserRole) if item else None
        return None if key is None or key.startswith(SCENE_PREFIX) else key

    def select_scene(self) -> None:
        self.list.setCurrentRow(0)

    def select(self, entity_id: str) -> None:
        for row in range(self.list.count()):
            if self.list.item(row).data(Qt.ItemDataRole.UserRole) == entity_id:
                self.list.setCurrentRow(row)

    def _selected(self, *_args) -> None:
        self.delete.setEnabled(self.selected_id() is not None)
        item = self.list.currentItem()
        key = item.data(Qt.ItemDataRole.UserRole) if item else None
        if key and key.startswith(SCENE_PREFIX):
            self.entity_selected.emit(None)
            self.scene_selected.emit(key[len(SCENE_PREFIX):])
        else:
            self.entity_selected.emit(self.selected_id())

    def add_primitive(self, asset: str) -> None:
        """Adds an entity showing one of the editor's primitives (Add ▸ Cube, Sphere, Plane)."""
        if self.project is None:
            return
        label = PRIMITIVES[asset]
        count = sum(1 for e in self.project.entities() if e.name.startswith(label))
        entity = Entity(self.project.next_entity_id(),
                        f"{label} {count + 1}" if count else label, None,
                        {"transform": {"position": [1.5 * count, 0.0 if asset == "prim_plane"
                                                    else 0.5, 0.0]},
                         "mesh": {"asset": asset},
                         "material": {"color": [0.8, 0.8, 0.8, 1.0]}})
        self.project.undo_stack.push(AddEntityCommand(self.project, entity))
        self.select(entity.id)

    def add_cube_entity(self) -> None:
        self.add_primitive("prim_cube")

    def delete_selected(self) -> None:
        if self.project is None or (entity_id := self.selected_id()) is None:
            return
        entity = self.project.entity(entity_id)
        if entity is not None:
            self.project.undo_stack.push(DeleteEntityCommand(self.project, entity))
