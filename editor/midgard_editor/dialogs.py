# SPDX-License-Identifier: Apache-2.0
"""Small dialogs: New Project, Preferences, and Git (Put Project Under Git, Commit)."""
import logging
import sys
from pathlib import Path

from PySide6.QtCore import Qt
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
    QListWidget,
    QListWidgetItem,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from . import vcs
from .project import NAME_PATTERN, WORLD_MODES, Project, ProjectError
from .settings import Settings, projects_dir
from .vcs import Identity

log = logging.getLogger("midgard")


class NewProjectDialog(QDialog):
    def __init__(self, settings: Settings, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWindowTitle("New Project")
        self.project: Project | None = None
        self.name = QLineEdit()
        self.name.setPlaceholderText("MyGame")
        self.title = QLineEdit()
        self.title.setPlaceholderText("The title players see (can be changed later)")
        # Every project is made in the projects folder (Edit ▸ Preferences).
        self.location = QLineEdit(str(projects_dir(settings)))
        self.location.setReadOnly(True)
        self.location.setToolTip("Projects are made in the projects folder: change it in "
                                 "Edit ▸ Preferences")
        where = QLabel("Change the projects folder in Edit ▸ Preferences.")
        where.setEnabled(False)
        self.world_mode = QComboBox()
        for key, label in WORLD_MODES.items():
            self.world_mode.addItem(label, key)
        note = QLabel("The world mode is fixed once the project is created.")
        note.setEnabled(False)
        self.git = QCheckBox("Keep the project in Git (version control)")
        self.git.setToolTip("Makes the project folder a Git repository with a first commit. "
                            "The Script editor then marks lines changed since the last commit.")
        self.git.setEnabled(vcs.available())
        self.git.setChecked(vcs.available() and bool(settings.get("new_project_git")))

        form = QFormLayout(self)
        form.addRow("Project name", self.name)
        form.addRow("Game title", self.title)
        form.addRow("Projects folder", self.location)
        form.addRow("", where)
        form.addRow("World mode", self.world_mode)
        form.addRow("", note)
        form.addRow("", self.git)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok
                                   | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self._create)
        buttons.rejected.connect(self.reject)
        form.addRow(buttons)
        self.settings = settings

    def _create(self) -> None:
        name = self.name.text().strip()
        if not NAME_PATTERN.match(name):
            QMessageBox.warning(self, "New Project",
                                "Project names start with a letter and use only letters, "
                                "digits, '-' and '_'.")
            return
        try:
            Path(self.location.text()).mkdir(parents=True, exist_ok=True)
            self.project = Project.create(Path(self.location.text()), name,
                                          self.world_mode.currentData(), self.title.text())
        except (ProjectError, OSError) as e:
            QMessageBox.warning(self, "New Project", str(e))
            return
        if self.git.isEnabled():
            self.settings.set("new_project_git", self.git.isChecked())
            if self.git.isChecked():
                put_project_under_git(self.project, self)
        self.accept()


def ask_identity(parent: QWidget | None) -> Identity | None:
    """Asks for the name and email Git commits with (kept in this repository only)."""
    dialog = QDialog(parent)
    dialog.setWindowTitle("Git Name and Email")
    name, email = QLineEdit(), QLineEdit()
    email.setPlaceholderText("you@example.com")
    form = QFormLayout(dialog)
    note = QLabel("Git records who made each commit. These are saved in this project's "
                  "repository only.")
    note.setWordWrap(True)
    form.addRow(note)
    form.addRow("Name", name)
    form.addRow("Email", email)
    buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok
                               | QDialogButtonBox.StandardButton.Cancel)
    buttons.accepted.connect(dialog.accept)
    buttons.rejected.connect(dialog.reject)
    form.addRow(buttons)
    if dialog.exec() != QDialog.DialogCode.Accepted:
        return None
    if not name.text().strip() or "@" not in email.text():
        QMessageBox.warning(parent, "Git", "A name and an email address are needed.")
        return None
    return Identity(name.text().strip(), email.text().strip())


def put_project_under_git(project: Project, parent: QWidget | None) -> bool:
    """File ▸ Put Project Under Git…, and New Project's tick box. True if it worked."""
    who = None
    if vcs.identity(project.root) is None:
        who = ask_identity(parent)
        if who is None:
            return False
    try:
        commit = vcs.put_under_git(project, who)
    except vcs.VcsError as e:
        QMessageBox.warning(parent, "Git", str(e))
        return False
    log.info("The project is now in Git (first commit %s).", commit)
    return True


