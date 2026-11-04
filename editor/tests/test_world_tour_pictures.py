# SPDX-License-Identifier: Apache-2.0
"""The World Tour's pictures and speed (milestone 5.18 step 4): each world as the game first
draws it, compared with its reference picture (editor/tests/images/world_tour/); and each
world drawn in time for 60 frames a second at the phone's quality, with its ground made in
seconds.

    MIDGARD_UPDATE_IMAGES=1 pytest editor/tests/test_world_tour_pictures.py   new references
"""
import os
import re
import subprocess
import sys
import time
from pathlib import Path

import pytest

from midgard_editor import about, engine, scenes, world_tour
from midgard_editor.cook import cook

ROOT = Path(__file__).resolve().parents[2]
REFERENCES = Path(__file__).resolve().parent / "images" / "world_tour"
FRAME_BUDGET_MS = 1000.0 / 60.0
GROUND_BUDGET_S = 15.0  # the first frame of a world, its ground made (with the game's start)

pytestmark = pytest.mark.skipif(
    engine.module() is None or not hasattr(engine.module(), "sample_world_types") or
    not about.runtime_candidates(), reason="needs the engine module and the runtime")


@pytest.fixture(scope="module")
def tour(tmp_path_factory, qapp):
    return world_tour.make_world_tour(tmp_path_factory.mktemp("pictures"))


def scene_ids(project) -> list[tuple[str, str]]:
    return [(s["id"], s["name"]) for s in scenes.load_all(project)]


def draw(package: Path, picture: Path, *extra: str) -> tuple[subprocess.CompletedProcess, float]:
    start = time.monotonic()
    run = subprocess.run([str(about.runtime_candidates()[0]), "--package", str(package),
                          "--screenshot", str(picture), *extra],
                         capture_output=True, text=True, timeout=300)
    seconds = time.monotonic() - start
    if run.returncode != 0 and "Vulkan" in (run.stderr + run.stdout):
        pytest.skip("no Vulkan driver here: " + run.stderr.strip())
    assert run.returncode == 0, run.stderr[-2000:]
    return run, seconds


@pytest.mark.parametrize("name", ["Main", "Archipelago", "Habitat", "Planet", "Hollow World"])
def test_each_world_looks_as_it_did(tour, tmp_path, name):
    sys.path.insert(0, str(ROOT / "tools"))
    try:
        from images import compare
    finally:
        sys.path.remove(str(ROOT / "tools"))
    scene = next(i for i, n in scene_ids(tour) if n == name)
    package = cook(tour, tmp_path / "pkg", entry_scene=scene).out_dir
    picture = tmp_path / f"{scene}.png"
    draw(package, picture, "--size", "480x270")
    reference = REFERENCES / f"{scene}.png"
    if os.environ.get("MIDGARD_UPDATE_IMAGES") or not reference.is_file():
        reference.parent.mkdir(parents=True, exist_ok=True)
        reference.write_bytes(picture.read_bytes())
        if not os.environ.get("MIDGARD_UPDATE_IMAGES"):
            pytest.fail(f"no reference picture for {name}: made {reference} (look at it)")
    # (Terrain far away and shadow edges are filtered a little differently by each GPU.)
    result = compare(picture, reference, tolerance=8, max_bad=0.02,
                     diff=tmp_path / f"{scene}_diff.png")
    assert result.ok, f"{name}: {result.message} (see {tmp_path / f'{scene}_diff.png'})"


@pytest.mark.parametrize("name", ["Main", "Archipelago", "Habitat", "Planet", "Hollow World"])
def test_each_world_runs_at_60_frames_a_second_on_the_phones_quality(tour, tmp_path, name):
    scene = next(i for i, n in scene_ids(tour) if n == name)
    package = cook(tour, tmp_path / "pkg", entry_scene=scene).out_dir
    run, seconds = draw(package, tmp_path / "shot.png", "--size", "1280x720",
                        "--quality", "phone", "--frames", "60")
    found = re.search(r"Frame time: ([0-9.]+) ms median over 60 frames( \(software)?",
                      run.stdout)
    assert found, run.stdout
    assert seconds < GROUND_BUDGET_S, f"{name}: {seconds:.1f} s to its first frame"
    if found.group(2):
        pytest.skip(f"a software renderer ({found.group(1)} ms a frame): not held to it")
    assert float(found.group(1)) < FRAME_BUDGET_MS, f"{name}: {found.group(1)} ms a frame"
