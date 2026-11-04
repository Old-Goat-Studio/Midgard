# SPDX-License-Identifier: Apache-2.0
"""The texture generator from the editor's side (milestone 5.17 step 6).

The engine makes the picture from a recipe (midgard/texture_gen.h). A generated texture is
saved in the project as textures/generated/<name>.png, with its recipe beside it
(<name>.texgen.json) so the generator can open it again; with "bumps too", its normal map as
<name>_normal.png. From there it is a picture like any other: materials use it, and it can be
added to the project's Asset Library or sent to the Master.
"""
import json
import shutil
import tempfile
from pathlib import Path

from . import asset_library as lib
from . import engine

FOLDER = "textures/generated"
RECIPE_SUFFIX = ".texgen.json"

DEFAULT: dict = {"pattern": "noise", "size": 512, "scale": 4, "octaves": 4, "roughness": 0.5,
                 "seed": 1, "warp": 0.0, "angle": 0.0, "width": 0.5, "softness": 0.05,
                 "jitter": 1.0, "variation": 0.3, "invert": False,
                 "colors": [[0.0, [0.0, 0.0, 0.0]], [1.0, [1.0, 1.0, 1.0]]],
                 "output": "color", "strength": 2.0}

# The settings each pattern uses (besides scale, seed, warp, colours and output).
PATTERN_SETTINGS = {
    "noise": ("octaves", "roughness"),
    "ridged": ("octaves", "roughness"),
    "cells": ("jitter",),
    "gradient": ("angle",),
    "radial": (),
    "rings": (),
    "checker": ("softness",),
    "stripes": ("angle", "width", "softness"),
    "bricks": ("width", "softness", "variation"),
    "waves": ("angle",),
}
PATTERN_LABELS = {"noise": "Noise", "ridged": "Ridged noise", "cells": "Cells",
                  "gradient": "Gradient", "radial": "Radial", "rings": "Rings",
                  "checker": "Checker", "stripes": "Stripes", "bricks": "Bricks",
                  "waves": "Waves"}


def _preset(**changes) -> dict:
    return {**DEFAULT, **changes}


PRESETS: dict[str, dict] = {
    "Clouds": _preset(pattern="noise", scale=3, octaves=6, roughness=0.55,
                      colors=[[0.3, [0.35, 0.55, 0.85]], [0.75, [1.0, 1.0, 1.0]]]),
    "Marble": _preset(pattern="waves", scale=3, warp=2.5, angle=35,
                      colors=[[0.0, [0.45, 0.45, 0.5]], [0.35, [0.85, 0.85, 0.82]],
                              [1.0, [0.97, 0.96, 0.94]]]),
    "Wood": _preset(pattern="waves", scale=10, angle=90, warp=0.9,
                    colors=[[0.0, [0.42, 0.25, 0.12]], [1.0, [0.74, 0.52, 0.31]]]),
    "Rock": _preset(pattern="ridged", scale=4, octaves=6, roughness=0.6,
                    colors=[[0.0, [0.25, 0.24, 0.23]], [1.0, [0.62, 0.6, 0.56]]]),
    "Flagstones": _preset(pattern="cells", scale=5, jitter=0.9,
                          colors=[[0.0, [0.2, 0.2, 0.2]], [0.2, [0.45, 0.43, 0.4]],
                                  [1.0, [0.68, 0.65, 0.6]]]),
    "Bricks": _preset(pattern="bricks", scale=4, width=0.5, softness=0.04, variation=0.35,
                      colors=[[0.0, [0.75, 0.73, 0.68]], [0.05, [0.4, 0.17, 0.12]],
                              [1.0, [0.72, 0.33, 0.22]]]),
    "Tiles": _preset(pattern="checker", scale=8, softness=0.04,
                     colors=[[0.0, [0.12, 0.12, 0.14]], [1.0, [0.9, 0.9, 0.88]]]),
    "Warning stripes": _preset(pattern="stripes", scale=6, angle=45, width=0.5,
                               softness=0.02,
                               colors=[[0.0, [0.08, 0.08, 0.08]], [1.0, [0.95, 0.75, 0.1]]]),
    "Sand ripples (bumps)": _preset(pattern="waves", scale=10, warp=1.2, output="normal",
                                    strength=3.0),
    "Hammered metal (bumps)": _preset(pattern="cells", scale=12, jitter=1.0,
                                      output="normal", strength=4.0),
}


