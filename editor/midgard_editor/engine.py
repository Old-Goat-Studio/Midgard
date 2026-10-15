# SPDX-License-Identifier: Apache-2.0
"""The _midgard engine module (roadmap §2, channel 3): the engine's own C++ code, called
in-process for anything that must match the game exactly.

It is optional. A packaged editor carries it in _internal/engine/; run from source it is
found in the newest build/<preset>/engine/. Without it the editor still works, and the
features that need it say so.
"""
import importlib
import logging
import sys
from functools import cache
from pathlib import Path
from types import ModuleType

from . import BUNDLE_DIR

log = logging.getLogger("midgard")
MODULE = "_midgard"


def candidate_dirs() -> list[Path]:
    """Folders that may hold _midgard, best first."""
    pattern = f"{MODULE}*{'.pyd' if sys.platform == 'win32' else '.so'}"
    built = sorted((BUNDLE_DIR / "build").glob(f"*/engine/{pattern}"),
                   key=lambda p: p.stat().st_mtime, reverse=True)
    return [BUNDLE_DIR / "engine"] + [p.parent for p in built]


@cache
def module() -> ModuleType | None:
    """The _midgard module, or None if it isn't available for this Python."""
    try:
        return importlib.import_module(MODULE)
    except ImportError:
        pass
    for folder in candidate_dirs():
        if not folder.is_dir():
            continue
        sys.path.insert(0, str(folder))
        try:
            return importlib.import_module(MODULE)
        except ImportError as e:  # built for another Python, or damaged
            log.debug("%s in %s did not load: %s", MODULE, folder, e)
        finally:
            sys.path.remove(str(folder))
    return None


def version() -> dict | None:
    """The engine's version facts, or None without the module."""
    m = module()
    return m.version() if m else None


def validate(package_dir: Path) -> dict | None:
    """The engine's check of a cooked package ({"ok", "errors", "warnings", "entities",
    "assets"}), or None without the module."""
    m = module()
    return m.validate(Path(package_dir)) if m else None
