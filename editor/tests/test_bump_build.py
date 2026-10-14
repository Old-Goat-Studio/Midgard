# SPDX-License-Identifier: Apache-2.0
import datetime
import importlib.util

import pytest

from midgard_editor import BUNDLE_DIR

_spec = importlib.util.spec_from_file_location("bump_build", BUNDLE_DIR / "bump_build.py")
bump_build = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(bump_build)

DAY = datetime.date(2026, 10, 14)


def run(tmp_path, version):
    (tmp_path / "VERSION").write_text(version + "\n", "utf-8")
    return bump_build.bump(tmp_path / "VERSION", tmp_path / "version.py", DAY)


def test_first_build_is_one(tmp_path):
    assert run(tmp_path, "1.0.0") == ("1.0.0", 1, "2026-10-14")
    text = (tmp_path / "version.py").read_text("utf-8")
    assert 'VERSION = "1.0.0"' in text and "BUILD = 1" in text
    assert 'BUILD_DATE = "2026-10-14"' in text
    assert text.startswith("# SPDX-License-Identifier: Apache-2.0")


@pytest.mark.parametrize("next_version, build", [
    ("1.0.0", 3),  # unchanged: +1
    ("1.0.1", 3),  # patch: +1
    ("1.1.0", 1),  # minor: reset
    ("2.0.0", 1),  # major: reset
])
def test_build_numbering(tmp_path, next_version, build):
    run(tmp_path, "1.0.0")
    run(tmp_path, "1.0.0")
    assert run(tmp_path, next_version)[1] == build


def test_bad_version_is_refused(tmp_path):
    with pytest.raises(SystemExit):
        run(tmp_path, "1.0")