class GeneratorError(Exception):
    pass


def generate(recipe: dict) -> tuple[int, bytes, list[str]]:
    """(size, RGBA pixels top row first, problems) for a recipe; GeneratorError without the
    engine."""
    midgard = engine.module()
    if midgard is None or not hasattr(midgard, "generate_texture"):
        raise GeneratorError("the texture generator needs the engine module")
    out = midgard.generate_texture(json.dumps(recipe))
    return int(out["size"]), bytes(out["pixels"]), list(out["problems"])


def to_image(size: int, pixels: bytes):
    """A QImage (a copy) of the generator's pixels."""
    from PySide6.QtGui import QImage
    return QImage(pixels, size, size, size * 4, QImage.Format.Format_RGBA8888).copy()


def folder(project_root: Path) -> Path:
    return Path(project_root) / FOLDER


def safe_name(name: str) -> str:
    cleaned = "".join(c if c.isalnum() or c in "-_ " else "_" for c in name).strip(" .")
    return cleaned or "texture"


def save(project_root: Path, name: str, recipe: dict, bumps_too: bool = False) -> list[Path]:
    """The texture (and with bumps_too, for a colour recipe, its normal map) saved in the
    project, its recipe beside it; the pictures, colour first. Saving again under the same name
    replaces them."""
    target = folder(project_root)
    target.mkdir(parents=True, exist_ok=True)
    base = safe_name(name)
    made = []
    outputs = [(base, recipe)]
    if bumps_too and recipe.get("output", "color") == "color":
        outputs.append((base + "_normal", {**recipe, "output": "normal"}))
    for stem, r in outputs:
        size, pixels, problems = generate(r)
        if problems:
            raise GeneratorError("; ".join(problems))
        path = target / f"{stem}.png"
        if not to_image(size, pixels).save(str(path), "PNG"):
            raise GeneratorError(f"{path.name} couldn't be saved")
        made.append(path)
    (target / f"{base}{RECIPE_SUFFIX}").write_text(
        json.dumps({**recipe, "bumps_too": bool(bumps_too)}, indent=2), encoding="utf-8")
    return made


def saved(project_root: Path) -> list[str]:
    """The names of the generated textures the project has (those with a recipe)."""
    f = folder(project_root)
    if not f.is_dir():
        return []
    return sorted(p.name[:-len(RECIPE_SUFFIX)] for p in f.glob("*" + RECIPE_SUFFIX))


def load(project_root: Path, name: str) -> tuple[dict, bool]:
    """A generated texture's recipe and whether it has its bumps too."""
    path = folder(project_root) / f"{safe_name(name)}{RECIPE_SUFFIX}"
    try:
        recipe = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise GeneratorError(f"the recipe for '{name}' can't be read: {e}") from e
    bumps = bool(recipe.pop("bumps_too", False))
    return {**DEFAULT, **recipe}, bumps


def add_to_library(project, pictures: list[Path], name: str,
                   licence: str = lib.OWN_WORK, author: str = "") -> lib.Asset:
    """Generated pictures as one texture asset in the project's Asset Library (colour and
    normal by their roles)."""
    with tempfile.TemporaryDirectory() as scratch:
        files, roles = [], {}
        for p in pictures:
            copy = Path(scratch) / p.name
            shutil.copyfile(p, copy)
            files.append(copy)
            normal = p.stem.endswith("_normal") or (len(pictures) == 1 and _is_normal(p))
            roles[p.name] = "normal" if normal else "color"
        return lib.import_asset(project, files, "textures", name, category="miscellaneous",
                                description="Made with Midgard's texture generator.",
                                licence=licence, author=author, roles=roles)


def _is_normal(picture: Path) -> bool:
    recipe = picture.with_name(picture.stem + RECIPE_SUFFIX)
    try:
        return json.loads(recipe.read_text(encoding="utf-8")).get("output") == "normal"
    except (OSError, ValueError):
        return False
