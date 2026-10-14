# SPDX-License-Identifier: Apache-2.0
"""Check that midgard_runtime --screenshot drew the demo scene (used in CI with lavapipe).

    python tools/check_render.py render.png

Looks at a few sample points: night-blue sky at the top, the gold cube near the centre, moss
ground near the bottom. Colours are compared loosely, because drivers differ slightly.
"""
import sys

from PIL import Image

EXPECTED = {
    # name: (x fraction, y fraction, (r, g, b))
    "sky (top left)": (0.05, 0.05, (20, 28, 46)),
    "cube (centre)": (0.48, 0.48, (155, 120, 20)),
    "ground (bottom middle)": (0.5, 0.92, (48, 72, 36)),
}
TOLERANCE = 45


def main(path: str) -> int:
    img = Image.open(path).convert("RGB")
    w, h = img.size
    failures = []
    for name, (fx, fy, want) in EXPECTED.items():
        got = img.getpixel((int(fx * (w - 1)), int(fy * (h - 1))))
        if any(abs(g - e) > TOLERANCE for g, e in zip(got, want, strict=True)):
            failures.append(f"{name}: expected about {want}, got {got}")
        else:
            print(f"ok   {name}: {got}")
    for f in failures:
        print(f"FAIL {f}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
