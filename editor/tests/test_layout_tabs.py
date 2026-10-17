# SPDX-License-Identifier: Apache-2.0
"""Tabs dragged into a new order stay in it: across restarts, and even if the editor never got
to close tidily (the layout is kept as it changes)."""
from PySide6.QtCore import Qt
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication

from midgard_editor.main_window import MainWindow


def tab_order(window: MainWindow, panel: str) -> list[str]:
    bar = window.panels[panel].dockAreaWidget().titleBar().tabBar()
    return [bar.tab(i).text() for i in range(bar.count())]


def drag_first_tab_to_the_end(window: MainWindow, panel: str) -> None:
    """Drags the area's first tab along its tab strip onto the last one, as a user would."""
    bar = window.panels[panel].dockAreaWidget().titleBar().tabBar()
    first, last = bar.tab(0), bar.tab(bar.count() - 1)
    start = first.rect().center()
    origin = first.mapToGlobal(start)
    target = last.mapToGlobal(last.rect().center())
    QTest.mousePress(first, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, start)
    # The dragged tab travels with the cursor, so each step is placed from screen positions.
    for step in range(1, 21):  # a real mouse's moves arrive one by one
        QTest.mouseMove(first, first.mapFromGlobal(origin + (target - origin) * step / 20))
        QApplication.processEvents()
    end = first.mapFromGlobal(target)
    QTest.mouseRelease(first, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, end)


def open_window(qtbot, settings) -> MainWindow:
    window = MainWindow(settings)
    qtbot.addWidget(window)
    window.resize(1600, 1000)
    window.show()
    qtbot.waitExposed(window)
    qtbot.wait(100)  # the docks lay their tabs out
    return window


def test_dragged_tabs_keep_their_order_after_a_restart(qtbot, settings):
    window = open_window(qtbot, settings)
    assert tab_order(window, "viewport") == ["Viewport", "Script Editor", "Project Page", "Objects"]
    drag_first_tab_to_the_end(window, "viewport")
    drag_first_tab_to_the_end(window, "log")
    assert tab_order(window, "viewport") == ["Script Editor", "Project Page", "Objects", "Viewport"]
    assert tab_order(window, "log") == ["History", "Log"]
    assert not window.dock_manager.floatingWidgets()  # moved along the strip, not undocked
    window.close()

    again = open_window(qtbot, settings)
    assert tab_order(again, "viewport") == ["Script Editor", "Project Page", "Objects", "Viewport"]
    assert tab_order(again, "log") == ["History", "Log"]


def test_the_layout_is_kept_without_a_tidy_exit(qtbot, settings):
    window = open_window(qtbot, settings)
    drag_first_tab_to_the_end(window, "project")
    qtbot.waitUntil(lambda: settings.get("layout_last") is not None, timeout=5000)
    # No close: as if the editor crashed. The next one still has the new order.
    again = open_window(qtbot, settings)
    assert tab_order(again, "project") == ["Scene", "Project"]
