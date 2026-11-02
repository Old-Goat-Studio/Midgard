# SPDX-License-Identifier: Apache-2.0
"""error.log (FixMe): the last 100 keys and clicks (typing counted, never kept; nothing from a
password field), errors (the Log's, Python's, Qt's, the game's; the last 50), a hard crash's
traceback; in the project's folder (or the settings folder), left out of Git; turned off from
Preferences or the Project Page."""
import logging
import subprocess
import sys
import textwrap

import pytest
from PySide6.QtCore import Qt
from PySide6.QtWidgets import QLineEdit, QPushButton, QVBoxLayout, QWidget

from midgard_editor.error_log import EVENTS, ErrorLog


def window_with(qtbot):
    w = QWidget()
    w.setWindowTitle("Test Window")
    layout = QVBoxLayout(w)
    name, secret, button = QLineEdit(), QLineEdit(), QPushButton("Save")
    name.setAccessibleName("Name")
    secret.setAccessibleName("API key")
    secret.setEchoMode(QLineEdit.EchoMode.Password)
    for x in (name, secret, button):
        layout.addWidget(x)
    qtbot.addWidget(w)
    w.show()
    qtbot.waitExposed(w)
    return w, name, secret, button


def test_keys_and_clicks_are_recorded_but_never_what_is_typed(qtbot, tmp_path):
    log = ErrorLog(tmp_path)
    log.set_enabled(True)
    try:
        w, name, secret, button = window_with(qtbot)
        name.setFocus()
        qtbot.keyClicks(name, "Hello")
        qtbot.keyClick(name, Qt.Key.Key_S, Qt.KeyboardModifier.ControlModifier)
        secret.setFocus()
        qtbot.keyClicks(secret, "sk-secret")
        qtbot.mouseClick(button, Qt.MouseButton.LeftButton)
        log.write()
        text = (tmp_path / "error.log").read_text("utf-8")
    finally:
        log.set_enabled(False)
    assert 'typed 5 characters in LineEdit "Name" in Test Window' in text
    assert 'key Ctrl+S in LineEdit "Name"' in text
    assert 'left click on PushButton "Save" in Test Window' in text
    assert "Hello" not in text and "secret" not in text and "API key" not in text


def test_only_the_last_100_events_and_50_errors(qtbot, tmp_path):
    log = ErrorLog(tmp_path)
    log.set_enabled(True)
    try:
        for i in range(EVENTS + 20):
            log.record_event(f"event {i}")
        for i in range(70):
            log.record_error("ERROR", f"problem {i}")
        text = (tmp_path / "error.log").read_text("utf-8")
    finally:
        log.set_enabled(False)
    assert "event 19\n" not in text and "event 20\n" in text and "event 119" in text
    assert "problem 19\n" not in text and "problem 20\n" in text and "problem 69" in text


@pytest.mark.qt_no_exception_capture  # (its Python error is passed on, as it should be)
def test_errors_from_the_log_python_qt_and_the_game(qtbot, tmp_path):
    from PySide6.QtCore import qWarning
    log = ErrorLog(tmp_path)
    log.set_enabled(True)
    try:
        logging.getLogger("midgard").warning("Something odd: %s", "the bench")
        logging.getLogger("midgard").info("Just saying")  # (not an error)
        logging.getLogger("midgard").info("game: error: the scene has no camera")
        logging.getLogger("midgard").info("The game exited (3).")
        logging.getLogger("midgard").info("The game exited (0).")
        try:
            raise ValueError("a bad value")
        except ValueError:
            sys.excepthook(*sys.exc_info())
        qWarning("a Qt warning")
        text = (tmp_path / "error.log").read_text("utf-8")
    finally:
        log.set_enabled(False)
    assert "WARNING: Something odd: the bench" in text
    assert "Just saying" not in text
    assert "GAME: error: the scene has no camera" in text
    assert "GAME: The game exited (3)." in text and "exited (0)" not in text
    assert "ERROR: Python: Traceback" in text and "ValueError: a bad value" in text
    assert "WARNING: Qt: a Qt warning" in text


def test_it_follows_the_project_and_keeps_out_of_git(qtbot, tmp_path, project):
    (project.root / ".gitignore").write_text(".midgard/\n", "utf-8")
    hook_before = sys.excepthook
    log = ErrorLog(tmp_path / "settings")
    log.set_enabled(True)
    try:
        log.record_event("before")
        assert (tmp_path / "settings" / "error.log").is_file()
        log.set_folder(project.root)
        log.record_error("ERROR", "in the project")
        assert "in the project" in (project.root / "error.log").read_text("utf-8")
        assert (project.root / ".gitignore").read_text("utf-8").split() == [".midgard/",
                                                                             "error.log"]
        log.set_folder(project.root)  # (again: not added twice)
        assert (project.root / ".gitignore").read_text("utf-8").count("error.log") == 1
    finally:
        log.set_enabled(False)
    # Off: nothing more is written, and the hooks are as they were before.
    (project.root / "error.log").unlink()
    log.record_error("ERROR", "while off")
    assert not (project.root / "error.log").exists()
    assert sys.excepthook is hook_before


def test_a_hard_crash_leaves_its_traceback(tmp_path):
    script = textwrap.dedent(f"""
        import faulthandler, os, sys
        os.environ["QT_QPA_PLATFORM"] = "offscreen"
        sys.path.insert(0, {str(__import__('pathlib').Path(__file__).parents[1])!r})
        from PySide6.QtWidgets import QApplication
        from midgard_editor.error_log import ErrorLog
        app = QApplication([])
        log = ErrorLog({str(tmp_path)!r})
        log.set_enabled(True)
        log.record_event("the last thing before the crash")
        faulthandler._sigsegv()
    """)
    run = subprocess.run([sys.executable, "-c", script], capture_output=True, text=True,
                         timeout=60)
    assert run.returncode != 0  # (it crashed)
    text = (tmp_path / "error.log").read_text("utf-8")
    assert "the last thing before the crash" in text
    assert "Fatal Python error" in text


def test_preferences_and_the_project_page_turn_it_off(qtbot, settings, project):
    from midgard_editor.dialogs import PreferencesDialog
    from midgard_editor.main_window import MainWindow
    window = MainWindow(settings)
    qtbot.addWidget(window)
    window.set_project(project)
    assert window.error_log.enabled  # (on unless turned off)
    assert window.error_log.path == project.root / "error.log"
    window.project_page.error_log.setChecked(False)  # this project: off
    assert project.config["error_log"] is False and not window.error_log.enabled
    window.project_page.error_log.setChecked(True)
    assert window.error_log.enabled
    prefs = PreferencesDialog(settings)
    qtbot.addWidget(prefs)
    prefs.error_log.setChecked(False)  # every project: off
    prefs._save()
    window.apply_error_log()
    assert not window.error_log.enabled
    window.close()
