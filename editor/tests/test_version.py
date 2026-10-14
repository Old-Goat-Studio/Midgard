# SPDX-License-Identifier: Apache-2.0
from midgard_editor import BUNDLE_DIR, __main__, version


def test_version_matches_repository_version_file():
    assert (BUNDLE_DIR / "VERSION").read_text("utf-8").strip() == version.VERSION


def test_cli_prints_version(capsys):
    assert __main__.main(["--version"]) == 0
    assert version.VERSION in capsys.readouterr().out
