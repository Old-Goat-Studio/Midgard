# SPDX-License-Identifier: Apache-2.0
"""Physics in the editor: the Inspector's collider, rigid body and physics material rows,
World ▸ World Shape… and World ▸ Physics Layers…, and cooking a scene with physics."""
import sqlite3

import pytest
from PySide6.QtCore import Qt

from midgard_editor import contract, engine
from midgard_editor.cook import CookError, cook
from midgard_editor.docks.entity_form import EntityForm
from midgard_editor.physics_dialogs import PhysicsLayersDialog, WorldShapeDialog
from midgard_editor.scene_model import Entity


def form_for(qtbot, project, entity_id: str) -> EntityForm:
    form = EntityForm(project, entity_id)
    qtbot.addWidget(form)
    return form


def test_new_projects_have_physics_layers_and_ground_to_stand_on(project):
    assert project.physics_layers()[:3] == ["default", "player", "debris"]
    assert project.physics_ignored() == {(1, 2)}
    assert project.entity("e000003").components["collider"] == {"shape": "box"}


def test_older_projects_gain_the_layers_when_they_open(tmp_path):
    db = tmp_path / "old.db"
    con = sqlite3.connect(db)
    con.execute("PRAGMA user_version = 4")
    con.close()
    # Migration 5 on its own (1-4 would fail on this empty file, which claims to be at 4).
    assert contract.migrate(db) == len(contract.migrations())
    con = sqlite3.connect(db)
    assert con.execute("SELECT name FROM physics_layer WHERE id = 0").fetchone() == ("default",)
    con.close()


def test_adding_physics_components_in_the_inspector(qtbot, project):
    sphere = Entity("e000050", "Ball", None, {"transform": {}, "mesh": {"asset": "prim_sphere"}})
    project.put_entity(sphere)
    form = form_for(qtbot, project, "e000050")

    # A rigid body brings a collider that fits the mesh.
    form.add("rigidbody")
    components = project.entity("e000050").components
    assert components["rigidbody"] == {"motion": "dynamic"}
    assert components["collider"] == {"shape": "sphere", "radius": 0.5}

    form = form_for(qtbot, project, "e000050")  # as the Inspector rebuilds it
    assert form.collider_shape.currentData() == "sphere"
    assert not form.form.isRowVisible(form.collider_size)  # spheres have a radius, not a size
    assert form.form.isRowVisible(form.collider_radius)
    form.collider_radius.setValue(0.75)
    form.motion.setCurrentIndex(form.motion.findData("kinematic"))
    form.mass.setValue(12.5)
    form.layer.setCurrentIndex(form.layer.findData(2))
    components = project.entity("e000050").components
    assert components["collider"]["radius"] == 0.75
    assert components["rigidbody"] == {"motion": "kinematic", "mass": 12.5, "layer": 2}

    form.collider_shape.setCurrentIndex(form.collider_shape.findData("box"))
    assert form.form.isRowVisible(form.collider_size)
    form.collider_size.boxes[1].setValue(2.0)
    assert project.entity("e000050").components["collider"]["size"] == [1.0, 2.0, 1.0]

    form.add("physics_material")
    form = form_for(qtbot, project, "e000050")
    form.bounciness.setValue(0.8)
    assert project.entity("e000050").components["physics_material"] == {
        "friction": 0.5, "bounciness": 0.8}
    offered = [a.text() for a in form.add_component.menu().actions()]
    assert offered == ["Script", "Trigger"]  # the physics components are all there

    # Every change is one undo step.
    project.undo_stack.undo()
    assert project.entity("e000050").components["physics_material"]["bounciness"] == 0.0


def test_world_shape_dialog(qtbot, project):
    dialog = WorldShapeDialog(project)
    qtbot.addWidget(dialog)
    assert dialog.shape.currentData() == ""  # flat ground by default
    dialog.shape.setCurrentIndex(dialog.shape.findData("sphere_outside"))
    assert dialog.form.isRowVisible(dialog.sizes["radius"])
    assert not dialog.form.isRowVisible(dialog.sizes["width"])
    dialog.sizes["radius"].setValue(1500)
    dialog.gravity.setValue(5.0)
    dialog.accept()
    assert project.scene_settings()["body"] == {
        "id": "world", "shape": "sphere_outside", "gravity": 5.0, "radius": 1500.0}
    assert project.scene_settings()["sky"]  # the other settings stay

    dialog = WorldShapeDialog(project)  # shows what's set
    qtbot.addWidget(dialog)
    assert dialog.shape.currentData() == "sphere_outside"
    assert dialog.sizes["radius"].value() == 1500
    dialog.shape.setCurrentIndex(dialog.shape.findData(""))
    dialog.accept()
    assert "body" not in project.scene_settings()
    project.undo_stack.undo()
    assert project.scene_settings()["body"]["radius"] == 1500.0


def test_physics_layers_dialog(qtbot, project):
    dialog = PhysicsLayersDialog(project)
    qtbot.addWidget(dialog)
    assert dialog.table.rowCount() == 3  # default, player, debris
    assert dialog.table.item(1, 2).checkState() == Qt.CheckState.Unchecked  # player vs debris
    dialog.names[3].setText("water")
    dialog.names[3].editingFinished.emit()
    assert dialog.table.rowCount() == 4
    dialog.table.item(1, 3).setCheckState(Qt.CheckState.Unchecked)  # the player swims through
    dialog.accept()
    assert project.physics_layers()[3] == "water"
    assert project.physics_ignored() == {(1, 2), (1, 3)}
    project.undo_stack.undo()
    assert project.physics_layers()[3] == ""
    assert project.physics_ignored() == {(1, 2)}

    with pytest.raises(ValueError):
        project.set_physics_layers(["default", "same", "same"], set())


@pytest.mark.skipif(engine.module() is None, reason="the _midgard engine module isn't built")
def test_cooking_checks_colliders(project, tmp_path):
    crate = Entity("e000060", "Crate", None, {
        "transform": {"position": [0.0, 3.0, 0.0]}, "mesh": {"asset": "prim_cube"},
        "collider": {"shape": "box"}, "rigidbody": {"motion": "dynamic"}})
    project.put_entity(crate)
    cook(project, tmp_path / "ok")  # a falling crate on the ground: fine

    crate.components["collider"] = {"shape": "mesh"}  # meshes can't be dynamic
    project.put_entity(crate)
    with pytest.raises(CookError, match="e000060"):
        cook(project, tmp_path / "bad")


def test_the_players_walking_settings(qtbot, project):
    form = form_for(qtbot, project, "e000004")  # the new project's player
    assert form.player_rows["speed"].value() == 4.0
    assert form.player_rows["jump"].value() == 4.5  # the engine's default when unset
    form.player_rows["jump"].setValue(6.0)
    form.player_rows["height"].setValue(1.8)
    assert project.entity("e000004").components["player"] == {
        "speed": 4.0, "jump": 6.0, "height": 1.8}

