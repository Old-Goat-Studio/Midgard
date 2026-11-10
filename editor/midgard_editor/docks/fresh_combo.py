# SPDX-License-Identifier: Apache-2.0
"""A drop-down list that fills itself again each time it opens: for choices that change
elsewhere (the Asset Library's GUI pictures, the Audio panel's playlists), so what was imported a
moment ago is there to choose."""
from collections.abc import Callable

from PySide6.QtWidgets import QComboBox


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
