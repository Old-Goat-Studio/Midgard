# SPDX-License-Identifier: Apache-2.0
"""The World Type editor (milestone 5.11): a world graph's JSON and the node-graph widget's model
convert both ways; every edit (the graph, a node's setting, the climate, biomes, placements, the
recipe's fields) is one undoable step saved as the world type's; world types are made,
duplicated, renamed and deleted (not while a scene uses one); the preview is a map of the world,
the ground's cost and what the cook says."""
import json

import pytest
from PySide6.QtCore import QPointF

from midgard_editor import engine, world_types
from midgard_editor.docks.world_type_editor import (
    MAP_SIZE,
    SPECS,
    WorldTypePanel,
    classic_graph,
    from_model,
    preview_settings,
    to_model,
)

needs_engine = pytest.mark.skipif(engine.module() is None, reason="engine module not built")

GRAPH = {"output": "out", "nodes": {
    "n": {"op": "noise", "size": 300, "octaves": 4, "salt": "base"},
    "lat": {"op": "input", "name": "latitude"},
    "c": {"op": "curve", "in": ["lat"], "points": [[0, 0], [90, 40]]},
    "out": {"op": "add", "in": ["n", "c", 3]}},
    "layout": {"n": [0, 0], "lat": [0, 200], "c": [220, 200], "out": [440, 0]}}


def test_a_world_graph_converts_to_the_widget_and_back_unchanged():
    model = to_model(GRAPH)
    assert model["output"] == "out"
    assert model["nodes"]["out"]["inputs"] == ["n", "c", 3]
    assert model["nodes"]["c"]["params"]["points"] == [[0, 0], [90, 40]]
    assert model["nodes"]["n"]["pos"] == [0, 0]
    back = from_model(model)
    assert back["nodes"] == GRAPH["nodes"]
    assert back["layout"] == GRAPH["layout"]
    # An empty input is 0 to the engine; every op has a node.
    model["nodes"]["c"]["inputs"] = [None]
    assert from_model(model)["nodes"]["c"]["in"] == [0]
    assert set(SPECS) >= {n["op"] for n in GRAPH["nodes"].values()}


@needs_engine
def test_the_classic_graph_is_the_59_ground_and_costs_one():
    midgard = engine.module()
    terrain = {"height_scale": 40.0, "base": 2.0, "feature_size": 700.0, "octaves": 6}
    graph = {k: v for k, v in classic_graph(terrain).items() if k != "layout"}
    assert midgard.graph_cost(json.dumps(graph)) == pytest.approx(1.0)
    with pytest.raises(ValueError):
        midgard.graph_cost(json.dumps({"output": "nope", "nodes": {}}))
    # The same heights as the settings without a graph.
    shape = "bounded_area"
    plain = preview_settings({"terrain": terrain}, shape, 2000, 3)
    with_graph = preview_settings({"terrain": terrain, "graph": classic_graph(terrain)}, shape,
                                  2000, 3)
    assert "graph" in with_graph["terrain"] and "layout" not in with_graph["terrain"]["graph"]
    assert midgard.world_map(json.dumps(plain), 64, 32) == \
        midgard.world_map(json.dumps(with_graph), 64, 32)


@needs_engine
def test_a_map_for_every_shape_in_its_size_range():
    midgard = engine.module()
    recipe = {"terrain": {}, "graph": classic_graph({})}
    for shape in ("bounded_area", "flat_cylinder", "curved_cylinder", "sphere_outside",
                  "sphere_inside"):
        settings = preview_settings(recipe, shape, 1e9, 1)  # (too big: kept in range)
        pixels = midgard.world_map(json.dumps(settings), 32, 16)
        assert len(pixels) == 32 * 16 * 4
        assert pixels[3::4] == b"\xff" * (32 * 16)  # opaque
        assert len(set(pixels[0::4])) > 4, shape  # not one colour
    with pytest.raises(ValueError):
        midgard.world_map(json.dumps({"terrain": {}}), 8, 8)


@pytest.fixture
def panel(project, qtbot):
    project.undo_stack.push(world_types.SaveRecipeCommand(
        project, "wt_hills", {"terrain": {"sea_level": 0.0}, "graph": GRAPH}, "Hills"))
    project.undo_stack.clear()
    p = WorldTypePanel()
    qtbot.addWidget(p)
    p.resize(1400, 800)
    p.set_project(project)
    return p


def latest(project, world_type="wt_hills"):
    return world_types.recipe(project, world_type)


