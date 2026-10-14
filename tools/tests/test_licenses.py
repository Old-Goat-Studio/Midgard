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
    assert "The Midgard Authors" in text


def test_editor_notices_include_qt():
    assert "PySide6 (LGPL-3.0-only)" in licenses.notices("editor")
