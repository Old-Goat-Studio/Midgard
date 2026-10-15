# SPDX-License-Identifier: Apache-2.0
"""The Viewport panel. Tests run on Qt's offscreen platform, where there is no native window to
draw into, so the engine drawing itself is tested in engine/tests/test_bindings.py."""
import math

import pytest

from midgard_editor import engine
from midgard_editor.docks.viewport import OrbitCamera, ViewportPanel


def close(a, b, tol=1e-6):
    return all(abs(x - y) < tol for x, y in zip(a, b, strict=True))


def test_the_orbit_camera_starts_where_it_is_told():
    camera = OrbitCamera.looking([4.0, 3.0, 6.0], [0.0, 0.5, 0.0])
    assert close(camera.eye(), [4.0, 3.0, 6.0], 1e-4)
    assert camera.distance == pytest.approx(math.sqrt(16 + 6.25 + 36))


def test_orbiting_zooming_and_panning():
    camera = OrbitCamera(target=[0.0, 0.0, 0.0], yaw=0.0, pitch=0.0, distance=10.0)
    assert close(camera.eye(), [0.0, 0.0, 10.0])
    camera.orbit(-225.0, 0.0)  # 90 degrees around
    assert close(camera.eye(), [10.0, 0.0, 0.0], 1e-4)
    camera.orbit(0.0, 1000.0)  # pitch stops short of straight up
    assert camera.pitch == 89.0
    camera.zoom(1)
    assert camera.distance == pytest.approx(8.5)
    camera.zoom(-1000)
    assert camera.distance == 5000.0

    flat = OrbitCamera(target=[0.0, 0.0, 0.0], yaw=0.0, pitch=0.0, distance=10.0)
    flat.pan(-100, 0, 500)  # dragging left moves the view (and its target) to the right
    assert flat.target[0] > 0 and flat.target[1] == pytest.approx(0.0)
    flat.pan(0, 100, 500)
    assert flat.target[1] > 0


def test_the_panel_explains_when_it_cannot_draw(qtbot, project):
    panel = ViewportPanel()
    qtbot.addWidget(panel)
    panel.set_project(project)
    assert panel.view is None  # the offscreen test platform has no native windows
    text = panel.message.text()
    if engine.module() is None:
        assert "engine module" in text
    else:
        assert "can't draw on this display" in text
    panel.set_project(None)


def test_old_saved_layouts_get_the_viewport(qtbot, settings):
    from midgard_editor.main_window import MainWindow

    first = MainWindow(settings)
    qtbot.addWidget(first)
    assert "viewport" in first._layout_record()["panels"]
    # A layout saved before the Viewport panel existed: it isn't in the saved state.
    first.panels["viewport"].toggleView(False)
    old = first._layout_record()
    del old["panels"]
    settings.set("layout_last", old)
    second = MainWindow(settings)
    qtbot.addWidget(second)
    assert not second.panels["viewport"].isClosed()
