# SPDX-License-Identifier: Apache-2.0
"""Build ▸ Export…: choose a target and a folder; install runtime templates."""
import logging
from pathlib import Path

from PySide6.QtCore import QUrl
from PySide6.QtGui import QDesktopServices
from PySide6.QtWidgets import (
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
    QWidget,
)

from . import android_apk
from .cook import CookError
from .exporter import (
    TARGETS,
    ExportError,
    android_package,
    export,
    find_template,
    install_template,
)
from .project import Project
from .settings import Settings

log = logging.getLogger("midgard")


class ExportDialog(QDialog):
    def __init__(self, project: Project, settings: Settings, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWindowTitle("Export")
        self.project, self.settings = project, settings
        self.archive: Path | None = None

        self.target = QComboBox()
        for key, spec in TARGETS.items():
            self.target.addItem(spec["label"], key)
        self.target.currentIndexChanged.connect(self._refresh)
        self.status = QLabel()
        self.status.setWordWrap(True)
        install = QPushButton("Install Template…")
        install.clicked.connect(self._install)
        default_out = str(project.root / "build" / "export")
        self.folder = QLineEdit(settings.get("export_dir") or default_out)
        browse = QPushButton("Browse…")
        browse.clicked.connect(self._browse)
        row = QHBoxLayout()
        row.addWidget(self.folder)
        row.addWidget(browse)
        # Android only: the application ID, and which key signs the APK.
        self.package = QLineEdit(android_package(project))
        self.package.setToolTip("The game's unique Android ID, for example "
                                "com.yourstudio.yourgame. Keep it once players have the game.")
        self.signing = QLabel(
            f"The editor's development key, kept in {settings.dir / 'android'}. Back it up: "
            "an installed game only accepts updates signed with the same key.")
        self.signing.setWordWrap(True)

        form = QFormLayout(self)
        self.form = form
        form.addRow("Target", self.target)
        form.addRow("Runtime template", self.status)
        form.addRow("", install)
        form.addRow("Android package", self.package)
        form.addRow("Signing", self.signing)
        form.addRow("Output folder", row)
        self.buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Cancel)
        self.export_button = self.buttons.addButton("Export",
                                                    QDialogButtonBox.ButtonRole.AcceptRole)
        self.buttons.accepted.connect(self._export)
        self.buttons.rejected.connect(self.reject)
        form.addRow(self.buttons)
        self._refresh()

    def selected_target(self) -> str:
        return self.target.currentData()

    def _refresh(self) -> None:
        android = TARGETS[self.selected_target()]["os"] == "android"
        for field in (self.package, self.signing):
            self.form.setRowVisible(field, android)
        template = find_template(self.selected_target(), self.settings.dir)
        if template is None:
            self.status.setText("Not installed. Download the template zip for this target from "
                                "Midgard's CI or releases, then choose Install Template….")
        else:
            self.status.setText(f"Engine {template.info.get('engine', '?')} ({template.folder})")
        self.export_button.setEnabled(template is not None)

    def _install(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Install Runtime Template", "",
                                              "Runtime templates (midgard-runtime-*.zip)")
        if not path:
            return
        try:
            folder = install_template(Path(path), self.settings.dir)
        except ExportError as e:
            QMessageBox.warning(self, "Install Template", str(e))
            return
        log.info("Installed runtime template in %s", folder)
        self._refresh()

    def _browse(self) -> None:
        folder = QFileDialog.getExistingDirectory(self, "Output folder", self.folder.text())
        if folder:
            self.folder.setText(folder)

    def _export(self) -> None:
        out = Path(self.folder.text())
        if TARGETS[self.selected_target()]["os"] == "android":
            package = self.package.text().strip()
            if not android_apk.valid_package_name(package):
                QMessageBox.warning(self, "Export", f"'{package}' is not a valid Android package "
                                    "name. Use something like com.yourstudio.yourgame.")
                return
            if package != android_package(self.project):
                self.project.set_config("android_package", package)
        try:
            self.archive = export(self.project, self.selected_target(), out, self.settings.dir)
        except (ExportError, CookError, OSError) as e:
            QMessageBox.warning(self, "Export", str(e))
            log.error("Export failed: %s", e)
            return
        self.settings.set("export_dir", str(out))
        log.info("Exported %s", self.archive)
        QDesktopServices.openUrl(QUrl.fromLocalFile(str(out)))
        self.accept()
