# SPDX-License-Identifier: Apache-2.0
"""The Midgard main window: "Midgard - {project}", dockable panels across monitors, menus."""
import logging
from pathlib import Path

import PySide6QtAds as QtAds
from PySide6.QtCore import QProcess, QRect, Qt
from PySide6.QtGui import (
    QAction,
    QActionGroup,
    QCloseEvent,
    QGuiApplication,
    QIcon,
    QKeySequence,
    QUndoGroup,
)
from PySide6.QtWidgets import (
    QFileDialog,
    QInputDialog,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QMainWindow,
    QMenu,
    QMessageBox,
    QPushButton,
    QStackedWidget,
    QUndoView,
    QVBoxLayout,
    QWidget,
)

from . import APP_NAME, DESCRIPTION, ICON_PATH
from . import layouts as layout_store
from .about import AboutDialog, engine_info, runtime_candidates
from .backup import BackupScheduler
from .cook import CookError, cook
from .dialogs import NewProjectDialog, PreferencesDialog
from .docks.inspector import Inspector
from .docks.log import LOGGER_NAME, LogPanel
from .docks.project_browser import ProjectBrowser
from .docks.scene_panel import ScenePanel
from .project import WORLD_MODES, Project, ProjectError
from .settings import Settings
from .themes import QSS_PREFIX, THEMES, ThemeApplier, qss_themes, theme_title

log = logging.getLogger(LOGGER_NAME)

# objectName -> title. Object names are the keys in saved layouts, so never rename them.
PANELS = {"project": "Project", "scene": "Scene", "inspector": "Inspector", "log": "Log",
          "history": "History"}
BUILT_IN_LAYOUTS = ("Default", "Two Monitors")


def add_action(menu: QMenu, text: str, slot, shortcut=None) -> QAction:
    action = menu.addAction(text)
    action.triggered.connect(lambda _=False: slot())
    if shortcut is not None:
        action.setShortcut(QKeySequence(shortcut))
    return action


class StartPage(QWidget):
    """The central area: a start page with no project open, a summary with one open. The 3D
    viewport replaces the summary in Phase 3."""

    def __init__(self, window: "MainWindow"):
        super().__init__()
        self.stack = QStackedWidget()
        layout = QVBoxLayout(self)
        layout.addWidget(self.stack)

        welcome = QWidget()
        w = QVBoxLayout(welcome)
        heading = QLabel(f"<h1>{APP_NAME}</h1><p>{DESCRIPTION}</p>")
        new = QPushButton("New Project…")
        new.clicked.connect(window.new_project)
        open_ = QPushButton("Open Project…")
        open_.clicked.connect(window.open_project_dialog)
        self.recent = QListWidget()
        self.recent.itemActivated.connect(
            lambda item: window.open_project(Path(item.data(Qt.ItemDataRole.UserRole))))
        w.addWidget(heading)
        w.addWidget(new)
        w.addWidget(open_)
        w.addWidget(QLabel("Recent projects"))
        w.addWidget(self.recent)
        self.stack.addWidget(welcome)

        self.summary = QLabel()
        self.summary.setWordWrap(True)
        self.stack.addWidget(self.summary)

    def show_welcome(self, recent: list[Path]) -> None:
        self.recent.clear()
        for path in recent:
            item = QListWidgetItem(f"{path.stem}    {path.parent}")
            item.setData(Qt.ItemDataRole.UserRole, str(path))
            self.recent.addItem(item)
        self.stack.setCurrentIndex(0)

    def show_project(self, project: Project) -> None:
        mode = WORLD_MODES.get(project.config.get("world_mode"), "?")
        self.summary.setText(
            f"<h2>{project.game_title}</h2><p>{mode} · version "
            f"{project.config.get('game_version')}</p><p>{project.root}</p>"
            "<p><i>The 3D viewport arrives in Phase 3.</i></p>")
        self.stack.setCurrentIndex(1)


