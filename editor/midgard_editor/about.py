# SPDX-License-Identifier: Apache-2.0
"""Help ▸ About Midgard, following the About Scheme (midgard_roadmap.md §12)."""
import os
import re
import subprocess
from dataclasses import dataclass
from datetime import date
from functools import cache
from pathlib import Path

from PySide6.QtCore import Qt
from PySide6.QtGui import QPixmap
from PySide6.QtWidgets import QDialog, QDialogButtonBox, QLabel, QVBoxLayout, QWidget

from . import APP_NAME, BUNDLE_DIR, DESCRIPTION, ICON_PATH, version

RUNTIME_NAME = "midgard_runtime.exe" if os.name == "nt" else "midgard_runtime"
_RUNTIME_VERSION = re.compile(
    r"Midgard runtime (\S+) \(engine ABI (\d+), package format (\d+)\.x")


@dataclass(frozen=True)
class EngineInfo:
    version: str
    abi: int
    format_major: int


def runtime_candidates() -> list[Path]:
    """Where the engine runtime may be: bundled with a packaged editor, or built from source."""
    bundled = [BUNDLE_DIR / "engine" / RUNTIME_NAME]
    built = sorted((BUNDLE_DIR / "build").glob(f"*/engine/{RUNTIME_NAME}"),
                   key=lambda p: p.stat().st_mtime, reverse=True)
    return [p for p in bundled + built if p.is_file()]


def parse_runtime_version(text: str) -> EngineInfo | None:
    m = _RUNTIME_VERSION.search(text)
    return EngineInfo(m.group(1), int(m.group(2)), int(m.group(3))) if m else None


@cache
def engine_info() -> EngineInfo | None:
    """The engine's version: from the _midgard module, or else by asking the runtime."""
    from . import engine

    if v := engine.version():
        return EngineInfo(v["engine"], v["engine_abi"], v["format_major"])
    for runtime in runtime_candidates():
        try:
            out = subprocess.run(
                [str(runtime), "--version"], capture_output=True, text=True, timeout=5,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))  # no console flash
        except (OSError, subprocess.TimeoutExpired):
            continue
        if info := parse_runtime_version(out.stdout):
            return info
    return None


def engine_line(info: EngineInfo | None) -> tuple[str, bool]:
    """The engine line for the About box, and whether it matches the editor."""
    if info is None:
        return "Engine not found", False
    text = f"Engine v{info.version} · package format {info.format_major}.x"
    if info.version != version.VERSION:
        return text + " (mismatch)", False
    return text, True


def build_date_text(iso: str | None) -> str | None:
    if not iso:
        return None
    d = date.fromisoformat(iso)
    return f"{d:%B} {d.day}, {d.year}"


class AboutDialog(QDialog):
    def __init__(self, parent: QWidget | None = None, engine: EngineInfo | None = None):
        super().__init__(parent)
        self.setWindowTitle(f"About {APP_NAME}")
        layout = QVBoxLayout(self)
        layout.setSpacing(10)
        layout.setContentsMargins(32, 24, 32, 16)

        name = QLabel(APP_NAME)
        font = name.font()
        font.setPointSizeF(font.pointSizeF() * 1.8)
        font.setBold(True)
        name.setFont(font)
        icon = QLabel()
        icon.setPixmap(QPixmap(str(ICON_PATH)).scaled(
            128, 128, Qt.AspectRatioMode.KeepAspectRatio,
            Qt.TransformationMode.SmoothTransformation))
        engine_text, engine_ok = engine_line(engine)
        self.engine_label = QLabel(engine_text)
        # A mismatch uses the theme's disabled colour, never a fixed red (Theme Scheme).
        self.engine_label.setEnabled(engine_ok)
        self.version_label = QLabel(f"v{version.VERSION} (build {version.BUILD})")
        lines = [name, QLabel(DESCRIPTION), icon, self.version_label, self.engine_label]
        if built := build_date_text(version.BUILD_DATE):
            lines.append(QLabel(built))
        for w in lines:
            w.setAlignment(Qt.AlignmentFlag.AlignCenter)
            layout.addWidget(w)

        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok)
        buttons.setCenterButtons(True)
        buttons.accepted.connect(self.accept)
        layout.addSpacing(6)
        layout.addWidget(buttons)
        # Widen to the golden ratio (1.6:1) so the title bar has room for its title.
        hint = self.sizeHint()
        self.setFixedSize(max(hint.width(), round(hint.height() * 1.6)), hint.height())
