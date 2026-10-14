# SPDX-License-Identifier: Apache-2.0
"""Midgard editor."""
import sys
from pathlib import Path

from .version import VERSION as __version__

# The repository root when run from source; the bundle's folder when packaged.
if getattr(sys, "frozen", False):
    BUNDLE_DIR = Path(getattr(sys, "_MEIPASS", Path(sys.executable).parent))
else:
    BUNDLE_DIR = Path(__file__).resolve().parent.parent.parent

SCHEMA_DIR = BUNDLE_DIR / "schema"

__all__ = ["BUNDLE_DIR", "SCHEMA_DIR", "__version__"]
