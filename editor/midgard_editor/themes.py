# SPDX-License-Identifier: Apache-2.0
"""View ▸ Themes, following the Theme Scheme (midgard_roadmap.md §11).

System Light and System Dark follow Qt's own colour scheme. System Light keeps the desktop's
native look; System Dark and the .qss themes use Qt's Fusion style, because the native Windows
style ignores the dark colour scheme and the .qss themes are written for Fusion. Every .qss file
in the bundled
Themes folder, or in the user's <AppData>/Midgard/Themes folder, is a further theme named after
its file; a user theme with the same name replaces the bundled one. A theme can bring images in
a <name>-icons folder beside its .qss file, loaded as url(renoir:<file>).
"""
from pathlib import Path

from PySide6.QtCore import QDir, Qt
from PySide6.QtGui import QGuiApplication
from PySide6.QtWidgets import QApplication, QStyleFactory, QWidget

from . import THEMES_DIR

THEMES = {
    "light": ("System &Light", Qt.ColorScheme.Light),
    "dark": ("System &Dark", Qt.ColorScheme.Dark),
}
QSS_PREFIX = "qss:"
# Renoir-generated themes load arrow images as url(renoir:...) from a "<name>-icons" folder.
ICON_SEARCH_PREFIX = "renoir"
FUSION = "Fusion"


def theme_title(name: str) -> str:
    """Menu label for a .qss file stem: "solarized-light" -> "Solarized Light"."""
    words = name.replace("-", " ").replace("_", " ").split()
    return " ".join(w[0].upper() + w[1:] for w in words).replace("&", "&&")


def qss_themes(user_dir: Path | None = None) -> dict[str, Path]:
    """.qss themes by name, sorted by menu label. User themes replace bundled ones."""
    found: dict[str, Path] = {}
    for folder in (THEMES_DIR, user_dir / "Themes" if user_dir else None):
        if folder and folder.is_dir():
            for path in folder.glob("*.qss"):
                found[path.stem] = path
    return dict(sorted(found.items(), key=lambda item: theme_title(item[0]).casefold()))


class ThemeApplier:
    """Applies themes to the application and keeps the docking system's own stylesheet out of
    the way of .qss themes (a widget's own stylesheet would otherwise override the theme)."""

    def __init__(self, user_dir: Path | None = None, dock_manager: QWidget | None = None):
        self.user_dir = user_dir
        self.dock_manager = dock_manager
        self._dock_default_qss = dock_manager.styleSheet() if dock_manager else ""
        # The desktop's own style, before any theme. (Under a stylesheet Qt reports an empty
        # style name, so the style in use is tracked here rather than asked for.)
        app = QApplication.instance()
        self.native_style = (app.style().name() if app is not None else "") or FUSION
        self.style = self.native_style

    def _use_style(self, name: str) -> None:
        available = {style.casefold() for style in QStyleFactory.keys()}  # noqa: SIM118 - a list
        if self.style.casefold() != name.casefold() and name.casefold() in available:
            QApplication.instance().setStyle(name)
            self.style = name

    def apply(self, key: str | None) -> bool:
        """System themes set the colour scheme; .qss themes set the app stylesheet on top of the
        desktop's own scheme. Returns False if the key is unknown or its file is gone."""
        app = QApplication.instance()
        if key in THEMES:
            self._use_style(FUSION if key == "dark" else self.native_style)
            QGuiApplication.styleHints().setColorScheme(THEMES[key][1])
            app.setStyleSheet("")
            self._dock_stylesheet(self._dock_default_qss)
            return True
        if key and key.startswith(QSS_PREFIX):
            path = qss_themes(self.user_dir).get(key[len(QSS_PREFIX):])
            if path is None:
                return False
            try:
                stylesheet = path.read_text(encoding="utf-8")
            except OSError:
                return False
            icons = path.with_name(f"{path.stem}-icons")
            QDir.setSearchPaths(ICON_SEARCH_PREFIX, [str(icons)] if icons.is_dir() else [])
            self._use_style(FUSION)
            QGuiApplication.styleHints().unsetColorScheme()
            self._dock_stylesheet("")
            app.setStyleSheet(stylesheet)
            return True
        return False

    def _dock_stylesheet(self, qss: str) -> None:
        if self.dock_manager is not None:
            self.dock_manager.setStyleSheet(qss)
