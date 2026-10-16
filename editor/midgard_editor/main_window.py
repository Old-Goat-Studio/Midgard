# SPDX-License-Identifier: Apache-2.0
"""The Midgard main window: "Midgard - {project}", dockable panels across monitors, menus."""
import logging
from pathlib import Path

import PySide6QtAds as QtAds
from PySide6.QtCore import QRect, Qt
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

from . import APP_NAME, DESCRIPTION, ICON_PATH, vcs
from . import layouts as layout_store
from .about import AboutDialog, engine_info, runtime_candidates
from .backup import BackupScheduler
from .cook import CookError, cook
from .dialogs import CommitDialog, NewProjectDialog, PreferencesDialog, put_project_under_git
from .docks.assistant import AssistantPanel
from .docks.inspector import Inspector
from .docks.log import LOGGER_NAME, LogPanel
from .docks.project_browser import ProjectBrowser
from .docks.project_page import ProjectPage
from .docks.scene_panel import ScenePanel
from .docks.script_editor import ScriptEditorPanel
from .docks.viewport import ViewportPanel
from .export_dialog import ExportDialog
from .live_link import LinkError, PlaySession, State
from .physics_dialogs import PhysicsLayersDialog, WorldShapeDialog
from .project import Project, ProjectError
from .settings import Settings
from .themes import QSS_PREFIX, THEMES, ThemeApplier, qss_themes, theme_title

log = logging.getLogger(LOGGER_NAME)

# objectName -> title. Object names are the keys in saved layouts, so never rename them.
PANELS = {"project": "Project", "scene": "Scene", "inspector": "Inspector", "log": "Log",
          "history": "History", "viewport": "Viewport", "scripts": "Script Editor",
          "assistant": "Assistant", "project_page": "Project Page"}
BUILT_IN_LAYOUTS = ("Default", "Two Monitors")


def add_action(menu: QMenu, text: str, slot, shortcut=None) -> QAction:
    action = menu.addAction(text)
    action.triggered.connect(lambda _=False: slot())
    if shortcut is not None:
        action.setShortcut(QKeySequence(shortcut))
    return action


