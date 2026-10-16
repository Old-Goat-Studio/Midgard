# SPDX-License-Identifier: Apache-2.0
import json
import random
import sys
from pathlib import Path

import pytest
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import images  # noqa: E402


def scene_image(path: Path, cube=(155, 120, 20), size=(64, 36)) -> Path:
    """A small stand-in for a rendered scene: sky, ground, and a cube in the middle."""
    img = Image.new("RGB", size, (20, 28, 46))
    w, h = size
    img.paste((48, 72, 36), (0, h * 2 // 3, w, h))
    img.paste(cube, (w * 2 // 5, h // 3, w * 3 // 5, h * 2 // 3))
    img.save(path)
    return path


def test_identical_images_match(tmp_path):
    a = scene_image(tmp_path / "a.png")
    result = images.compare(a, a)
    assert result.ok and result.bad_pixels == 0 and result.max_difference == 0


def test_small_driver_differences_are_tolerated(tmp_path):
    reference = scene_image(tmp_path / "reference.png")
    img = Image.open(reference)
    rng = random.Random(7)
    for x in range(img.width):
        for y in range(img.height):  # every pixel off by up to the tolerance
            img.putpixel((x, y), tuple(min(255, c + rng.randint(0, images.TOLERANCE))
                                       for c in img.getpixel((x, y))))
    img.putpixel((0, 0), (255, 255, 255))  # and one stray pixel (well under max_bad)
    img.save(tmp_path / "actual.png")
    # 1 of 2304 pixels is 0.04%, under the 0.2% allowed.
    assert images.compare(tmp_path / "actual.png", reference).ok


def test_a_broken_shader_fails(tmp_path):
    """What a broken shader looks like to the test: the cube comes out the wrong colour."""
    reference = scene_image(tmp_path / "reference.png")
    broken = scene_image(tmp_path / "broken.png", cube=(255, 0, 255))
    result = images.compare(broken, reference, diff=tmp_path / "diff.png")
    assert not result.ok
    assert result.bad_pixels == 13 * 12  # exactly the cube
    assert "differ by more than 6" in result.message
    diff = Image.open(tmp_path / "diff.png")
    assert diff.getpixel((32, 18)) == (255, 0, 0)  # marked in red
    assert diff.getpixel((2, 2))[0] == diff.getpixel((2, 2))[1]  # the rest in grey


def test_a_different_size_fails(tmp_path):
    result = images.compare(scene_image(tmp_path / "a.png", size=(64, 36)),
                            scene_image(tmp_path / "b.png", size=(32, 18)))
    assert not result.ok and "size 64x36, expected 32x18" in result.message


FAKE_RUNTIME = """
import sys
from PIL import Image
args = sys.argv[1:]
out = args[args.index("--screenshot") + 1]
colour = (255, 0, 255) if "--broken" in args else (155, 120, 20)
img = Image.new("RGB", (64, 36), (20, 28, 46))
img.paste(colour, (20, 10, 40, 26))
img.save(out)
"""


@pytest.fixture
def scene_folder(tmp_path, monkeypatch):
    folder = tmp_path / "images"
    folder.mkdir()
    (folder / "scenes.json").write_text(json.dumps({"scenes": [
        {"name": "cube", "args": []},
        {"name": "missing", "args": []}]}), "utf-8")
    monkeypatch.setattr(images, "IMAGES", folder)
    monkeypatch.setattr(images, "ROOT", tmp_path)
    fake = tmp_path / "fake_runtime.py"
    fake.write_text(FAKE_RUNTIME, "utf-8")
    return folder, [sys.executable, str(fake)]


def test_run_renders_compares_and_reports(scene_folder, tmp_path, capsys, monkeypatch):
    folder, runtime = scene_folder
    assert images.update_all(runtime) == 0  # writes both references
    assert (folder / "cube.png").is_file()
    (folder / "missing.png").unlink()

    monkeypatch.setenv("GITHUB_ACTIONS", "true")
    assert images.run_all(runtime, tmp_path / "out") == 1
    out = capsys.readouterr().out
    assert "ok   cube" in out
    assert "FAIL missing: no reference image" in out
    assert "::error title=Image test missing::" in out
    assert "1 of 2 image tests passed" in out

    images.update_all(runtime)
    assert images.run_all(runtime, tmp_path / "out") == 0
    assert images.run_all([*runtime, "--broken"], tmp_path / "out") == 1
    assert (tmp_path / "out" / "cube.diff.png").is_file()


def test_a_runtime_that_fails_is_reported(scene_folder, tmp_path, capsys):
    _folder, _runtime = scene_folder
    assert images.run_all([sys.executable, "-c", "raise SystemExit('no Vulkan driver')"],
                          tmp_path / "out") == 1
    assert "the runtime didn't draw it: no Vulkan driver" in capsys.readouterr().out


def test_the_real_scene_list_is_valid():
    names = [s["name"] for s in images.scenes()]
    assert names and len(names) == len(set(names))
    for scene in images.scenes():
        assert isinstance(scene["args"], list) and "--screenshot" not in scene["args"]
        if "same_as" in scene:  # an earlier scene, and no image of its own
            assert scene["same_as"] in names[:names.index(scene["name"])], scene["name"]
            assert not (images.IMAGES / f"{scene['name']}.png").exists(), scene["name"]
        else:
            assert (images.IMAGES / f"{scene['name']}.png").is_file(), scene["name"]


FAKE_FAR = """
import sys
from PIL import Image
args = sys.argv[1:]
out = args[args.index("--screenshot") + 1]
img = Image.new("RGB", (64, 36), (20, 28, 46))
img.paste((155, 120, 20), (20, 10, 40, 26))
if "--far" in args and "--jitter" in args:
    img.putpixel((20, 10), (20, 28, 46))  # one pixel off: what float positions do far out
img.save(out)
"""


def test_same_as_compares_exactly_with_this_runs_image(scene_folder, tmp_path, capsys):
    folder, _runtime = scene_folder
    fake = tmp_path / "fake_far.py"
    fake.write_text(FAKE_FAR, "utf-8")
    runtime = [sys.executable, str(fake)]
    (folder / "scenes.json").write_text(json.dumps({"scenes": [
        {"name": "cube", "args": []},
        {"name": "cube_far", "args": ["--far"], "same_as": "cube"}]}), "utf-8")
    assert images.update_all(runtime) == 0
    assert not (folder / "cube_far.png").exists()  # it has no image of its own
    assert images.run_all(runtime, tmp_path / "out") == 0
    # One pixel is within the stored references' allowance, but not same_as's: exact.
    assert images.run_all([*runtime, "--jitter"], tmp_path / "out") == 1
    assert "FAIL cube_far: 1 of 2304 pixels" in capsys.readouterr().out
    (folder / "scenes.json").write_text(json.dumps({"scenes": [
        {"name": "cube_far", "args": [], "same_as": "cube"}]}), "utf-8")
    images.run_all(runtime, tmp_path / "fresh")
    assert "scene cube wasn't drawn before it" in capsys.readouterr().out
