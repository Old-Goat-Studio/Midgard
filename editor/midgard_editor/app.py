# SPDX-License-Identifier: Apache-2.0
"""Starting the editor GUI."""
import json
import os
import sys
import tempfile
from pathlib import Path

from PySide6.QtCore import QCoreApplication, Qt, QTimer
from PySide6.QtGui import QGuiApplication, QIcon
from PySide6.QtWidgets import QApplication

from . import APP_NAME, ICON_PATH
from .project import Project
from .settings import Settings
from .version import VERSION


def _prepare_platform(settings: Settings) -> None:
    """Things that must happen before the QApplication exists."""
    # Don't round 150 % monitors up to 200 %: scale exactly, per monitor.
    QGuiApplication.setHighDpiScaleFactorRoundingPolicy(
        Qt.HighDpiScaleFactorRoundingPolicy.PassThrough)
    # Wayland doesn't let applications place their own windows, so floating panels can't be
    # restored onto a chosen monitor. XWayland (xcb) can.
    if (sys.platform.startswith("linux") and settings.get("use_x11")
            and os.environ.get("WAYLAND_DISPLAY") and "QT_QPA_PLATFORM" not in os.environ):
        os.environ["QT_QPA_PLATFORM"] = "xcb"


def run(project: Path | None = None, smoke_test: Path | None = None) -> int:
    QCoreApplication.setApplicationName(APP_NAME)
    QCoreApplication.setOrganizationName("")  # keeps settings in .../Midgard, not .../Org/Midgard
    QCoreApplication.setApplicationVersion(VERSION)
    settings = Settings(smoke_test / "settings" if smoke_test else None)
    _prepare_platform(settings)
    app = QApplication.instance() or QApplication(sys.argv)
    app.setWindowIcon(QIcon(str(ICON_PATH)))

    from .main_window import MainWindow  # import after QApplication exists

    window = MainWindow(settings)
    window.show()
    if smoke_test is not None:
        return _smoke_test(app, window, smoke_test)
    if project is not None:
        window.open_project(project)
    return app.exec()


def _smoke_test(app: QApplication, window, folder: Path) -> int:
    """Create and open a project, check the window, write smoke_test.json and quit. Used by CI
    to check a packaged editor really starts (a windowed build has no console to print to)."""
    folder.mkdir(parents=True, exist_ok=True)
    project_dir = Path(tempfile.mkdtemp(prefix="smoke_", dir=folder))
    result = {"ok": False}
    try:
        project = Project.create(project_dir, "SmokeTest", "system_map", "Smoke Test")
        opened = window.open_project(project.config_path)
        app.processEvents()
        from . import engine as engine_module
        from .about import engine_info

        engine = engine_info()
        result = {
            "engine": engine.version if engine else None,
            "engine_module": engine_module.module() is not None,
            "ok": opened and window.windowTitle() == f"{APP_NAME} - SmokeTest",
            "title": window.windowTitle(),
            "panels": sorted(window.panels),
            "version": VERSION,
            "frozen": bool(getattr(sys, "frozen", False)),
        }
    except Exception as e:  # report anything, so CI shows why the smoke test failed
        result["error"] = repr(e)
    (folder / "smoke_test.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    QTimer.singleShot(0, window.close)
    app.exec()
    return 0 if result["ok"] else 1
