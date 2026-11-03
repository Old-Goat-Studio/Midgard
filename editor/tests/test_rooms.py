# SPDX-License-Identifier: Apache-2.0
"""Rooms and system space in the editor and scripts (milestone 5.16 step 6): a Scene Exit into
space with its way back in, a planet's atmosphere ceiling, and midgard.rooms' space and an
entity's flying for scripts."""
from test_scripting import needs_scripting, simulate, write_script

from midgard_editor.cook import cook
from midgard_editor.scene_model import Entity


def test_a_scene_exit_can_lead_into_space_and_back_in_here(project, qtbot):
    from midgard_editor.docks.entity_form import SPACE, EntityForm
    from midgard_editor.docks.scene_panel import ScenePanel
    panel = ScenePanel()
    qtbot.addWidget(panel)
    panel.set_project(project)
    back = panel.add_arrival()
    dock = panel.add_scene_exit()
    form = EntityForm(project, dock)
    qtbot.addWidget(form)
    assert form.exit_scene.findData(SPACE) > 0
    assert form.exit_arrival_label.text() == "Arriving at"
    form.exit_scene.setCurrentIndex(form.exit_scene.findData(SPACE))
    assert project.entity(dock).components["scene_exit"]["scene"] == "@space"
    # The way back in: this scene's own arrival points.
    assert form.exit_arrival_label.text() == "Coming back in at"
    assert form.exit_arrival.findData(back) > 0
    form.exit_arrival.setCurrentIndex(form.exit_arrival.findData(back))
    assert project.entity(dock).components["scene_exit"]["arrival"] == back
    project.undo_stack.undo()
    project.undo_stack.undo()
    assert project.entity(dock).components["scene_exit"]["scene"] == ""


def test_a_planets_atmosphere_ceiling_is_set_in_the_world_shape_dialog(project, qtbot):
    from midgard_editor.physics_dialogs import WorldShapeDialog
    dialog = WorldShapeDialog(project)
    qtbot.addWidget(dialog)
    assert not dialog.form.isRowVisible(dialog.ceiling)  # (flat ground: no sky to leave by)
    dialog.shape.setCurrentIndex(dialog.shape.findData("sphere_outside"))
    assert dialog.form.isRowVisible(dialog.ceiling)
    assert dialog.ceiling.value() == 0.0  # automatic
    dialog.ceiling.setValue(650.0)
    dialog.add_ground.setChecked(False)
    dialog.accept()
    assert project.scene_settings()["ceiling"] == 650.0
    again = WorldShapeDialog(project)
    qtbot.addWidget(again)
    assert again.ceiling.value() == 650.0
    again.ceiling.setValue(0.0)  # back to automatic
    again.accept()
    assert "ceiling" not in project.scene_settings()
    # Not a planet: none kept.
    third = WorldShapeDialog(project)
    qtbot.addWidget(third)
    third.ceiling.setValue(800.0)
    third.shape.setCurrentIndex(third.shape.findData("sphere_inside"))
    third.add_ground.setChecked(False)
    third.accept()
    assert "ceiling" not in project.scene_settings()


SPACE_SCRIPT = """
import midgard

def on_start(entity):
    player = midgard.player()
    print("flying", player.flying, "ceiling", round(midgard.rooms.ceiling))
    player.flying = True
    print("flying", player.flying, "altitude", round(midgard.rooms.altitude) >= 0)
    try:
        entity.flying = True
    except ValueError as e:
        print("refused:", e)
    try:
        midgard.rooms.enter_space(0.1)
        print("off we go")
    except ValueError as e:
        print("no space:", e)
"""


@needs_scripting
def test_scripts_fly_the_player_and_take_it_into_space(project, tmp_path):
    project.put_entity(Entity("e000090", "Launcher", None, {
        "transform": {}, "script": {"module": "launcher"}}))
    write_script(project, "launcher", SPACE_SCRIPT)
    # On flat ground: there is no space.
    flat, run = simulate(cook(project, tmp_path / "flat").out_dir, 30)
    assert run.returncode == 0, run.stderr
    lines = run.stdout.splitlines()
    assert "flying False ceiling 0" in lines
    assert "flying True altitude True" in lines
    assert any(line.startswith("refused: entity 'e000090' isn't the player") for line in lines)
    assert any(line.startswith("no space: only a planet") for line in lines)
    assert flat["scene"] == "scn_main"
    # On a planet: up into space.
    settings = project.scene_settings()
    settings["body"] = {"id": "world", "shape": "sphere_outside", "radius": 2000.0,
                        "gravity": 9.81, "origin": [0.0, -2000.0, 0.0]}
    project.set_scene_settings(settings)
    planet, run = simulate(cook(project, tmp_path / "planet").out_dir, 120)
    assert run.returncode == 0, run.stderr
    lines = run.stdout.splitlines()
    assert "flying False ceiling 400" in lines  # (a fifth of 2 km)
    assert "off we go" in lines
    assert planet["scene"] == "@space"