class StartPage(QWidget):
    """The central area: a start page with no project open, a short summary with one open (the
    Project Page panel has the details)."""

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
        tagline = f"<p><i>{project.game_tagline}</i></p>" if project.game_tagline else ""
        self.summary.setText(f"<h2>{project.game_title}</h2>{tagline}<p>Version "
                             f"{project.config.get('game_version')} · {project.root}</p>")
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
        self.session: PlaySession | None = None
        self.inspector = Inspector()
        self.log_panel = LogPanel()
        self.viewport = ViewportPanel()
        self.script_editor = ScriptEditorPanel(settings)
        self.project_page = ProjectPage()
        self.assistant = AssistantPanel(settings)
        self.script_editor.ask_assistant.connect(self.ask_assistant)
        self.script_editor.commit_requested.connect(self.commit)
        self.history = QUndoView(self.undo_group)
        self.history.setEmptyLabel("<project opened>")
        self.browser.file_selected.connect(self.inspector.show_selection)
        self.scene_panel.entity_selected.connect(self.inspector.show_entity)
        self.inspector.setMinimumWidth(340)
        self.inspector.open_script.connect(self.edit_script)
        self.browser.setMinimumWidth(220)
        widgets = {"project": self.browser, "scene": self.scene_panel, "inspector": self.inspector,
                   "log": self.log_panel, "history": self.history, "viewport": self.viewport,
                   "scripts": self.script_editor, "assistant": self.assistant,
                   "project_page": self.project_page}
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
        self._build_play_toolbar()
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
        self._place_viewport()
        self._place_script_editor()
        self._tab_behind_viewport("project_page")
        self._place_assistant()

    def _place_assistant(self) -> None:
        """The Assistant is a tab behind the Inspector."""
        area = self.panels["inspector"].dockAreaWidget()
        if area is not None:
            self.dock_manager.addDockWidgetTabToArea(self.panels["assistant"], area)
            self.panels["inspector"].setAsCurrentTab()
        else:
            self.dock_manager.addDockWidget(QtAds.DockWidgetArea.RightDockWidgetArea,
                                            self.panels["assistant"])

    def ask_assistant(self, code: str, file: str) -> None:
        self.panels["assistant"].toggleView(True)
        self.panels["assistant"].setAsCurrentTab()
        self.assistant.ask_about(code, file)

    def _place_script_editor(self) -> None:
        """The Script editor shares the Viewport's area, as a tab behind it."""
        self._tab_behind_viewport("scripts")

    def _tab_behind_viewport(self, key: str) -> None:
        area = self.panels["viewport"].dockAreaWidget()
        if area is not None:
            self.dock_manager.addDockWidgetTabToArea(self.panels[key], area)
            self.panels["viewport"].setAsCurrentTab()
        else:
            self.dock_manager.addDockWidget(QtAds.DockWidgetArea.RightDockWidgetArea,
                                            self.panels[key])

    def _place_viewport(self) -> None:
        """The Viewport goes above the central start page and takes most of its height."""
        central = self.dock_manager.centralWidget().dockAreaWidget()
        self.dock_manager.addDockWidget(QtAds.DockWidgetArea.TopDockWidgetArea,
                                        self.panels["viewport"], central)
        splitter = central.parentSplitter()
        if splitter is not None and splitter.count() == 2:
            total = max(sum(splitter.sizes()), 400)
            splitter.setSizes([total * 4 // 5, total - total * 4 // 5])

    def _layout_record(self) -> dict:
        return {"state": layout_store.pack(self.dock_manager.saveState()),
                "geometry": layout_store.pack(self.saveGeometry()),
                "panels": list(PANELS)}

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
                return
            # Panels added since the layout was saved go where the default layout puts them.
            saved = last.get("panels", [])
            if "viewport" not in saved:
                self._place_viewport()
            if "scripts" not in saved:
                self._place_script_editor()
            if "assistant" not in saved:
                self._place_assistant()
            if "project_page" not in saved:
                self._tab_behind_viewport("project_page")

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
        self.git_action = add_action(file, "Put Project Under &Git…", self.put_under_git)
        self.commit_action = add_action(file, "Co&mmit…", self.commit, "Ctrl+K")
        file.aboutToShow.connect(self._update_git_action)
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
        add_action(edit, "&Preferences…", self.show_preferences,
                   QKeySequence.StandardKey.Preferences)

        build = bar.addMenu("&Build")
        self.cook_action = add_action(build, "&Cook", self.cook_project, "Ctrl+B")
        build.addSeparator()
        self.play_action = add_action(build, "&Play", self.play, "F5")
        self.pause_action = add_action(build, "P&ause", self.toggle_pause, "F6")
        self.pause_action.setCheckable(True)
        self.stop_action = add_action(build, "&Stop", self.stop_game, "Shift+F5")
        build.addSeparator()
        self.export_action = add_action(build, "&Export…", self.export_game, "Ctrl+E")

        world = bar.addMenu("W&orld")
        self.world_shape_action = add_action(world, "World &Shape…", self.edit_world_shape)
        self.physics_layers_action = add_action(world, "&Physics Layers…",
                                                self.edit_physics_layers)

        view = bar.addMenu("&View")
        # The viewport toolbar's switch, in the menu too.
        colliders = self.viewport.colliders_action
        colliders.setChecked(bool(self.settings.get("show_colliders")))
        colliders.toggled.connect(lambda on: self.settings.set("show_colliders", on))
        view.addAction(colliders)
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

    def _build_play_toolbar(self) -> None:
        bar = self.addToolBar("Play")
        bar.setObjectName("play_toolbar")
        bar.setMovable(False)
        for action in (self.play_action, self.pause_action, self.stop_action):
            bar.addAction(action)
        self.play_status = QLabel()
        self.statusBar().addPermanentWidget(self.play_status)
        self._play_state_changed(State.STOPPED)

    def show_preferences(self) -> None:
        PreferencesDialog(self.settings, self).exec()
        self.assistant.refresh_state()

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
        if not self.unsaved_scripts_handled():
            return False
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
        if self.unsaved_scripts_handled():
            self.set_project(None)

    def unsaved_scripts_handled(self, can_cancel: bool = True) -> bool:
        """Asks about scripts with unsaved changes. False if the user cancelled."""
        modified = [e for e in self.script_editor.editors() if e.document().isModified()]
        if not modified:
            return True
        buttons = QMessageBox.StandardButton.Save | QMessageBox.StandardButton.Discard
        if can_cancel:
            buttons |= QMessageBox.StandardButton.Cancel
        names = ", ".join(e.path.name for e in modified)
        answer = QMessageBox.question(self, "Unsaved Scripts",
                                      f"Save the changes to {names}?", buttons)
        if answer == QMessageBox.StandardButton.Cancel:
            return False
        if answer == QMessageBox.StandardButton.Save:
            self.script_editor.save_all()
        return True

    def _update_git_action(self) -> None:
        enabled, text = vcs.status_text(self.project)
        self.git_action.setEnabled(enabled)
        self.git_action.setText(text)
        in_git = self.project is not None and vcs.repository_root(self.project.root) is not None
        self.commit_action.setEnabled(in_git)
        self.script_editor.commit_button.setEnabled(in_git)
        self.script_editor.commit_button.setToolTip(
            "Commit the project's changes to Git, with a message" if in_git else
            "The project isn't in Git yet: File ▸ Put Project Under Git…")

    def commit(self) -> None:
        """File ▸ Commit… and the Script editor's Commit… button."""
        if self.project is None:
            return
        if vcs.repository_root(self.project.root) is None:
            QMessageBox.information(self, "Commit", "The project isn't in Git yet. Choose "
                                    "File ▸ Put Project Under Git… first.")
            return
        self.script_editor.save_all()  # commit what is in the editor
        if not vcs.changed_files(self.project):
            QMessageBox.information(self, "Commit", "Nothing has changed since the last commit.")
            return
        if CommitDialog(self.project, self).exec():
            self._refresh_git_views()

    def _refresh_git_views(self) -> None:
        for editor in self.script_editor.editors():
            editor.refresh_git()
        self.script_editor._update_titles()
        self._update_git_action()

    def put_under_git(self) -> None:
        if self.project is not None and put_project_under_git(self.project, self):
            self._refresh_git_views()

    def edit_script(self, module: str) -> None:
        """Shows a script in the Script editor (from the Inspector's Edit button)."""
        if not module:
            return
        self.panels["scripts"].toggleView(True)
        self.panels["scripts"].setAsCurrentTab()
        self.script_editor.open_module(module)

    def _script_error(self, error: dict) -> None:
        """A script failed in the running game: show the line in the Script editor."""
        self.panels["scripts"].toggleView(True)
        self.panels["scripts"].setAsCurrentTab()
        self.script_editor.show_error(error.get("file", ""), int(error.get("line", 0)),
                                      error.get("message", ""))

    def edit_world_shape(self) -> None:
        if self.project is not None:
            WorldShapeDialog(self.project, self).exec()

    def edit_physics_layers(self) -> None:
        if self.project is not None:
            PhysicsLayersDialog(self.project, self).exec()

    def set_project(self, project: Project | None) -> None:
        if self.session is not None:
            self.session.stop()
            self.session.deleteLater()
            self.session = None
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
        self.viewport.set_project(project)
        self.script_editor.set_project(project)
        self.assistant.set_project(project)
        self.project_page.set_project(project)
        self._update_git_action()
        for action in (self.close_action, self.cook_action, self.export_action,
                       self.world_shape_action, self.physics_layers_action):
            action.setEnabled(project is not None)
        if project is not None:
            self.session = PlaySession(project, self)
            self.session.state_changed.connect(self._play_state_changed)
            self.session.stats_received.connect(self._play_stats)
            self.session.script_error.connect(self._script_error)
        self._play_state_changed(State.STOPPED)
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

    def export_game(self) -> None:
        if self.project is not None:
            ExportDialog(self.project, self.settings, self).exec()

    # ---- play-in-editor ----------------------------------------------------------------
    def play(self) -> None:
        """Cooks the project and runs it, linked to the editor: edits reach the running game."""
        if self.session is None or self.session.running:
            return
        self.script_editor.save_all()  # the game runs what is in the editor
        runtimes = runtime_candidates()
        if not runtimes:
            QMessageBox.warning(self, "Play",
                                "The engine runtime was not found. Build the engine first "
                                "(see the README), or use a packaged editor.")
            return
        package = self.cook_project()
        if package is None:
            return
        try:
            self.session.start(runtimes[0], package)
        except LinkError as e:
            QMessageBox.warning(self, "Play", str(e))
            return
        log.info("Playing %s", package)

    def toggle_pause(self) -> None:
        if self.session is None:
            return
        if self.session.state == State.PAUSED:
            self.session.resume()
        else:
            self.session.pause()

    def stop_game(self) -> None:
        if self.session is not None and self.session.running:
            self.session.stop()

    def _play_state_changed(self, state: State) -> None:
        has_project = self.session is not None
        self.play_action.setEnabled(has_project and state == State.STOPPED)
        self.pause_action.setEnabled(state in (State.PLAYING, State.PAUSED))
        self.pause_action.setChecked(state == State.PAUSED)
        self.stop_action.setEnabled(state != State.STOPPED)
        self.play_status.setText({State.STOPPED: "", State.STARTING: "Starting the game…",
                                  State.PLAYING: "Playing", State.PAUSED: "Paused"}[state])

    def _play_stats(self, stats: dict) -> None:
        state = "Paused" if stats.get("paused") else "Playing"
        self.play_status.setText(f"{state} · {stats.get('fps', 0)} fps · "
                                 f"{stats.get('draw_calls', 0)} draws")
    # ---- shutdown ----------------------------------------------------------------------
    def closeEvent(self, event: QCloseEvent) -> None:
        if not self.unsaved_scripts_handled():
            event.ignore()
            return
        self.assistant.cancel()
        if self.session is not None:
            self.session.stop()
        self.settings.set("layout_last", self._layout_record())
        if self.backups is not None:
            self.backups.backup_if_needed()
        super().closeEvent(event)
