# SPDX-License-Identifier: Apache-2.0
"""One editor at a time: started again, Midgard hands over to the running one and quits."""
import subprocess
import sys
import textwrap
from pathlib import Path

from midgard_editor.app import bring_forward
from midgard_editor.single_instance import LOCK_FILE, SingleInstance, server_name

FIRST = """
import sys, time
sys.path.insert(0, {editor!r})
from pathlib import Path
from PySide6.QtCore import QCoreApplication, QTimer
from midgard_editor.single_instance import SingleInstance
app = QCoreApplication([])
first = SingleInstance(Path({folder!r}))
print("claimed", first.claim(), flush=True)
first.asked.connect(lambda m: print("asked", m.get("project"), flush=True))
time.sleep({busy})  # (busy, as when opening a big project)
QTimer.singleShot({stay}, app.quit)
app.exec()
"""


def a_first_editor(folder: Path, busy: float = 0.0, stay_ms: int = 3000):
    """Another editor (a process of its own) holding the lock, printing what it is asked."""
    script = FIRST.format(editor=str(Path(__file__).parents[1]), folder=str(folder),
                          busy=busy, stay=stay_ms)
    first = subprocess.Popen([sys.executable, "-c", script], stdout=subprocess.PIPE, text=True)
    assert first.stdout.readline().strip() == "claimed True"
    return first


def test_the_second_start_hands_over_to_the_first(qtbot, tmp_path):
    first = a_first_editor(tmp_path, busy=1.0)  # (busy at first: it hears when it's free)
    project = tmp_path / "Game" / "Game.config"
    second = SingleInstance(tmp_path)
    assert not second.claim(project)  # (another editor is running)
    assert second.reached
    third = SingleInstance(tmp_path)  # started again without a project: only to the front
    assert not third.claim()
    assert third.reached
    out, _ = first.communicate(timeout=30)
    assert out.splitlines() == [f"asked {project.resolve()}", "asked None"]
    # Closed: the next start is the editor.
    again = SingleInstance(tmp_path)
    assert again.claim()
    again.release()


def test_the_editor_itself_holds_the_lock_while_it_runs(qtbot, tmp_path):
    first = SingleInstance(tmp_path)
    assert first.claim()
    assert (tmp_path / LOCK_FILE).exists()
    first.release()
    assert SingleInstance(tmp_path).claim()


def test_each_settings_folder_has_its_own_editor(qtbot, tmp_path):
    a, b = SingleInstance(tmp_path / "a"), SingleInstance(tmp_path / "b")
    assert server_name(tmp_path / "a") != server_name(tmp_path / "b")
    try:
        assert a.claim() and b.claim()
    finally:
        a.release()
        b.release()


def test_a_crashed_editors_lock_is_taken_over(tmp_path):
    """An editor killed while holding the lock leaves it behind: the next start takes it."""
    script = textwrap.dedent(f"""
        import os, sys
        sys.path.insert(0, {str(Path(__file__).parents[1])!r})
        from PySide6.QtCore import QCoreApplication
        from midgard_editor.single_instance import SingleInstance
        app = QCoreApplication([])
        assert SingleInstance({str(tmp_path)!r}).claim()
        os._exit(3)  # (killed, holding it)
    """)
    run = subprocess.run([sys.executable, "-c", script], capture_output=True, text=True,
                         timeout=60)
    assert run.returncode == 3, run.stderr
    # (On Windows its lock file went with its process; elsewhere it's left, and seen as stale.)
    later = SingleInstance(tmp_path)
    assert later.claim()
    later.release()


def test_brought_forward_it_opens_another_project_but_not_the_same_one(qtbot, tmp_path,
                                                                       monkeypatch):
    from PySide6.QtWidgets import QWidget

    class Window(QWidget):
        def __init__(self):
            super().__init__()
            self.project = None
            self.opened = []

        def open_project(self, path):
            self.opened.append(Path(path))

    window = Window()
    qtbot.addWidget(window)
    window.showMinimized()
    bring_forward(window, {"project": None})
    assert not window.isMinimized() and window.isVisible()
    config = tmp_path / "Game.config"
    bring_forward(window, {"project": str(config)})
    qtbot.waitUntil(lambda: bool(window.opened), timeout=2000)
    assert window.opened == [config]

    class Open:
        config_path = config

    window.project = Open()
    bring_forward(window, {"project": str(config)})
    qtbot.wait(50)
    assert window.opened == [config]  # (already open: only to the front)
