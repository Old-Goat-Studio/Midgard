# SPDX-License-Identifier: Apache-2.0
"""The _midgard engine module, used in-process by the editor. Skipped where it isn't built."""
import json
import sqlite3

import pytest

from midgard_editor import about, engine
from midgard_editor.cook import CookError, cook
from midgard_editor.version import VERSION

needs_module = pytest.mark.skipif(engine.module() is None,
                                  reason="the _midgard engine module is not built")


@needs_module
def test_the_module_reports_the_engines_version():
    v = engine.version()
    assert v["engine"] == VERSION
    assert v["schema_min"] <= v["schema_max"]
    about.engine_info.cache_clear()
    assert about.engine_info().version == VERSION


@needs_module
def test_cooked_packages_pass_the_engines_check(project, tmp_path):
    result = cook(project, tmp_path / "pkg")
    report = engine.validate(result.out_dir)
    assert report["ok"], report
    assert report["entities"] == 4 and report["assets"] == 1


@needs_module
def test_the_engines_check_stops_a_broken_cook(project, tmp_path):
    with sqlite3.connect(project.db_path) as con:  # a transform the engine can't read
        con.execute("UPDATE entity SET components = ? WHERE id = 'e000004'",
                    (json.dumps({"transform": {"position": [1, 2]}}),))
    con.close()
    with pytest.raises(CookError, match="e000004"):
        cook(project, tmp_path / "pkg")


def test_cooking_works_without_the_module(project, tmp_path, monkeypatch):
    monkeypatch.setattr(engine, "module", lambda: None)
    assert engine.validate(tmp_path) is None
    assert cook(project, tmp_path / "pkg").manifest["content"]
