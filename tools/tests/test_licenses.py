# SPDX-License-Identifier: Apache-2.0
import importlib.util
from pathlib import Path

_spec = importlib.util.spec_from_file_location(
    "licenses", Path(__file__).resolve().parent.parent / "licenses.py")
licenses = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(licenses)


def test_repository_passes_the_licence_check():
    assert licenses.problems() == []


def test_game_notices_exclude_editor_and_build_components():
    text = licenses.notices("runtime")
    assert "sqlite3" in text and "nlohmann-json" in text
    assert "PySide6" not in text  # LGPL Qt is editor-only and never ships in games
    assert "pytest" not in text
    assert "Old Goat Studio" in text


def test_editor_notices_include_qt():
    assert "PySide6 (LGPL-3.0-only)" in licenses.notices("editor")


def _assets(tmp_path: Path, register: str, files: dict[str, bytes]) -> Path:
    folder = tmp_path / "third_party"
    folder.mkdir()
    (folder / "SOURCES.toml").write_text(register, "utf-8")
    for name, data in files.items():
        (folder / name).parent.mkdir(parents=True, exist_ok=True)
        (folder / name).write_bytes(data)
    return folder


CC0_ENTRY = """
[[asset]]
path = "{path}"
title = "Universal Base Characters"
author = "Quaternius"
source = "https://quaternius.com"
license = "{license}"
"""


def test_listed_cc0_assets_pass(tmp_path):
    folder = _assets(tmp_path, CC0_ENTRY.format(path="hero.glb", license="CC0-1.0")
                     + CC0_ENTRY.format(path="sounds/", license="CC0-1.0"),
                     {"hero.glb": b"glb", "sounds/step1.ogg": b"o", "sounds/step2.ogg": b"o"})
    assert licenses.asset_problems(folder) == []


def test_unlisted_and_non_cc0_assets_fail(tmp_path):
    folder = _assets(tmp_path, CC0_ENTRY.format(path="hero.glb", license="CC-BY-4.0")
                     + CC0_ENTRY.format(path="gone.glb", license="CC0-1.0")
                     + '[[asset]]\npath = "rock.png"\nlicense = "CC0-1.0"\n',
                     {"hero.glb": b"glb", "rock.png": b"png", "stray.wav": b"wav"})
    found = licenses.asset_problems(folder)
    assert "SOURCES.toml entry 'hero.glb': licence 'CC-BY-4.0' is not allowed (only CC0-1.0)" \
        in found
    assert "SOURCES.toml entry 'gone.glb': no such file" in found
    assert "SOURCES.toml entry 'rock.png': needs a source (an https:// address)" in found
    assert "SOURCES.toml entry 'rock.png': needs a title" in found
    assert "assets/third_party/stray.wav is not listed in SOURCES.toml" in found


def test_a_missing_or_broken_register_fails(tmp_path):
    assert licenses.asset_problems(tmp_path / "nowhere") == [
        "assets/third_party/SOURCES.toml is missing"]
    folder = _assets(tmp_path, "[[asset]\n", {})
    assert licenses.asset_problems(folder)[0].startswith("SOURCES.toml:")
