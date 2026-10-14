# SPDX-License-Identifier: Apache-2.0
"""Entry script for the packaged editor (PyInstaller needs a plain script, not a package)."""
import sys

from midgard_editor.__main__ import main

sys.exit(main())
