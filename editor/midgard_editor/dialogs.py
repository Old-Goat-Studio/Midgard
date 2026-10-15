# SPDX-License-Identifier: Apache-2.0
"""Small dialogs: New Project and Preferences."""
import sys
from pathlib import Path

from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFileDialog,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPushButton,
    QSpinBox,
    QWidget,
)

from .project import NAME_PATTERN, WORLD_MODES, Project, ProjectError
from .settings import Settings


class NewProjectDialog(QDialog):
    def __init__(self, settings: Settings, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWindowTitle("New Project")
        self.project: Project | None = None
        self.name = QLineEdit()
        self.name.setPlaceholderText("MyGame")
        self.title = QLineEdit()
        self.title.setPlaceholderText("The title players see (can be changed later)")
        self.location = QLineEdit(settings.get("last_project_dir") or str(Path.home()))
        browse = QPushButton("Browse…")
        browse.clicked.connect(self._browse)
        row = QHBoxLayout()
        row.addWidget(self.location)
        row.addWidget(browse)
        self.world_mode = QComboBox()
        for key, label in WORLD_MODES.items():
            self.world_mode.addItem(label, key)
        note = QLabel("The world mode is fixed once the project is created.")
        note.setEnabled(False)

        form = QFormLayout(self)
        form.addRow("Project name", self.name)
        form.addRow("Game title", self.title)
        form.addRow("Location", row)
        form.addRow("World mode", self.world_mode)
        form.addRow("", note)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok
                                   | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self._create)
        buttons.rejected.connect(self.reject)
        form.addRow(buttons)
        self.settings = settings

    def _browse(self) -> None:
        folder = QFileDialog.getExistingDirectory(self, "Project location", self.location.text())
        if folder:
            self.location.setText(folder)

    def _create(self) -> None:
        name = self.name.text().strip()
        if not NAME_PATTERN.match(name):
            QMessageBox.warning(self, "New Project",
                                "Project names start with a letter and use only letters, "
                                "digits, '-' and '_'.")
            return
        try:
            self.project = Project.create(Path(self.location.text()), name,
                                          self.world_mode.currentData(), self.title.text())
        except (ProjectError, OSError) as e:
            QMessageBox.warning(self, "New Project", str(e))
            return
        self.settings.set("last_project_dir", self.location.text())
        self.accept()


class PreferencesDialog(QDialog):
    def __init__(self, settings: Settings, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWindowTitle("Preferences")
        self.settings = settings
        form = QFormLayout(self)
        self.keep = QSpinBox(minimum=1, maximum=100, value=int(settings.get("backup_keep")))
        self.minutes = QSpinBox(minimum=1, maximum=120, suffix=" min",
                                value=int(settings.get("backup_minutes")))
        form.addRow("Project backups to keep", self.keep)
        form.addRow("Back up every", self.minutes)
        self.x11 = QCheckBox("Start under X11 (XWayland) for full multi-monitor support")
        self.x11.setChecked(bool(settings.get("use_x11")))
        if sys.platform.startswith("linux"):
            form.addRow("Linux", self.x11)
            hint = QLabel("Takes effect the next time Midgard starts.")
            hint.setEnabled(False)
            form.addRow("", hint)
        from .assistant import EFFORTS, MODELS

        self.assistant = QCheckBox("Turn on the Assistant")
        self.assistant.setToolTip("Sends your questions, and the project data it looks at, to "
                                  "Anthropic's API with your own API key.")
        self.assistant.setChecked(bool(settings.get("assistant_enabled")))
        self.model = QComboBox()
        self.model.addItems(MODELS)
        self.model.setEditable(True)  # newer models can be typed in
        self.model.setCurrentText(settings.get("assistant_model"))
        self.effort = QComboBox()
        self.effort.addItems(EFFORTS)
        self.effort.setCurrentText(settings.get("assistant_effort"))
        form.addRow("Assistant", self.assistant)
        form.addRow("Model", self.model)
        form.addRow("Effort", self.effort)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok
                                   | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self._save)
        buttons.rejected.connect(self.reject)
        form.addRow(buttons)

    def _save(self) -> None:
        self.settings.set("backup_keep", self.keep.value())
        self.settings.set("backup_minutes", self.minutes.value())
        self.settings.set("use_x11", self.x11.isChecked())
        self.settings.set("assistant_enabled", self.assistant.isChecked())
        self.settings.set("assistant_model", self.model.currentText().strip() or "claude-opus-5-5")
        self.settings.set("assistant_effort", self.effort.currentText())
        self.accept()
