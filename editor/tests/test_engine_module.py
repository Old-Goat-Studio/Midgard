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
    assert report["entities"] == 6 and report["assets"] == 1


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


TWO_VIEWS = r"""
import json, os, sys
sys.path[:0] = sys.argv[1:]
os.environ.pop("QT_QPA_PLATFORM", None)  # a real window, as the editor's viewport has
from PySide6.QtGui import QGuiApplication, QWindow
from midgard_editor import engine
app = QGuiApplication([])
platform = QGuiApplication.platformName()
if platform not in ("windows", "xcb"):
    print("SKIP no window system here:", platform)
    sys.exit(0)
window = QWindow()
window.setSurfaceType(QWindow.SurfaceType.VulkanSurface)
window.resize(64, 48)
window.show()
app.processEvents()
m = engine.module()
viewport, preview = m.Viewport(), m.Viewport()
try:
    viewport.attach_window(platform, int(window.winId()), 64, 48)
    preview.attach_offscreen(64, 48)  # made second, without a window surface
except RuntimeError as e:
    print("SKIP", e)
    sys.exit(0)
for v in (viewport, preview):
    v.put_entity(json.dumps({"id": "box", "name": "Box",
                             "components": {"mesh": {"asset": "builtin:cube"}}}))
    v.set_camera([2, 2, 2], [0, 0, 0])
viewport.resize(80, 60)  # the window's surface asked about again
viewport.render()
preview.render()
viewport.detach()  # the window's surface let go of
preview.render()
print("OK")
"""


@pytest.mark.skipif(engine.module() is None, reason="engine module not built here")
def test_two_engine_views_in_one_process_keep_their_own_vulkan(tmp_path):
    """The editor draws with several renderers at once (the viewport in a window, and the
    object and material previews offscreen); each must call Vulkan with its own functions."""
    import subprocess
    import sys
    from pathlib import Path

    import midgard_editor
    editor_dir = str(Path(midgard_editor.__file__).parent.parent)
    run = subprocess.run([sys.executable, "-c", TWO_VIEWS, editor_dir], capture_output=True,
                         text=True, timeout=120)
    assert run.returncode == 0, run.stderr[-2000:]
    if run.stdout.startswith("SKIP"):
        pytest.skip(run.stdout)
    assert run.stdout.strip().endswith("OK")
