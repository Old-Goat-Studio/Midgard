# SPDX-License-Identifier: Apache-2.0
"""The Scenes panel (milestone 5.11a): the game's scenes, the one it starts in first (marked,
in bold; the one open for editing in italics), with Open, New, Duplicate, Rename, Delete and
Make Starting Scene, each change one undoable step. Double-clicking a scene opens it too."""
from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QFont
from PySide6.QtWidgets import (
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QMessageBox,
    QPushButton,
    QStackedLayout,
    QVBoxLayout,
    QWidget,
)

from .. import scenes

START_MARK = "  (starts here)"


class ScenesPanel(QWidget):
    open_requested = Signal(str)  # scene id

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.project = None
        self.stack = QStackedLayout(self)
        self.stack.addWidget(QLabel("Open a project to see its scenes."))
        page = QWidget()
        self.stack.addWidget(page)
        layout = QVBoxLayout(page)
        self.list = QListWidget()
        self.list.itemDoubleClicked.connect(lambda _item: self.open_selected())
        self.list.currentRowChanged.connect(lambda _r: self._enable())
        layout.addWidget(self.list, 1)
        self.buttons: dict[str, QPushButton] = {}
        for row in ((("Open", self.open_selected), ("New", self.new_scene),
                     ("Duplicate", self.duplicate)),
                    (("Rename", self.rename), ("Delete", lambda: self.delete(confirm=True)),
                     ("Make Starting Scene", self.make_start))):
            line = QHBoxLayout()
            for text, slot in row:
                b = QPushButton(text)
                b.clicked.connect(lambda _=False, s=slot: s())
                line.addWidget(b)
                self.buttons[text] = b
            layout.addLayout(line)
        self.set_can_open(False)

    def set_can_open(self, can: bool) -> None:
        """Whether the editor can open a scene (Open, and double-clicking one)."""
        self.can_open = can
        self.buttons["Open"].setVisible(can)

    def set_project(self, project) -> None:
        if self.project is not None:
            self.project.scenes_changed.disconnect(self._changed)
            self.project.scene_opened.disconnect(self._changed)
        self.project = project
        self.stack.setCurrentIndex(0 if project is None else 1)
        if project is not None:
            project.scenes_changed.connect(self._changed)
            project.scene_opened.connect(self._changed)
        self.reload()

    def _changed(self, scene: str) -> None:
        self.reload(select=scene)

    def reload(self, select: str | None = None) -> None:
        keep = select or self.selected()
        self.list.clear()
        if self.project is None:
            return
        start = scenes.start_scene(self.project)
        for s in scenes.load_all(self.project):
            item = QListWidgetItem(s["name"] + (START_MARK if s["id"] == start else ""))
            item.setData(Qt.ItemDataRole.UserRole, s["id"])
            is_open = s["id"] == self.project.scene
            item.setToolTip(s["id"] + (" (open for editing)" if is_open else ""))
            font = QFont(item.font())
            font.setBold(s["id"] == start)
            font.setItalic(is_open)  # the scene the editor is editing
            item.setFont(font)
            self.list.addItem(item)
        rows = [i for i in range(self.list.count())
                if self.list.item(i).data(Qt.ItemDataRole.UserRole) == keep]
        self.list.setCurrentRow(rows[0] if rows else 0)
        self._enable()

    def selected(self) -> str | None:
        item = self.list.currentItem()
        return item.data(Qt.ItemDataRole.UserRole) if item else None

    def _enable(self) -> None:
        scene = self.selected()
        start = scenes.start_scene(self.project) if self.project else None
        for key in ("Open", "Duplicate", "Rename"):
            self.buttons[key].setEnabled(scene is not None)
        self.buttons["Delete"].setEnabled(scene is not None and scene != start
                                          and self.list.count() > 1)
        self.buttons["Make Starting Scene"].setEnabled(scene is not None and scene != start)

    def _push(self, command) -> None:
        self.project.undo_stack.push(command)

    # ---- actions ------------------------------------------------------------------------------
    def open_selected(self) -> None:
        if self.can_open and self.selected():
            self.open_requested.emit(self.selected())

    def new_scene(self, name: str | None = None) -> str | None:
        if self.project is None:
            return None
        if name is None:
            name, ok = QInputDialog.getText(self, "New Scene", "Name")
            if not ok or not name.strip():
                return None
        command = scenes.AddSceneCommand(self.project, name.strip())
        self._push(command)
        return command.scene

    def duplicate(self, name: str | None = None) -> str | None:
        source = self.selected()
        if self.project is None or source is None:
            return None
        if name is None:
            name, ok = QInputDialog.getText(self, "Duplicate Scene", "Name",
                                            text=f"{scenes.name(self.project, source)} (copy)")
            if not ok or not name.strip():
                return None
        command = scenes.AddSceneCommand(self.project, name.strip(), copy_of=source)
        self._push(command)
        return command.scene

    def rename(self, name: str | None = None) -> None:
        scene = self.selected()
        if self.project is None or scene is None:
            return
        if name is None:
            name, ok = QInputDialog.getText(self, "Rename Scene", "Name",
                                            text=scenes.name(self.project, scene))
            if not ok or not name.strip():
                return
        if name.strip() != scenes.name(self.project, scene):
            self._push(scenes.RenameSceneCommand(self.project, scene, name.strip()))

    def delete(self, confirm: bool = True) -> bool:
        scene = self.selected()
        if self.project is None or scene is None:
            return False
        try:
            command = scenes.DeleteSceneCommand(self.project, scene)
        except scenes.SceneError as e:
            if confirm:
                QMessageBox.information(self, "Scenes", str(e))
            return False
        if confirm and QMessageBox.question(
                self, "Delete Scene", f"Delete {scenes.name(self.project, scene)} and "
                "everything in it?") != QMessageBox.StandardButton.Yes:
            return False
        self._push(command)
        return True

    def make_start(self) -> None:
        scene = self.selected()
        if self.project is not None and scene and scene != scenes.start_scene(self.project):
            self._push(scenes.SetStartSceneCommand(self.project, scene))