def test_graph_edits_are_saved_as_one_step_each(panel, project):
    assert panel.world_type == "wt_hills"
    assert set(panel.model.nodes) == {"n", "lat", "c", "out"}
    stack = project.undo_stack
    new = panel.graph_view.add_node("ridged", QPointF(0, 400))
    assert new in latest(project)["graph"]["nodes"]
    assert stack.count() == 1
    panel.graph_view.link(new, "out", 3)
    assert latest(project)["graph"]["nodes"]["out"]["in"] == ["n", "c", 3, new]
    # A drag is one step however many moves it takes.
    from midgard_editor.node_graph import edit
    for x in (10, 20, 30):
        edit(panel.model, panel.graph_saver, "Move", lambda m, x=x: m.move({new: (x, 400)}),
             moved=frozenset({new}))
    assert latest(project)["graph"]["layout"][new] == [30, 400]
    assert stack.count() == 3
    stack.undo()
    assert latest(project)["graph"]["layout"][new] == [0, 400]
    assert panel.model.nodes[new]["pos"] == [0, 400]  # the panel shows the undo
    stack.undo()
    stack.undo()
    assert new not in panel.model.nodes and latest(project)["graph"] == GRAPH
    stack.redo()
    assert new in panel.model.nodes


def test_a_nodes_settings_are_edited_in_the_form(panel, project):
    panel.graph_view.items_by_id["n"].setSelected(True)
    assert panel.node_form.rowCount() > 2
    panel.node_form.itemAt(panel.node_form.rowCount() - 1, panel.node_form.ItemRole.FieldRole)
    # The size field is a spin box: set it as the form would.
    boxes = [panel.node_form.itemAt(r, panel.node_form.ItemRole.FieldRole).widget()
             for r in range(1, panel.node_form.rowCount())]
    size = next(b for b in boxes if hasattr(b, "setValue") and b.value() == 300)
    size.setValue(450)
    size.editingFinished.emit()
    assert latest(project)["graph"]["nodes"]["n"]["size"] == 450
    project.undo_stack.undo()
    assert latest(project)["graph"]["nodes"]["n"]["size"] == 300


def test_climate_biomes_placements_and_fields_are_saved(panel, project):
    panel.has_climate.setChecked(True)
    assert latest(project)["terrain"]["climate"]["temperature"] == 14
    panel.add_biome()
    panel.set_biome_colour(0, 5, [0.1, 0.5, 0.2])
    biome = latest(project)["terrain"]["biomes"][0]
    assert biome["color"] == [0.1, 0.5, 0.2] and biome["name"] == "Biome 1"
    panel.add_rule("obj_tree")
    rule = latest(project)["terrain"]["placements"][0]
    assert rule["object"] == "obj_tree" and rule["spacing"] == 8
    panel.shapes["sphere_outside"].setChecked(True)
    assert latest(project)["shapes"] == ["sphere_outside"]
    assert panel.preview_shape.count() == 1  # the preview offers the shapes it suits
    panel.ground_fields["sea_level"].setValue(5)
    panel.ground_fields["sea_level"].editingFinished.emit()
    assert latest(project)["terrain"]["sea_level"] == 5
    for _ in range(6):
        project.undo_stack.undo()
    assert latest(project) == {"terrain": {"sea_level": 0.0}, "graph": GRAPH}
    assert not panel.has_climate.isChecked() and panel.biomes.rowCount() == 0


def test_world_types_are_made_duplicated_renamed_and_deleted(panel, project, monkeypatch):
    made = panel.new_world_type("Moons")
    assert made == "wt_moons" and panel.world_type == "wt_moons"
    assert "graph" in latest(project, made)
    copy_id = panel.duplicate()
    assert latest(project, copy_id) == latest(project, made)
    panel.name.setText("Small Moons")
    panel.name.editingFinished.emit()
    names = {t["id"]: t["name"] for t in world_types.load_all(project)}
    assert names[copy_id] == "Small Moons"
    assert panel.delete_world_type(confirm=False)
    assert copy_id not in {t["id"] for t in world_types.load_all(project)}
    project.undo_stack.undo()
    assert copy_id in {t["id"] for t in world_types.load_all(project)}
    # Not while a scene's world is made from it.
    settings = project.scene_settings("scn_main") if hasattr(project, "scene_settings") else {}
    settings = {**settings, "terrain": {"world_type": "wt_hills", "version": 1, "seed": 1}}
    with project.connect() as con:
        con.execute("UPDATE scene SET settings = ?", (json.dumps(settings),))
    panel.chooser.setCurrentIndex(panel.chooser.findData("wt_hills"))
    assert not panel.delete_world_type(confirm=False)
    with pytest.raises(world_types.WorldTypeError):
        world_types.DeleteWorldTypeCommand(project, "wt_hills")


@needs_engine
def test_the_preview_shows_a_map_its_cost_and_the_cooks_problems(panel):
    panel.update_preview()
    assert panel.map_image is not None and panel.map_image.width() == MAP_SIZE[0]
    assert "costs" in panel.cost.text()
    assert panel.problems.count() == 0
    # A size the recipe doesn't suit: the cook's complaint, and no map.
    panel.limit_size.setChecked(True)
    panel.size_min.setValue(10_000)
    panel.size_max.setValue(12_000)
    panel.size_max.editingFinished.emit()
    panel.update_preview()
    assert panel.problems.count() >= 1
