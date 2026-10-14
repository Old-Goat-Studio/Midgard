# SPDX-License-Identifier: Apache-2.0
"""Midgard editor."""
import sys
from pathlib import Path

from .version import VERSION as __version__

APP_NAME = "Midgard"
DESCRIPTION = "A game engine with an action-focused editor"

# The repository root when run from source; the bundle's _internal folder when packaged.
if getattr(sys, "frozen", False):
    BUNDLE_DIR = Path(getattr(sys, "_MEIPASS", Path(sys.executable).parent))
else:
    BUNDLE_DIR = Path(__file__).resolve().parent.parent.parent


def _source_dir() -> Path | None:
    """The project folder a packaged build was made from (recorded by tools/package.py), so a
    packaged editor uses the live Themes folder. None if it no longer exists."""
    if not getattr(sys, "frozen", False):
        return BUNDLE_DIR
    try:
        source = Path((BUNDLE_DIR / "source_dir.txt").read_text(encoding="utf-8").strip())
    except OSError:
        return None
    return source if source.is_dir() else None


_SOURCE_DIR = _source_dir()
SCHEMA_DIR = BUNDLE_DIR / "schema"
THEMES_DIR = (_SOURCE_DIR or BUNDLE_DIR) / "Themes"
ICON_PATH = BUNDLE_DIR / "midgard512.png"

__all__ = ["APP_NAME", "BUNDLE_DIR", "DESCRIPTION", "ICON_PATH", "SCHEMA_DIR", "THEMES_DIR",
           "__version__"]