class CommitDialog(QDialog):
    """Commit: the project's changed files (tick the ones to commit) and a message."""

    LABELS = {"modified": "Changed", "new": "New", "deleted": "Deleted", "renamed": "Renamed",
              "conflict": "Conflict"}

    def __init__(self, project: Project, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWindowTitle(f"Commit - {project.name}")
        self.resize(520, 440)
        self.project = project
        self.commit_id = ""
        self.files = vcs.changed_files(project)
        self.list = QListWidget()
        for f in self.files:
            item = QListWidgetItem(f"{f.path}   ({self.LABELS.get(f.state, f.state).lower()})")
            item.setData(Qt.ItemDataRole.UserRole, f.path)
            item.setFlags(item.flags() | Qt.ItemFlag.ItemIsUserCheckable)
            item.setCheckState(Qt.CheckState.Checked)
            self.list.addItem(item)
        self.message = QPlainTextEdit(vcs.suggest_message(self.files))
        self.message.setPlaceholderText("What changed, and why")
        self.message.setMaximumHeight(110)
        self.message.selectAll()
        note = QLabel("The project's scenes and settings are in its .db file, so commit it "
                      "with the scripts that go with them.")
        note.setWordWrap(True)
        note.setEnabled(False)
        self.buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Cancel)
        self.commit_button = self.buttons.addButton("Commit",
                                                    QDialogButtonBox.ButtonRole.AcceptRole)
        self.buttons.accepted.connect(self._commit)
        self.buttons.rejected.connect(self.reject)
        self.list.itemChanged.connect(lambda _i: self._update())
        self.message.textChanged.connect(self._update)
        layout = QVBoxLayout(self)
        layout.addWidget(QLabel("Files to commit"))
        layout.addWidget(self.list)
        layout.addWidget(note)
        layout.addWidget(QLabel("Message"))
        layout.addWidget(self.message)
        layout.addWidget(self.buttons)
        self.message.setFocus()
        self._update()

    def ticked(self) -> list[str]:
        return [self.list.item(i).data(Qt.ItemDataRole.UserRole) for i in range(self.list.count())
                if self.list.item(i).checkState() == Qt.CheckState.Checked]

    def _update(self) -> None:
        self.commit_button.setEnabled(bool(self.ticked()) and
                                      bool(self.message.toPlainText().strip()))

    def _commit(self) -> None:
        who = None
        if vcs.identity(self.project.root) is None:
            who = ask_identity(self)
            if who is None:
                return
        try:
            self.commit_id = vcs.commit(self.project, self.ticked(), self.message.toPlainText(),
                                        who)
        except vcs.VcsError as e:
            QMessageBox.warning(self, "Commit", str(e))
            return
        log.info("Committed %d file(s) as %s: %s", len(self.ticked()), self.commit_id,
                 self.message.toPlainText().strip().splitlines()[0])
        self.accept()


class PreferencesDialog(QDialog):
    def __init__(self, settings: Settings, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWindowTitle("Preferences")
        self.settings = settings
        form = QFormLayout(self)
        self.open_last = QCheckBox("Open the last project when Midgard starts")
        self.open_last.setChecked(bool(settings.get("open_last_project")))
        form.addRow("Starting", self.open_last)
        self.projects = QLineEdit(str(projects_dir(settings)))
        self.projects.setToolTip("New projects are made here, each in its own folder (with its "
                                 "Asset Library); Open Project starts here")
        browse = QPushButton("Browse…")
        browse.clicked.connect(self._browse_projects)
        row = QHBoxLayout()
        row.addWidget(self.projects)
        row.addWidget(browse)
        form.addRow("Projects folder", row)
        self.error_log = QCheckBox("Keep an error log (error.log in the project's folder)")
        self.error_log.setToolTip("The last 100 keys and clicks, and errors, so a crash can be "
                                  "understood. Turn it off for speed (each project can too, on "
                                  "its Project Page).")
        self.error_log.setChecked(bool(settings.get("error_log", True)))
        form.addRow("Error log", self.error_log)
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

    def _browse_projects(self) -> None:
        folder = QFileDialog.getExistingDirectory(self, "Projects folder", self.projects.text())
        if folder:
            self.projects.setText(folder)

    def _save(self) -> None:
        folder = self.projects.text().strip()
        if folder and folder != str(projects_dir(self.settings)):  # (changed: made, if need be)
            try:
                Path(folder).mkdir(parents=True, exist_ok=True)
            except OSError as e:
                QMessageBox.warning(self, "Preferences", f"The projects folder can't be made: {e}")
                return
            self.settings.set("projects_dir", folder)
        self.settings.set("open_last_project", self.open_last.isChecked())
        self.settings.set("error_log", self.error_log.isChecked())
        self.settings.set("backup_keep", self.keep.value())
        self.settings.set("backup_minutes", self.minutes.value())
        self.settings.set("use_x11", self.x11.isChecked())
        self.settings.set("assistant_enabled", self.assistant.isChecked())
        self.settings.set("assistant_model", self.model.currentText().strip() or "claude-opus-5-5")
        self.settings.set("assistant_effort", self.effort.currentText())
        self.accept()
