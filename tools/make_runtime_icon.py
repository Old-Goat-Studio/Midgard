# SPDX-License-Identifier: Apache-2.0
"""Write engine/runtime/midgard.ico: the runtime's built-in icon (the Midgard logo) as
uncompressed 32-bit images from 16 to 256 pixels. Exports overwrite these images in place with a
game's own icon (editor/midgard_editor/winicon.py), so they must stay uncompressed.

    python tools/make_runtime_icon.py        (needs PySide6; run again if the logo changes)
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "editor"))


def main() -> int:
    from PySide6.QtGui import QGuiApplication, QImage

    from midgard_editor.winicon import ico_dib_bytes

    app = QGuiApplication.instance() or QGuiApplication([])  # noqa: F841 - QImage scaling
    image = QImage(str(ROOT / "midgard512.png"))
    if image.isNull():
        sys.exit("midgard512.png not found")
    out = ROOT / "engine" / "runtime" / "midgard.ico"
    out.write_bytes(ico_dib_bytes(image))
    print(f"{out} ({out.stat().st_size:,} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
