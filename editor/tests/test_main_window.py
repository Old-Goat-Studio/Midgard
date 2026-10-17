# SPDX-License-Identifier: Apache-2.0
from PySide6.QtCore import Qt

from midgard_editor.main_window import PANELS, MainWindow
from midgard_editor.project import GAME_TITLE_KEY, Project
from midgard_editor.settings import Settings


def test_title_without_a_project(window):
    assert window.windowTitle() == "Midgard"
    assert set(window.panels) == set(PANELS)
    assert not window.close_action.isEnabled()


def test_opening_a_project(window, project, settings):
    assert window.open_project(project.config_path)
    assert window.windowTitle() == "Midgard - HelloCube"
    assert settings.recent_projects()[0] == project.config_path.resolve()
    assert window.close_action.isEnabled()
    window.close_project()
    assert window.windowTitle() == "Midgard"


def test_bad_project_shows_a_message(window, tmp_path, monkeypatch):
    shown = []
    monkeypatch.setattr("midgard_editor.main_window.QMessageBox.warning",
                        lambda *args: shown.append(args[2]))
    bad = tmp_path / "Bad.config"
    bad.write_text("{broken", "utf-8")
    assert not window.open_project(bad)
    assert shown and "not valid JSON" in shown[0]


def test_inspector_edits_are_undoable(window, project, qtbot):
    window.open_project(project.config_path)
    form = window.inspector.settings
    form.title.setText("Cube Quest")
    form.title.editingFinished.emit()
    opened = window.project
    assert opened.string(GAME_TITLE_KEY) == "Cube Quest"
    assert window.undo_group.canUndo()
    window.undo_group.undo()
    assert opened.string(GAME_TITLE_KEY) == "Hello Cube"
    assert form.title.text() == "Hello Cube"


def test_layout_is_restored_after_restart(qtbot, settings):
    first = MainWindow(settings)
    qtbot.addWidget(first)
    first.panels["log"].toggleView(False)
    first.close()
    assert settings.get("layout_last")
    second = MainWindow(Settings(settings.dir))
    qtbot.addWidget(second)
    assert second.panels["log"].isClosed()
    second.reset_layout()
    assert not second.panels["log"].isClosed()


def test_saved_layouts(window, settings, monkeypatch):
    monkeypatch.setattr("midgard_editor.main_window.QInputDialog.getText",
                        lambda *args, **kw: ("Scripting", True))
    window.save_layout_as()
    assert "Scripting" in settings.get("layouts")
    window.panels["inspector"].toggleView(False)
    assert window.apply_layout(settings.get("layouts")["Scripting"], geometry=False)
    assert not window.panels["inspector"].isClosed()
    window.delete_layout("Scripting")
    assert "Scripting" not in settings.get("layouts")


def test_two_monitor_layout_needs_a_second_screen(window):
    # The offscreen test platform has a single screen, so the layout falls back to Default.
    assert window.two_monitor_layout() is False


def test_theme_choice_is_saved(window, settings):
    window.set_theme("qss:yggdrasil")
    assert settings.get("theme") == "qss:yggdrasil"
    window.set_theme("light")
    assert settings.get("theme") == "light"


def test_reopening_a_closed_project_from_recent(window, project):
    window.open_project(project.config_path)
    window.close_project()
    window.start.show_welcome(window.settings.recent_projects())
    assert window.start.recent.count() == 1
    assert Project.open(project.config_path).name == "HelloCube"


def test_play_pause_and_stop_are_pictures_named_in_tooltips(window):
    from PySide6.QtWidgets import QToolBar, QToolButton

    from midgard_editor import ICONS_DIR

    bar = window.findChild(QToolBar, "play_toolbar")
    assert bar.toolButtonStyle() == Qt.ToolButtonStyle.ToolButtonIconOnly
    for name in ("play", "pause", "stop"):
        assert (ICONS_DIR / f"{name}.png").is_file()
    tips = {}
    for action in (window.play_action, window.pause_action, window.stop_action):
        assert not action.icon().isNull()
        button = bar.widgetForAction(action)
        assert isinstance(button, QToolButton)
        tips[action.toolTip()] = button.toolButtonStyle()
    assert list(tips) == ["Play (F5)", "Pause / Resume (F6)", "Stop (Shift+F5)"]
