# SPDX-License-Identifier: Apache-2.0
from PySide6.QtCore import QByteArray, QPoint, QRect

from midgard_editor import layouts

LEFT = QRect(0, 0, 1920, 1080)
RIGHT = QRect(1920, 0, 2560, 1440)


def test_pack_round_trips():
    data = QByteArray(b"\x00\x01dock state\xff")
    assert layouts.unpack(layouts.pack(data)) == data


def test_window_on_a_connected_monitor_stays():
    window = QRect(2100, 100, 800, 600)
    assert layouts.rescued_position(window, [LEFT, RIGHT], LEFT) is None


def test_window_on_a_disconnected_monitor_is_rescued():
    window = QRect(2100, 100, 800, 600)  # it was on RIGHT, which is gone
    assert layouts.rescued_position(window, [LEFT], LEFT) == QPoint(40, 40)


def test_window_hanging_off_an_edge_with_its_title_bar_visible_stays():
    window = QRect(1700, 200, 800, 600)  # mostly off the right edge, title bar still reachable
    assert layouts.is_visible(window, [LEFT])


def test_window_whose_title_bar_is_above_every_screen_is_rescued():
    window = QRect(100, -500, 800, 400)
    assert not layouts.is_visible(window, [LEFT])
