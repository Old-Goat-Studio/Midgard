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


def drag_last_tab_to_the_front(window: MainWindow, panel: str) -> None:
    """Drags the area's last tab along its tab strip onto the first one, as a user would. (This
    way the dragged tab, travelling with the cursor, stays within the strip: dragged past its
    end it would come out of the strip and float.)"""
    bar = window.panels[panel].dockAreaWidget().titleBar().tabBar()
    last, first = bar.tab(bar.count() - 1), bar.tab(0)
    start = last.rect().center()
    origin = last.mapToGlobal(start)
    target = first.mapToGlobal(first.rect().center())
    QTest.mousePress(last, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, start)
    # The dragged tab travels with the cursor, so each step is placed from screen positions.
    for step in range(1, 21):  # a real mouse's moves arrive one by one
        QTest.mouseMove(last, last.mapFromGlobal(origin + (target - origin) * step / 20))
        QApplication.processEvents()
    end = last.mapFromGlobal(target)
    QTest.mouseRelease(last, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, end)


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
    assert tab_order(window, "viewport") == ["Viewport", "Script Editor", "Project Page", "Objects",
                                             "World Types", "Animation", "Audio",
                                             "Shader Editor", "Stats", "Menu Editor", "Input"]
    drag_last_tab_to_the_front(window, "viewport")
    drag_last_tab_to_the_front(window, "log")
    assert tab_order(window, "viewport") == ["Input", "Viewport", "Script Editor", "Project Page",
                                             "Objects", "World Types", "Animation", "Audio",
                                             "Shader Editor", "Stats", "Menu Editor"]
    assert tab_order(window, "log") == ["History", "Log"]
    assert not window.dock_manager.floatingWidgets()  # moved along the strip, not undocked
    window.close()

    again = open_window(qtbot, settings)
    assert tab_order(again, "viewport") == ["Input", "Viewport", "Script Editor", "Project Page",
                                             "Objects", "World Types", "Animation", "Audio",
                                             "Shader Editor", "Stats", "Menu Editor"]
    assert tab_order(again, "log") == ["History", "Log"]


def test_the_layout_is_kept_without_a_tidy_exit(qtbot, settings):
    window = open_window(qtbot, settings)
    drag_last_tab_to_the_front(window, "project")
    qtbot.waitUntil(lambda: settings.get("layout_last") is not None, timeout=5000)
    # No close: as if the editor crashed. The next one still has the new order.
    again = open_window(qtbot, settings)
    assert tab_order(again, "project") == ["Scenes", "Project", "Scene"]
