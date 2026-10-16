# SPDX-License-Identifier: Apache-2.0
"""The Inspector panel. In Phase 1 it shows the project settings (every change undoable) or
details of the file selected in the Project panel. Later phases add an inspector per system."""
from datetime import datetime
from pathlib import Path

from PySide6.QtCore import QRegularExpression, QTimer, Signal
from PySide6.QtGui import QRegularExpressionValidator
from PySide6.QtWidgets import QFormLayout, QLabel, QLineEdit, QSpinBox, QStackedLayout, QWidget

from ..project import GAME_TITLE_KEY, WORLD_MODES, Project
from ..undo import SetConfigCommand, SetStringCommand
from .entity_form import EntityForm


class ProjectSettingsForm(QWidget):
    def __init__(self, project: Project, parent: QWidget | None = None):
        super().__init__(parent)
        self.project = project
        form = QFormLayout(self)
        self.title = QLineEdit()
        self.title.setToolTip("Shown to players. Stored as the localisation key 'game.title'.")
        self.game_version = QLineEdit()
        self.game_version.setValidator(QRegularExpressionValidator(
            QRegularExpression(r"\d{1,3}\.\d{1,3}\.\d{1,3}"), self.game_version))
        self.max_players = QSpinBox(minimum=2, maximum=16)
        self.world_mode = QLabel(WORLD_MODES.get(project.config.get("world_mode"), "?"))
        self.world_mode.setToolTip("Chosen when the project is created; it can't be changed.")
        form.addRow("Game title", self.title)
        form.addRow("Game version", self.game_version)
        form.addRow("Max players", self.max_players)
        form.addRow("World mode", self.world_mode)
        form.addRow("Project", QLabel(project.name))
        folder = QLineEdit(str(project.root), readOnly=True)  # long paths scroll inside the field
        folder.setToolTip(str(project.root))
        folder.setCursorPosition(0)
        form.addRow("Folder", folder)
        form.addRow("Created", QLabel(project.config.get("created", "")))
        form.addRow("Database schema", QLabel(str(project.schema_version())))
        self.refresh()

        self.title.editingFinished.connect(self._title_edited)
        self.game_version.editingFinished.connect(self._version_edited)
        self.max_players.editingFinished.connect(self._players_edited)
        project.config_changed.connect(self.refresh)
        project.string_changed.connect(self.refresh)

    def refresh(self, *_args) -> None:
        self.title.setText(self.project.game_title)
        self.game_version.setText(self.project.config.get("game_version", "0.1.0"))
        self.max_players.setValue(int(self.project.config.get("max_players", 4)))

    def _title_edited(self) -> None:
        text = self.title.text().strip()
        if text and text != self.project.game_title:
            self.project.undo_stack.push(
                SetStringCommand(self.project, GAME_TITLE_KEY, text, "game title"))
        else:
            self.refresh()

    def _version_edited(self) -> None:
        # editingFinished only fires when the validator accepts major.minor.patch
        text = ".".join(str(int(p)) for p in self.game_version.text().split("."))
        if text != self.project.config.get("game_version"):
            self.project.undo_stack.push(
                SetConfigCommand(self.project, "game_version", text, "game version"))
        self.refresh()

    def _players_edited(self) -> None:
        value = self.max_players.value()
        if value != self.project.config.get("max_players"):
            self.project.undo_stack.push(
                SetConfigCommand(self.project, "max_players", value, "max players"))


class FileDetails(QWidget):
    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        form = QFormLayout(self)
        self.name, self.kind, self.size, self.modified = QLabel(), QLabel(), QLabel(), QLabel()
        for label, widget in (("Name", self.name), ("Type", self.kind), ("Size", self.size),
                              ("Modified", self.modified)):
            form.addRow(label, widget)

    def show_path(self, path: Path) -> None:
        stat = path.stat()
        self.name.setText(path.name)
        self.kind.setText("Folder" if path.is_dir() else (path.suffix[1:].upper() or "File"))
        self.size.setText("" if path.is_dir() else f"{stat.st_size:,} bytes")
        self.modified.setText(datetime.fromtimestamp(stat.st_mtime).strftime("%Y-%m-%d %H:%M"))


class Inspector(QWidget):
    open_script = Signal(str)  # a script module to open in the Script editor

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.stack = QStackedLayout(self)
        self.empty = QLabel("Open a project to see its settings.")
        self.files = FileDetails()
        self.stack.addWidget(self.empty)
        self.stack.addWidget(self.files)
        self.settings: ProjectSettingsForm | None = None
        self.entity_form: EntityForm | None = None

    def _drop_entity_form(self) -> None:
        if self.entity_form is not None:
            self.stack.removeWidget(self.entity_form)
            self.entity_form.deleteLater()
            self.entity_form = None

    def show_entity(self, entity_id: str | None) -> None:
        """An entity selected in the Scene panel (None: back to the project settings)."""
        self._drop_entity_form()
        if self.settings is None:
            return
        if entity_id is None or self.settings.project.entity(entity_id) is None:
            self.stack.setCurrentWidget(self.settings)
            return
        self.entity_form = EntityForm(self.settings.project, entity_id)
        self.entity_form.open_script.connect(self.open_script)
        # Rebuilt after the current signal has been delivered (the old form sent it).
        self.entity_form.rebuild.connect(
            lambda eid: QTimer.singleShot(0, lambda: self.show_entity(eid)))
        self.stack.addWidget(self.entity_form)
        self.stack.setCurrentWidget(self.entity_form)

    def set_project(self, project: Project | None) -> None:
        self._drop_entity_form()
        if self.settings is not None:
            self.stack.removeWidget(self.settings)
            self.settings.deleteLater()
            self.settings = None
        if project is None:
            self.stack.setCurrentWidget(self.empty)
            return
        self.settings = ProjectSettingsForm(project)
        self.stack.addWidget(self.settings)
        self.stack.setCurrentWidget(self.settings)

    def show_selection(self, path: Path | None) -> None:
        """A file or folder selected in the Project panel; the project's own files (or nothing)
        show the project settings."""
        if self.settings is None:
            return
        project = self.settings.project
        if path is None or path in (project.config_path, project.db_path) or not path.exists():
            self.stack.setCurrentWidget(self.settings)
        else:
            self.files.show_path(path)
            self.stack.setCurrentWidget(self.files)
