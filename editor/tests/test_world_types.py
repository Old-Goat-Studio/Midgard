# SPDX-License-Identifier: Apache-2.0
"""World types (milestone 5.11): an old project's terrain becomes a Custom world type with the
same ground; recipes are edited in place until a version ships, then saved as a new version;
shipped versions are kept; the World Shape dialog chooses and makes them; the cook checks them."""
import json
import sqlite3

import pytest

from midgard_editor import contract, engine, world_types
from midgard_editor.cook import CookError, cook
from midgard_editor.physics_dialogs import WorldShapeDialog

needs_engine = pytest.mark.skipif(engine.module() is None, reason="engine module not built")
HABITAT = {"id": "world", "shape": "curved_cylinder", "radius": 1000.0, "length": 5000.0,
           "gravity": 9.81, "origin": [0.0, 1000.0, 0.0]}


def test_an_old_projects_terrain_becomes_a_custom_world_type(tmp_path):
    db = tmp_path / "old.db"
    con = sqlite3.connect(db)
    for number, path in contract.migrations():
        if number < 12:
            con.executescript(path.read_text("utf-8"))
    con.execute("PRAGMA user_version = 11")
    con.execute("INSERT INTO string VALUES ('scn_main.name', 'Main', '')")
    old_terrain = {"seed": 1, "height_scale": 60.0, "sea_level": 0.0, "wall": "barrier"}
    con.execute("INSERT INTO scene (id, name_key, settings) VALUES ('scn_main', "
                "'scn_main.name', ?)", (json.dumps({"body": HABITAT, "terrain": old_terrain}),))
    con.commit()
    con.close()
    assert contract.migrate(db) == len(contract.migrations())
    con = sqlite3.connect(db)
    settings = json.loads(con.execute("SELECT settings FROM scene").fetchone()[0])
    assert settings["terrain"] == {"world_type": "wt_scn_main", "version": 1, "seed": 1}
    assert con.execute("SELECT name, version FROM world_type").fetchall() == [("Custom", 1)]
    recipe = json.loads(con.execute("SELECT recipe FROM world_type_version").fetchone()[0])
    # The same ground: the recipe's settings with the scene's seed are what was there.
    assert {**recipe["terrain"], "seed": 1} == old_terrain


def test_recipes_change_in_place_until_shipped_then_get_a_new_version(project):
    stack = project.undo_stack
    stack.push(world_types.SaveRecipeCommand(project, "wt_hills", {"terrain": {}}, "Hills"))
    assert world_types.load_all(project) == [{"id": "wt_hills", "name": "Hills", "version": 1}]
    save = world_types.SaveRecipeCommand(project, "wt_hills", {"terrain": {"height_scale": 90}})
    stack.push(save)
    assert save.version == 1  # not shipped: changed in place
    assert world_types.recipe(project, "wt_hills")["terrain"]["height_scale"] == 90
    # A scene made from it, then an export: version 1 is shipped.
    settings = project.scene_settings()
    settings["body"] = HABITAT
    settings["terrain"] = {"world_type": "wt_hills", "version": 1, "seed": 5}
    project.set_scene_settings(settings)
    world_types.mark_shipped(project)
    assert world_types.is_shipped(project, "wt_hills", 1)
    save = world_types.SaveRecipeCommand(project, "wt_hills", {"terrain": {"height_scale": 30}})
    stack.push(save)
    assert save.version == 2  # a new version; the old one generates as before
    assert world_types.recipe(project, "wt_hills", 1)["terrain"]["height_scale"] == 90
    assert world_types.recipe(project, "wt_hills")["terrain"]["height_scale"] == 30
    assert world_types.scene_world(project)["version"] == 1  # the scene keeps its version
    stack.undo()
    assert world_types.recipe(project, "wt_hills", 2) is None
    assert world_types.load_all(project)[0]["version"] == 1
    # Shipped versions are kept, by the database itself.
    with pytest.raises(sqlite3.IntegrityError, match="kept"), project.connect() as con:
        con.execute("DELETE FROM world_type WHERE id = 'wt_hills'")


def test_resolved_settings_give_the_viewport_the_ground_itself(project):
    project.undo_stack.push(world_types.SaveRecipeCommand(
        project, "wt_hills", {"terrain": {"height_scale": 90}}, "Hills"))
    settings = {"body": HABITAT, "terrain": {"world_type": "wt_hills", "version": 1, "seed": 5}}
    assert world_types.resolved_settings(project, settings)["terrain"] == {
        "height_scale": 90, "seed": 5}
    graph = {"output": "h", "nodes": {"h": {"op": "constant", "value": 3}}}
    project.undo_stack.push(world_types.SaveRecipeCommand(
        project, "wt_flat", {"graph": graph, "terrain": {"sea_level": 1}}, "Flat"))
    flat = {"body": HABITAT, "terrain": {"world_type": "wt_flat", "version": 1}}
    assert world_types.resolved_settings(project, flat)["terrain"] == {"sea_level": 1,
                                                                       "graph": graph}
    missing = {"body": HABITAT, "terrain": {"world_type": "wt_nope", "version": 1}}
    assert "terrain" not in world_types.resolved_settings(project, missing)


def test_the_world_shape_dialog_chooses_and_makes_world_types(qtbot, project):
    project.undo_stack.push(world_types.SaveRecipeCommand(
        project, "wt_hills", {"terrain": {"height_scale": 90, "octaves": 4}}, "Hills"))
    dialog = WorldShapeDialog(project)
    qtbot.addWidget(dialog)
    dialog.shape.setCurrentIndex(dialog.shape.findData("curved_cylinder"))
    dialog.terrain.setChecked(True)
    dialog.world_type.setCurrentIndex(dialog.world_type.findData("wt_hills"))
    assert dialog.terrain_fields["height_scale"].value() == 90  # the world type's settings
    assert dialog.octaves.value() == 4
    dialog.seed.setValue(77)
    dialog.accept()
    assert world_types.scene_world(project) == {"world_type": "wt_hills", "version": 1,
                                                "seed": 77}
    # A new world type from the fields, named Custom.
    again = WorldShapeDialog(project)
    qtbot.addWidget(again)
    assert again.world_type.currentData() == "wt_hills"
    again.world_type.setCurrentIndex(again.world_type.findData(""))
    again.terrain_fields["height_scale"].setValue(15)
    again.accept()
    world = world_types.scene_world(project)
    assert world["world_type"] == "wt_custom"
    assert world_types.recipe(project, "wt_custom")["terrain"]["height_scale"] == 15
    assert [t["name"] for t in world_types.load_all(project)] == ["Custom", "Hills"]


@needs_engine
def test_the_cook_takes_a_scenes_world_type_and_refuses_one_for_other_shapes(project, tmp_path):
    project.undo_stack.push(world_types.SaveRecipeCommand(
        project, "wt_planets", {"shapes": ["sphere_outside"], "terrain": {}}, "Planets"))
    settings = project.scene_settings()
    settings["body"] = {"id": "world", "shape": "sphere_outside", "radius": 3000.0,
                        "gravity": 9.81, "origin": [0.0, -3000.0, 0.0]}
    settings["terrain"] = {"world_type": "wt_planets", "version": 1, "seed": 3}
    project.set_scene_settings(settings)
    cook(project, tmp_path / "ok")
    settings["body"] = HABITAT  # not a planet
    project.set_scene_settings(settings)
    with pytest.raises(CookError, match="isn't for a curved_cylinder"):
        cook(project, tmp_path / "bad")
