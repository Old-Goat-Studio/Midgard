# SPDX-License-Identifier: Apache-2.0
"""Terrain in the editor (milestone 5.9): the World Shape dialog's Terrain section, the cook's
checks through _midgard, and the viewport drawing streamed terrain."""
import json

import pytest

from midgard_editor import engine
from midgard_editor.physics_dialogs import WorldShapeDialog

needs_engine = pytest.mark.skipif(engine.module() is None, reason="engine module not built")


def choose(dialog, shape: str) -> None:
    dialog.shape.setCurrentIndex(dialog.shape.findData(shape))


def test_terrain_is_offered_on_bounded_areas_and_cylinders(qtbot, project):
    dialog = WorldShapeDialog(project)
    qtbot.addWidget(dialog)
    dialog.show()
    for shape, offered in (("", False), ("bounded_area", True), ("flat_cylinder", True),
                           ("curved_cylinder", True), ("sphere_outside", False)):
        choose(dialog, shape)
        assert dialog.terrain.isVisible() == offered, shape
    choose(dialog, "bounded_area")
    form = dialog.terrain_form
    assert form.isRowVisible(dialog.terrain_fields["ocean_band"])
    assert not form.isRowVisible(dialog.wall)  # walls are a cylinder's ends
    choose(dialog, "curved_cylinder")
    assert form.isRowVisible(dialog.wall) and form.isRowVisible(dialog.terrain_fields["end_ramp"])
    assert not form.isRowVisible(dialog.terrain_fields["ocean_band"])
    # With terrain, the flat World Surface isn't offered as well.
    dialog.terrain.setChecked(True)
    assert not dialog.form.isRowVisible(dialog.add_ground)


def test_terrain_settings_are_saved_undone_and_shown_again(qtbot, project):
    dialog = WorldShapeDialog(project)
    qtbot.addWidget(dialog)
    choose(dialog, "bounded_area")
    dialog.sizes["width"].setValue(3000)
    dialog.sizes["length"].setValue(3000)
    dialog.terrain.setChecked(True)
    dialog.seed.setValue(42)
    dialog.terrain_fields["height_scale"].setValue(90)
    dialog.octaves.setValue(5)
    dialog.accept()
    terrain = project.scene_settings()["terrain"]
    assert terrain["seed"] == 42 and terrain["height_scale"] == 90 and terrain["octaves"] == 5
    assert "wall" not in terrain  # a bounded area has no ends
    assert not any(e.components.get("mesh", {}).get("asset") == "builtin:surface"
                   for e in project.entities())  # terrain is the ground

    again = WorldShapeDialog(project)  # shows what's set
    qtbot.addWidget(again)
    assert again.terrain.isChecked() and again.seed.value() == 42
    assert again.terrain_fields["height_scale"].value() == 90
    again.terrain.setChecked(False)
    again.accept()
    assert "terrain" not in project.scene_settings()
    project.undo_stack.undo()
    assert project.scene_settings()["terrain"]["seed"] == 42


@needs_engine
def test_the_dialog_refuses_terrain_the_cook_would(qtbot, project, monkeypatch):
    from PySide6.QtWidgets import QMessageBox

    warned = []
    monkeypatch.setattr(QMessageBox, "warning", lambda *a: warned.append(a[2]))
    assert engine.module().terrain_problems(json.dumps({"octaves": 40}))
    dialog = WorldShapeDialog(project)
    qtbot.addWidget(dialog)
    choose(dialog, "curved_cylinder")
    dialog.terrain.setChecked(True)
    dialog.terrain_fields["feature_size"].setMinimum(0)
    dialog.terrain_fields["feature_size"].setValue(0)  # (the box would stop at 1)
    dialog.accept()
    assert warned and "feature size" in warned[-1]
    assert "terrain" not in project.scene_settings()


@needs_engine
def test_the_viewport_draws_streamed_terrain(project):
    view = engine.module().Viewport()
    try:
        view.attach_offscreen(96, 64)
    except RuntimeError:
        pytest.skip("no offscreen rendering here")
    body = {"id": "world", "shape": "bounded_area", "width": 3000, "length": 3000,
            "gravity": 9.81, "origin": [0, 0, 0]}
    view.set_settings(json.dumps({"sky": [0.0, 0.0, 0.0], "body": body}))
    view.set_camera([0.0, 150.0, 0.0], [300.0, 0.0, -600.0], 60.0)

    def ground() -> float:  # how much of the picture isn't the black sky
        view.render()
        pixels = view.read_pixels()
        return sum(1 for i in range(0, len(pixels), 4) if max(pixels[i:i + 3]) > 12) / (
            len(pixels) / 4)

    assert ground() < 0.05  # no terrain: nothing below the horizon
    view.set_settings(json.dumps({"sky": [0.0, 0.0, 0.0], "body": body,
                                  "terrain": {"seed": 3}}))
    assert ground() > 0.4  # hills
    view.set_settings(json.dumps({"sky": [0.0, 0.0, 0.0], "body": body}))
    assert ground() < 0.05  # taken away again
    view.detach()