class MainWindow(QMainWindow):
    def __init__(self, settings: Settings):
        super().__init__()
        self.settings = settings
        self.project: Project | None = None
        self.backups: BackupScheduler | None = None
        self.setWindowIcon(QIcon(str(ICON_PATH)))
        self.resize(1400, 900)

        QtAds.CDockManager.setConfigFlag(QtAds.CDockManager.eConfigFlag.OpaqueSplitterResize, True)
        QtAds.CDockManager.setConfigFlag(QtAds.CDockManager.eConfigFlag.FocusHighlighting, True)
        QtAds.CDockManager.setConfigFlag(
            QtAds.CDockManager.eConfigFlag.FloatingContainerHasWidgetTitle, True)
        self.dock_manager = QtAds.CDockManager(self)
        self.dock_manager.setFloatingContainersTitle(APP_NAME)
        self.themes = ThemeApplier(settings.dir, self.dock_manager)

        self.start = StartPage(self)
        central = QtAds.CDockWidget(self.dock_manager, "Start")
        central.setObjectName("central")
        central.setWidget(self.start)
        central.setFeature(QtAds.CDockWidget.DockWidgetFeature.NoTab, True)
        self.dock_manager.setCentralWidget(central)

        self.undo_group = QUndoGroup(self)
        self.browser = ProjectBrowser()
        self.scene_panel = ScenePanel()
        self.runtime_process: QProcess | None = None
        self.inspector = Inspector()
        self.log_panel = LogPanel()
        self.history = QUndoView(self.undo_group)
        self.history.setEmptyLabel("<project opened>")
        self.browser.file_selected.connect(self.inspector.show_selection)
        self.scene_panel.entity_selected.connect(self.inspector.show_entity)
        self.inspector.setMinimumWidth(340)
        self.browser.setMinimumWidth(220)
        widgets = {"project": self.browser, "scene": self.scene_panel, "inspector": self.inspector,
                   "log": self.log_panel, "history": self.history}
        self.panels: dict[str, QtAds.CDockWidget] = {}
        for key, title in PANELS.items():
            dock = QtAds.CDockWidget(self.dock_manager, title)
            dock.setObjectName(key)
            # Panels scroll themselves; without ADS's extra scroll area their minimum sizes count.
            dock.setWidget(widgets[key], QtAds.CDockWidget.eInsertMode.ForceNoScrollArea)
            dock.setMinimumSizeHintMode(
                QtAds.CDockWidget.eMinimumSizeHintMode.MinimumSizeHintFromContentMinimumSize)
            self.panels[key] = dock
        self._arrange_default()
        self._default_state = self.dock_manager.saveState()

        self._build_menus()
        self._restore_session()
        self.themes.apply(settings.get("theme"))
        self.set_project(None)

    # ---- layouts -----------------------------------------------------------------------
    def _arrange_default(self) -> None:
        area = QtAds.DockWidgetArea
        left = self.dock_manager.addDockWidget(area.LeftDockWidgetArea, self.panels["project"])
        self.dock_manager.addDockWidgetTabToArea(self.panels["scene"], left)
        self.panels["scene"].setAsCurrentTab()
        self.dock_manager.addDockWidget(area.RightDockWidgetArea, self.panels["inspector"])
        bottom = self.dock_manager.addDockWidget(area.BottomDockWidgetArea, self.panels["log"])
        self.dock_manager.addDockWidgetTabToArea(self.panels["history"], bottom)
        self.panels["log"].setAsCurrentTab()

    def _layout_record(self) -> dict:
        return {"state": layout_store.pack(self.dock_manager.saveState()),
                "geometry": layout_store.pack(self.saveGeometry())}

    def apply_layout(self, record: dict, geometry: bool = True) -> bool:
        if geometry and record.get("geometry"):
            self.restoreGeometry(layout_store.unpack(record["geometry"]))
        ok = self.dock_manager.restoreState(layout_store.unpack(record["state"]))
        self._rescue_windows()
        return ok

    def _rescue_windows(self) -> None:
        """Move the window and any floating panels back if their monitor has gone."""
        moved = layout_store.keep_on_screen(self)
        for floating in self.dock_manager.floatingWidgets():
            moved |= layout_store.keep_on_screen(floating)
        if moved:
            log.info("Moved windows from a monitor that is no longer connected.")

    def _restore_session(self) -> None:
        last = self.settings.get("layout_last")
        if last:
            try:
                self.apply_layout(last)
            except (KeyError, ValueError):
                log.warning("The saved layout could not be restored; using the default.")

    def reset_layout(self) -> None:
        self.dock_manager.restoreState(self._default_state)
        for dock in self.panels.values():
            if dock.isClosed():
                dock.toggleView(True)

    def two_monitor_layout(self) -> bool:
        """Default layout, with the Inspector, History and Log tabbed in one floating window on a
        second monitor. Returns False (and keeps Default) when there is only one monitor."""
        self.reset_layout()
        screens = QGuiApplication.screens()
        here = self.screen()
        others = [s for s in screens if s is not here]
        if not others:
            log.info("Only one monitor found, so the Two Monitors layout is the same as Default.")
            return False
        container = self.dock_manager.addDockWidgetFloating(self.panels["inspector"])
        area = self.panels["inspector"].dockAreaWidget()
        for key in ("history", "log"):
            self.dock_manager.addDockWidgetTabToArea(self.panels[key], area)
        target: QRect = others[0].availableGeometry()
        container.setGeometry(target.adjusted(60, 60, -60, -60))
        return True

    def save_layout_as(self) -> None:
        name, ok = QInputDialog.getText(self, "Save Layout", "Layout name:")
        name = name.strip()
        if not ok or not name:
            return
        if name in BUILT_IN_LAYOUTS:
            QMessageBox.warning(self, "Save Layout", f"'{name}' is a built-in layout name.")
            return
        saved = self.settings.get("layouts")
        saved[name] = self._layout_record()
        self.settings.set("layouts", saved)
        log.info("Saved layout '%s'.", name)

    def delete_layout(self, name: str) -> None:
        saved = self.settings.get("layouts")
        saved.pop(name, None)
        self.settings.set("layouts", saved)

    # ---- menus -------------------------------------------------------------------------
    def _build_menus(self) -> None:
        bar = self.menuBar()
        file = bar.addMenu("&File")
        add_action(file, "&New Project…", self.new_project, QKeySequence.StandardKey.New)
        add_action(file, "&Open Project…", self.open_project_dialog, QKeySequence.StandardKey.Open)
        self.recent_menu = file.addMenu("Open &Recent")
        self.recent_menu.aboutToShow.connect(self._fill_recent_menu)
        self.close_action = add_action(file, "&Close Project", self.close_project)
        file.addSeparator()
        add_action(file, "&Quit", self.close, QKeySequence.StandardKey.Quit)

        edit = bar.addMenu("&Edit")
        undo = self.undo_group.createUndoAction(self, "&Undo")
        undo.setShortcut(QKeySequence.StandardKey.Undo)
        redo = self.undo_group.createRedoAction(self, "&Redo")
        redo.setShortcut(QKeySequence.StandardKey.Redo)
        edit.addAction(undo)
        edit.addAction(redo)
        edit.addSeparator()
        add_action(edit, "&Preferences…", lambda: PreferencesDialog(self.settings, self).exec(),
                   QKeySequence.StandardKey.Preferences)

        build = bar.addMenu("&Build")
        self.cook_action = add_action(build, "&Cook", self.cook_project, "Ctrl+B")
        self.cook_run_action = add_action(build, "Cook and &Run", self.cook_and_run, "F5")

        view = bar.addMenu("&View")
        self.themes_menu = view.addMenu("&Themes")
        # Rebuilt each time it opens, so a .qss file added to a Themes folder appears at once.
        self.themes_menu.aboutToShow.connect(self._fill_themes_menu)

        window = bar.addMenu("&Window")
        for dock in self.panels.values():
            window.addAction(dock.toggleViewAction())
        window.addSeparator()
        self.layouts_menu = window.addMenu("&Layouts")
        self.layouts_menu.aboutToShow.connect(self._fill_layouts_menu)

        help_ = bar.addMenu("&Help")
        add_action(help_, f"&About {APP_NAME}", self.show_about)

    def _fill_recent_menu(self) -> None:
        self.recent_menu.clear()
        recent = self.settings.recent_projects()
        for path in recent:
            add_action(self.recent_menu, f"{path.stem}  ({path.parent})",
                       lambda p=path: self.open_project(p))
        if not recent:
            self.recent_menu.addAction("No recent projects").setEnabled(False)
        self.recent_menu.addSeparator()
        add_action(self.recent_menu, "Clear Recent Projects", self.settings.clear_recent)

    def _fill_themes_menu(self) -> None:
        menu = self.themes_menu
        menu.clear()
        group = QActionGroup(menu)
        current = self.settings.get("theme")

        def add(key: str, label: str) -> None:
            action = QAction(label, menu, checkable=True)
            action.setChecked(key == current)
            action.triggered.connect(lambda _=False, k=key: self.set_theme(k))
            group.addAction(action)
            menu.addAction(action)

        for key, (label, _scheme) in THEMES.items():
            add(key, label)
        stylesheets = qss_themes(self.settings.dir)
        if stylesheets:
            menu.addSeparator()
        for name in stylesheets:
            add(QSS_PREFIX + name, theme_title(name))

    def _fill_layouts_menu(self) -> None:
        menu = self.layouts_menu
        menu.clear()
        add_action(menu, "Default", self.reset_layout)
        add_action(menu, "Two Monitors", self.two_monitor_layout)
        saved = self.settings.get("layouts")
        if saved:
            menu.addSeparator()
            for name, record in sorted(saved.items()):
                add_action(menu, name, lambda r=record: self.apply_layout(r, geometry=False))
        menu.addSeparator()
        add_action(menu, "Save Current Layout As…", self.save_layout_as)
        if saved:
            delete = menu.addMenu("Delete Layout")
            for name in sorted(saved):
                add_action(delete, name, lambda n=name: self.delete_layout(n))
        add_action(menu, "Reset Layout", self.reset_layout)

    # ---- themes and about ----------------------------------------------------------------
    def set_theme(self, key: str) -> None:
        if self.themes.apply(key):
            self.settings.set("theme", key)
            log.info("Theme: %s", key)

    def show_about(self) -> None:
        AboutDialog(self, engine_info()).exec()

    # ---- projects ----------------------------------------------------------------------
    def new_project(self) -> None:
        dialog = NewProjectDialog(self.settings, self)
        if dialog.exec() and dialog.project:
            self.set_project(dialog.project)
            log.info("Created project %s in %s", dialog.project.name, dialog.project.root)

    def open_project_dialog(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "Open Project", self.settings.get("last_project_dir") or str(Path.home()),
            "Midgard projects (*.config)")
        if path:
            self.open_project(Path(path))

    def open_project(self, config: Path) -> bool:
        try:
            project = Project.open(config)
        except ProjectError as e:
            QMessageBox.warning(self, "Open Project", str(e))
            log.error("Could not open %s: %s", config, e)
            return False
        self.set_project(project)
        log.info("Opened project %s", project.name)
        return True

    def close_project(self) -> None:
        self.set_project(None)

    def set_project(self, project: Project | None) -> None:
        if self.project is not None:
            if self.backups is not None:
                self.backups.backup_if_needed()
                self.backups.deleteLater()
                self.backups = None
            self.undo_group.removeStack(self.project.undo_stack)
            self.project.deleteLater()
        self.project = project
        self.browser.set_project(project)
        self.inspector.set_project(project)
        self.scene_panel.set_project(project)
        for action in (self.close_action, self.cook_action, self.cook_run_action):
            action.setEnabled(project is not None)
        if project is None:
            self.setWindowTitle(APP_NAME)
            self.start.show_welcome(self.settings.recent_projects())
            return
        self.setWindowTitle(f"{APP_NAME} - {project.name}")
        self.undo_group.addStack(project.undo_stack)
        self.undo_group.setActiveStack(project.undo_stack)
        self.settings.add_recent(project.config_path)
        self.settings.set("last_project_dir", str(project.root.parent))
        self.backups = BackupScheduler(project, int(self.settings.get("backup_keep")),
                                       int(self.settings.get("backup_minutes")), self)
        self.start.show_project(project)
        project.string_changed.connect(lambda *_: self.start.show_project(project))
        project.config_changed.connect(lambda *_: self.start.show_project(project))

    # ---- build -------------------------------------------------------------------------
    def cook_project(self) -> Path | None:
        """Cook the open project for this machine. Returns the package folder."""
        if self.project is None:
            return None
        try:
            result = cook(self.project)
        except (CookError, OSError) as e:
            QMessageBox.warning(self, "Cook", str(e))
            log.error("Cook failed: %s", e)
            return None
        for warning in result.warnings:
            log.warning(warning)
        log.info("Cooked %s to %s (%d asset(s))", self.project.name, result.out_dir,
                 len(result.manifest["content"]))
        return result.out_dir

    def cook_and_run(self) -> None:
        package = self.cook_project()
        if package is None:
            return
        runtimes = runtime_candidates()
        if not runtimes:
            QMessageBox.warning(self, "Cook and Run",
                                "The engine runtime was not found. Build the engine first "
                                "(see the README), or use a packaged editor.")
            return
        if self.runtime_process is not None:
            self.runtime_process.kill()
        process = QProcess(self)
        process.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
        process.readyReadStandardOutput.connect(
            lambda: [log.info("runtime: %s", line) for line in
                     bytes(process.readAllStandardOutput()).decode(errors="replace").splitlines()])
        process.finished.connect(lambda code, _status: log.info("Runtime exited (%d).", code))
        process.start(str(runtimes[0]), ["--package", str(package)])
        self.runtime_process = process
        log.info("Running %s", package)
    # ---- shutdown ----------------------------------------------------------------------
    def closeEvent(self, event: QCloseEvent) -> None:
        self.settings.set("layout_last", self._layout_record())
        if self.backups is not None:
            self.backups.backup_if_needed()
        super().closeEvent(event)
