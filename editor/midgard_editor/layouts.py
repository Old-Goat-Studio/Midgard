# SPDX-License-Identifier: Apache-2.0
"""Saved panel layouts, and keeping windows on monitors that still exist.

A layout is the docking system's saveState() plus the main window geometry, stored base64 in
midgard.json ("layout_last" for the session, "layouts" for named ones).
"""
from PySide6.QtCore import QByteArray, QPoint, QRect
from PySide6.QtGui import QGuiApplication
from PySide6.QtWidgets import QWidget

OFFSET = QPoint(40, 40)


def pack(data: QByteArray) -> str:
    return bytes(data.toBase64()).decode("ascii")


def unpack(text: str) -> QByteArray:
    return QByteArray.fromBase64(text.encode("ascii"))


def is_visible(rect: QRect, screens: list[QRect], min_overlap: int = 64) -> bool:
    """True if a usable part of the window (at least min_overlap × 32 px of its top edge, where
    the title bar is) lies on one of the screens."""
    title_bar = QRect(rect.x(), rect.y(), rect.width(), 32)
    return any((title_bar & s).width() >= min(min_overlap, rect.width()) and
               (title_bar & s).height() > 0 for s in screens)


def rescued_position(rect: QRect, screens: list[QRect], primary: QRect) -> QPoint | None:
    """Where to move a window whose monitor has gone, or None if it is still visible."""
    if is_visible(rect, screens):
        return None
    return primary.topLeft() + OFFSET


def keep_on_screen(widget: QWidget) -> bool:
    """Move a top-level window back onto the primary screen if its monitor has gone (for
    example a laptop taken away from its dock). Returns True if it was moved."""
    screens = [s.availableGeometry() for s in QGuiApplication.screens()]
    primary = QGuiApplication.primaryScreen().availableGeometry()
    target = rescued_position(widget.frameGeometry(), screens, primary)
    if target is None:
        return False
    widget.move(target)
    return True
