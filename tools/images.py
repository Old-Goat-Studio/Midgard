# SPDX-License-Identifier: Apache-2.0
"""Image tests: render scenes with midgard_runtime and compare them with reference images.

    python tools/images.py run --runtime build/linux-release/engine/midgard_runtime [--out DIR]
    python tools/images.py update --runtime ...     write new reference images (review them!)
    python tools/images.py compare actual.png reference.png [--diff diff.png]

The scenes are listed in engine/tests/images/scenes.json, each with the runtime arguments that
draw it, and the reference images sit beside it (<name>.png). A scene can instead be compared
with this run's image of an earlier scene ("same_as": "demo"), exactly by default: the same
driver drew both, so ways of drawing that must look the same are held to every pixel.
CI runs them on Linux with software Vulkan (Mesa's lavapipe), so no GPU is needed.

Graphics drivers round slightly differently, so two images match when no colour channel differs
by more than `tolerance` (0-255) except in at most `max_bad` of the pixels (a fraction). A scene
can set its own values. When a change to the renderer is meant to change the pictures, run
`update` and review the new references like code.
"""
import argparse
import json
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parent.parent
IMAGES = ROOT / "engine" / "tests" / "images"
TOLERANCE = 6
MAX_BAD = 0.002


@dataclass
class Comparison:
    ok: bool
    message: str
    bad_pixels: int = 0
    total_pixels: int = 0
    max_difference: int = 0


def compare(actual: Path, reference: Path, tolerance: int = TOLERANCE, max_bad: float = MAX_BAD,
            diff: Path | None = None) -> Comparison:
    """Compares two images; with `diff`, also writes a picture of where they differ (the
    reference in grey, differing pixels in red)."""
    a = Image.open(actual).convert("RGB")
    r = Image.open(reference).convert("RGB")
    if a.size != r.size:
        return Comparison(False, f"size {a.size[0]}x{a.size[1]}, expected "
                                 f"{r.size[0]}x{r.size[1]}")
    channels = ImageChops.difference(a, r).split()
    worst = ImageChops.lighter(ImageChops.lighter(channels[0], channels[1]), channels[2])
    max_difference = worst.getextrema()[1]
    mask = worst.point(lambda v: 255 if v > tolerance else 0)
    bad = mask.histogram()[255]
    total = a.size[0] * a.size[1]
    ok = bad <= max_bad * total
    if diff is not None:
        picture = Image.merge("RGB", [r.convert("L")] * 3)
        picture.paste((255, 0, 0), mask=mask)
        picture.save(diff)
    message = (f"{bad} of {total} pixels ({100 * bad / total:.2f}%) differ by more than "
               f"{tolerance}; the largest difference is {max_difference}")
    return Comparison(ok, message, bad, total, max_difference)


def scenes() -> list[dict]:
    return json.loads((IMAGES / "scenes.json").read_text("utf-8"))["scenes"]


def render(runtime: list[str], scene: dict, out: Path) -> tuple[bool, str]:
    """Draws one scene to `out`. Returns (ok, the runtime's output)."""
    run = subprocess.run([*runtime, *scene["args"], "--screenshot", str(out)],
                         capture_output=True, text=True, timeout=120)
    text = (run.stdout + run.stderr).strip()
    return run.returncode == 0 and out.is_file(), text


def _report(name: str, ok: bool, message: str) -> None:
    print(f"{'ok  ' if ok else 'FAIL'} {name}: {message}")
    if not ok and os.environ.get("GITHUB_ACTIONS"):
        print(f"::error title=Image test {name}::{message}")


def run_all(runtime: list[str], out_dir: Path) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    failures = 0
    for scene in scenes():
        name = scene["name"]
        actual = out_dir / f"{name}.png"
        drawn, output = render(runtime, scene, actual)
        if not drawn:
            _report(name, False, f"the runtime didn't draw it: {output}")
            failures += 1
            continue
        if "same_as" in scene:
            reference = out_dir / f"{scene['same_as']}.png"
            tolerance, max_bad = scene.get("tolerance", 0), scene.get("max_bad", 0.0)
        else:
            reference = IMAGES / f"{name}.png"
            tolerance, max_bad = scene.get("tolerance", TOLERANCE), scene.get("max_bad", MAX_BAD)
        if not reference.is_file():
            missing = (f"scene {scene['same_as']} wasn't drawn before it" if "same_as" in scene
                       else f"no reference image ({reference.relative_to(ROOT)}); "
                            "run tools/images.py update")
            _report(name, False, missing)
            failures += 1
            continue
        result = compare(actual, reference, tolerance, max_bad, out_dir / f"{name}.diff.png")
        _report(name, result.ok, result.message)
        failures += not result.ok
    print(f"{len(scenes()) - failures} of {len(scenes())} image tests passed")
    return 1 if failures else 0


def update_all(runtime: list[str]) -> int:
    for scene in scenes():
        if "same_as" in scene:
            continue  # compared with another scene's image, so it has none of its own
        reference = IMAGES / f"{scene['name']}.png"
        drawn, output = render(runtime, scene, reference)
        print(f"{'wrote' if drawn else 'FAILED'} {reference.relative_to(ROOT)}"
              + ("" if drawn else f": {output}"))
        if not drawn:
            return 1
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("run", "update"):
        p = sub.add_parser(name)
        p.add_argument("--runtime", required=True, help="the midgard_runtime executable")
        if name == "run":
            p.add_argument("--out", type=Path, default=ROOT / "build" / "images",
                           help="where the rendered images and diffs go")
    p = sub.add_parser("compare")
    p.add_argument("actual", type=Path)
    p.add_argument("reference", type=Path)
    p.add_argument("--diff", type=Path)
    p.add_argument("--tolerance", type=int, default=TOLERANCE)
    p.add_argument("--max-bad", type=float, default=MAX_BAD)
    args = parser.parse_args(argv)
    if args.command == "compare":
        result = compare(args.actual, args.reference, args.tolerance, args.max_bad, args.diff)
        _report(args.actual.name, result.ok, result.message)
        return 0 if result.ok else 1
    if args.command == "run":
        return run_all([str(Path(args.runtime).resolve())], args.out)
    return update_all([str(Path(args.runtime).resolve())])


if __name__ == "__main__":
    sys.exit(main())
