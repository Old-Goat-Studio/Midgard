# SPDX-License-Identifier: Apache-2.0
"""The Scene panel: the main scene's entities as a tree, with folders (a new scene keeps its
camera, sky, light, ground and player in a World folder) and groups (several parts that move as
one, see schema/components.md). Add, Delete (the button, the Delete key or right-click), and drag
an entity onto a folder or a group (or beside another entity) to move it there: it stays where
it is in the world."""
import contextlib

from PySide6.QtCore import QPoint, Qt, Signal
from PySide6.QtGui import QDropEvent, QKeyEvent
from PySide6.QtWidgets import (
    QAbstractItemView,
    QHBoxLayout,
    QLabel,
    QMenu,
    QPushButton,
    QStackedLayout,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

from .. import transforms
from ..meshes import PRIMITIVES
from ..project import Project
from ..scene_model import Entity, descendants, parents_first, sky_component
from ..undo import AddEntityCommand, DeleteEntityCommand, SetEntityCommand

ICONS = {"folder": "📁", "group": "🧩", "player": "🧍", "camera": "🎥", "light": "☀", "sky": "🌌",
         "mesh": "■"}
# These place themselves in the world, so they stay out of groups.
NOT_IN_GROUPS = ("player", "camera")
ID_ROLE = Qt.ItemDataRole.UserRole


def describe(entity: Entity) -> str:
    kinds = [ICONS[k] for k in ICONS if k in entity.components]
    return f"{' '.join(kinds) or '·'}  {entity.name}"


class _SceneTree(QTreeWidget):
    """A tree whose drops don't move items themselves: they ask for a new parent (moved), and
    the panel rebuilds from the project."""

    moved = Signal(str, object)  # entity id, new parent id (None: the top of the scene)
    delete_pressed = Signal()

    def __init__(self):
        super().__init__()
        self.setHeaderHidden(True)
        self.setDragDropMode(QAbstractItemView.DragDropMode.InternalMove)
        self.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self.folders: set[str] = set()  # ids of the entities others can go into (folders, groups)

    def drop_parent(self, target: QTreeWidgetItem | None,
                    position: QAbstractItemView.DropIndicatorPosition) -> tuple[bool, str | None]:
        """Where a drop lands: (allowed, new parent id). Onto a folder or a group goes inside
        it, beside an entity goes next to it (same parent), onto any other entity is refused."""
        if target is None:
            return True, None
        target_id = target.data(0, ID_ROLE)
        if position == QAbstractItemView.DropIndicatorPosition.OnItem:
            return (True, target_id) if target_id in self.folders else (False, None)
        parent = target.parent()
        return True, parent.data(0, ID_ROLE) if parent is not None else None

    def keyPressEvent(self, event: QKeyEvent) -> None:
        if event.key() == Qt.Key.Key_Delete:
            self.delete_pressed.emit()
            event.accept()
            return
        super().keyPressEvent(event)

    def dropEvent(self, event: QDropEvent) -> None:
        item = self.currentItem()
        target = self.itemAt(event.position().toPoint())
        allowed, parent = self.drop_parent(target, self.dropIndicatorPosition())
        event.setDropAction(Qt.DropAction.IgnoreAction)  # the tree is rebuilt instead
        event.accept()
        if item is not None and allowed:
            self.moved.emit(item.data(0, ID_ROLE), parent)


class ScenePanel(QWidget):
    entity_selected = Signal(object)  # entity id (str) or None

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.project: Project | None = None
        self.collapsed: set[str] = set()  # folders the user closed, kept across refreshes
        self.tree = _SceneTree()
        self.tree.currentItemChanged.connect(self._selected)
        self.tree.moved.connect(self.move_entity)
        self.tree.itemCollapsed.connect(lambda i: self.collapsed.add(i.data(0, ID_ROLE)))
        self.tree.itemExpanded.connect(lambda i: self.collapsed.discard(i.data(0, ID_ROLE)))
        self.add = QPushButton("Add")
        add_menu = QMenu(self.add)
        add_menu.addAction("📁  Folder", self.add_folder)
        self.add_sky_action = add_menu.addAction("🌌  Sky", self.add_sky)
        add_menu.addSeparator()
        for asset, label in PRIMITIVES.items():
            add_menu.addAction(label, lambda a=asset: self.add_primitive(a))
        add_menu.aboutToShow.connect(self._update_add_menu)
        self.add.setMenu(add_menu)
        add_menu.insertAction(self.add_sky_action, add_menu.addAction("🧩  Group", self.add_group))
        self.delete = QPushButton("Delete")
        self.delete.setToolTip("Delete the selected entity and what's inside it (Delete key)")
        self.delete.clicked.connect(self.delete_selected)
        # The Delete key, and a right-click menu, do the same while the tree has the keyboard.
        self.tree.delete_pressed.connect(self.delete_selected)
        self.tree.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.tree.customContextMenuRequested.connect(self._context_menu)
        buttons = QHBoxLayout()
        buttons.addWidget(self.add)
        buttons.addWidget(self.delete)
        buttons.addStretch()

        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(4, 4, 4, 4)
        layout.addLayout(buttons)
        layout.addWidget(self.tree)
        self.empty = QLabel("No project open.")
        self.stack = QStackedLayout(self)
        self.stack.addWidget(self.empty)
        self.stack.addWidget(page)

    def set_project(self, project: Project | None) -> None:
        self.project = project
        self.collapsed.clear()
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
        self.tree.blockSignals(True)
        self.tree.clear()
        entities = self.project.entities()
        self.tree.folders = {e.id for e in entities if e.is_folder or "group" in e.components}
        items: dict[str, QTreeWidgetItem] = {}
        for entity in parents_first(entities):
            parent = items.get(entity.parent) if entity.parent else None
            item = QTreeWidgetItem(parent if parent is not None else self.tree)
            item.setText(0, describe(entity))
            item.setData(0, ID_ROLE, entity.id)
            flags = item.flags() | Qt.ItemFlag.ItemIsDragEnabled
            if entity.id in self.tree.folders:
                flags |= Qt.ItemFlag.ItemIsDropEnabled
            else:
                flags &= ~Qt.ItemFlag.ItemIsDropEnabled
            item.setFlags(flags)
            items[entity.id] = item
        for entity_id, item in items.items():
            item.setExpanded(entity_id not in self.collapsed)
        if current in items:
            self.tree.setCurrentItem(items[current])
        self.tree.blockSignals(False)
        if self.selected_id() != current:
            self.entity_selected.emit(self.selected_id())
        self.delete.setEnabled(self.selected_id() is not None)

    def selected_id(self) -> str | None:
        item = self.tree.currentItem()
        return item.data(0, ID_ROLE) if item else None

    def select(self, entity_id: str) -> None:
        for item in self._all_items():
            if item.data(0, ID_ROLE) == entity_id:
                self.tree.setCurrentItem(item)

    def _all_items(self) -> list[QTreeWidgetItem]:
        out, stack = [], [self.tree.topLevelItem(i) for i in range(self.tree.topLevelItemCount())]
        while stack:
            item = stack.pop()
            out.append(item)
            stack += [item.child(i) for i in range(item.childCount())]
        return out

    def _selected(self, *_args) -> None:
        self.delete.setEnabled(self.selected_id() is not None)
        self.entity_selected.emit(self.selected_id())

    def context_menu(self, item: QTreeWidgetItem) -> QMenu:
        """The right-click menu for an entity (which becomes the selection)."""
        self.tree.setCurrentItem(item)
        menu = QMenu(self.tree)
        menu.addAction("Delete", self.delete_selected)
        return menu

    def _context_menu(self, point: QPoint) -> None:
        item = self.tree.itemAt(point)
        if item is not None:
            self.context_menu(item).exec(self.tree.viewport().mapToGlobal(point))

    def _update_add_menu(self) -> None:
        has_sky = self.project is not None and any(
            "sky" in e.components for e in self.project.entities())
        self.add_sky_action.setEnabled(not has_sky)  # one sky per scene

    # ---- adding, moving and deleting -----------------------------------------------------
    def target_folder(self) -> str | None:
        """Where new entities go: the selected folder, or the selected entity's folder."""
        if self.project is None or (selected := self.selected_id()) is None:
            return None
        entity = self.project.entity(selected)
        if entity is None:
            return None
        return entity.id if entity.is_folder or "group" in entity.components else entity.parent

    def _add(self, entity: Entity) -> None:
        self.project.undo_stack.push(AddEntityCommand(self.project, entity))
        self.select(entity.id)

    def _unique_name(self, label: str) -> tuple[str, int]:
        count = sum(1 for e in self.project.entities() if e.name.startswith(label))
        return (f"{label} {count + 1}" if count else label), count

    def add_folder(self) -> None:
        if self.project is None:
            return
        name, _count = self._unique_name("Folder")
        self._add(Entity(self.project.next_entity_id(), name, self.target_folder(),
                         {"folder": {}}))

    def add_group(self) -> None:
        """Adds an empty group: drag parts into it, then move it as one thing."""
        if self.project is None:
            return
        name, _count = self._unique_name("Group")
        self._add(Entity(self.project.next_entity_id(), name, self.target_folder(),
                         {"transform": {"position": [0.0, 0.0, 0.0]}, "group": {}}))

    def add_sky(self) -> None:
        if self.project is None:
            return
        self._add(Entity(self.project.next_entity_id(), "Sky", self.target_folder(),
                         {"sky": sky_component(self.project.scene_settings())}))

    def add_primitive(self, asset: str) -> None:
        """Adds an entity showing one of the editor's primitives (Add ▸ Cube, Sphere, Plane,
        Cylinder, Wedge)."""
        if self.project is None:
            return
        name, count = self._unique_name(PRIMITIVES[asset])
        self._add(Entity(self.project.next_entity_id(), name, self.target_folder(),
                         {"transform": {"position": [1.5 * count, 0.0 if asset == "prim_plane"
                                                     else 0.5, 0.0]},
                          "mesh": {"asset": asset},
                          "material": {"color": [0.8, 0.8, 0.8, 1.0]}}))

    def add_cube_entity(self) -> None:
        self.add_primitive("prim_cube")

    def move_entity(self, entity_id: str, parent: str | None) -> None:
        """Puts an entity into a folder or a group (or at the top with None). It stays where it
        is in the world: its transform becomes relative to the new parent."""
        if self.project is None or (entity := self.project.entity(entity_id)) is None:
            return
        entities = self.project.entities()
        inside = {e.id for e in descendants(entities, entity_id)}
        if parent == entity.parent or parent == entity_id or parent in inside:
            return  # no change, or a folder into itself
        by_id = {e.id: e for e in entities}
        if any(k in entity.components for k in NOT_IN_GROUPS) and self._in_group(by_id, parent):
            return  # the player and cameras place themselves in the world
        if "transform" in entity.components:
            world = transforms.world_matrix(by_id, entity_id)
            with contextlib.suppress(ValueError):  # a parent scaled to nothing: kept as it was
                entity.components["transform"] = transforms.relative_to(
                    transforms.world_matrix(by_id, parent), world)
        entity.parent = parent
        folder = self.project.entity(parent) if parent else None
        label = f"moved to {folder.name}" if folder else "moved to the top"
        self.project.undo_stack.push(SetEntityCommand(self.project, entity, label))
        self.select(entity_id)

    @staticmethod
    def _in_group(by_id: dict[str, Entity], entity_id: str | None) -> bool:
        while entity_id is not None and entity_id in by_id:
            if "group" in by_id[entity_id].components:
                return True
            entity_id = by_id[entity_id].parent
        return False

    def delete_selected(self) -> None:
        if self.project is None or (entity_id := self.selected_id()) is None:
            return
        entity = self.project.entity(entity_id)
        if entity is not None:
            self.project.undo_stack.push(DeleteEntityCommand(self.project, entity))
