# SPDX-License-Identifier: Apache-2.0
"""A drop-down list that fills itself again each time it opens: for choices that change
elsewhere (the Asset Library's GUI pictures, the Audio panel's playlists), so what was imported a
moment ago is there to choose. And previews for lists of the library's pictures and fonts: each
picture's thumbnail, and the picture itself when the pointer rests on it; each font's name in
the font itself, and a sample when the pointer rests on it."""
import html
from collections.abc import Callable
from pathlib import Path

from PySide6.QtCore import QSize, Qt, QUrl
from PySide6.QtGui import QFont, QFontDatabase, QImageReader
from PySide6.QtWidgets import QComboBox

PREVIEW_MOST = 256  # (a picture's preview: at most this across or down, in pixels)
SAMPLE = "The quick brown fox jumps over the lazy dog. 0123456789"


class FreshComboBox(QComboBox):
    def __init__(self, fill: Callable[[], None] | None = None):
        super().__init__()
        self.fill = fill  # fills the list again, keeping the choice (called before it opens)

    def showPopup(self) -> None:  # noqa: N802 (Qt's name)
        if self.fill is not None:
            self.blockSignals(True)
            try:
                self.fill()
            finally:
                self.blockSignals(False)
        super().showPopup()


def picture_tip(name: str, file: Path) -> str:
    """A tooltip showing a picture (scaled down to fit), with its name and size."""
    reader = QImageReader(str(file))
    size = reader.size()
    w, h = size.width(), size.height()
    if w <= 0 or h <= 0:
        return html.escape(name)
    scale = min(1.0, PREVIEW_MOST / max(w, h))
    url = QUrl.fromLocalFile(str(file)).toString()
    return (f"<b>{html.escape(name)}</b> ({w} x {h})<br>"
            f'<img src="{html.escape(url)}" width="{max(1, round(w * scale))}" '
            f'height="{max(1, round(h * scale))}">')


def decorate_pictures(box: QComboBox, files: Callable[[str], Path | None]) -> None:
    """Each item that is a picture (its data, a reference `files` finds): its thumbnail, and
    the picture when the pointer rests on it."""
    from .asset_window import small_icon
    box.setIconSize(QSize(24, 24))
    for i in range(box.count()):
        ref = box.itemData(i)
        file = files(ref) if isinstance(ref, str) and ref else None
        if file is None:
            continue
        icon = small_icon(file, 24)
        if icon is not None:
            box.setItemIcon(i, icon)
        box.setItemData(i, picture_tip(box.itemText(i), file), Qt.ItemDataRole.ToolTipRole)


_FAMILIES: dict[str, str] = {}


def font_family(file: Path) -> str:
    """A font file's family, the font loaded for the editor's own use ("": it can't be)."""
    key = str(file)
    if key not in _FAMILIES:
        found = QFontDatabase.addApplicationFont(key)
        families = QFontDatabase.applicationFontFamilies(found) if found >= 0 else []
        _FAMILIES[key] = families[0] if families else ""
    return _FAMILIES[key]


def regular_face(files: list[Path]) -> Path | None:
    """The plain face among a font family's files (not its bold or italic), else the first."""
    plain = [f for f in files
             if not any(w in f.stem.lower() for w in ("italic", "oblique", "bold"))]
    return (plain or files or [None])[0]


def decorate_fonts(box: QComboBox, files: Callable[[str], list[Path]]) -> None:
    """Each item that is a font (its data, a reference whose files `files` finds): its name in
    the font itself, and a sample when the pointer rests on it."""
    for i in range(box.count()):
        ref = box.itemData(i)
        face = regular_face(files(ref)) if isinstance(ref, str) and ref else None
        family = font_family(face) if face is not None else ""
        if not family:
            continue
        font = QFont(family)
        font.setPointSizeF(max(box.font().pointSizeF(), 9.0) + 1)
        box.setItemData(i, font, Qt.ItemDataRole.FontRole)
        box.setItemData(i, f"<b>{html.escape(box.itemText(i))}</b><br>"
                           f'<span style="font-family: \'{html.escape(family)}\'; '
                           f'font-size: 20pt">{SAMPLE}</span>', Qt.ItemDataRole.ToolTipRole)
