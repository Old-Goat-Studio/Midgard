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
    MAP_BOX,
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
    # The shape's usual map, in its proportions: a square area from above.
    assert panel.preview_shape.currentData() == "bounded_area"
    assert panel.projection.currentData() == "top_down"
    assert panel.map_image is not None
    assert (panel.map_image.width(), panel.map_image.height()) == (MAP_BOX[1], MAP_BOX[1])
    assert "costs" in panel.cost.text()
    assert panel.problems.count() == 0
    # A size the recipe doesn't suit: the cook's complaint, and no map.
    panel.limit_size.setChecked(True)
    panel.size_min.setValue(10_000)
    panel.size_max.setValue(12_000)
    panel.size_max.editingFinished.emit()
    panel.update_preview()
    assert panel.problems.count() >= 1


@needs_engine
def test_maps_in_each_projection_saved_as_images(panel, tmp_path):
    panel.preview_shape.setCurrentIndex(panel.preview_shape.findData("sphere_outside"))
    assert [panel.projection.itemData(i) for i in range(panel.projection.count())] == \
        ["equirectangular", "cube_faces"]
    panel.update_preview()
    assert (panel.map_image.width(), panel.map_image.height()) == (320, 160)
    panel.projection.setCurrentIndex(1)
    panel.update_preview()
    assert (panel.map_image.width(), panel.map_image.height()) == (213, 160)
    assert panel.map_image.pixelColor(2, 2).alpha() == 0  # off the cross
    path = tmp_path / "planet.png"
    assert panel.save_map(str(path), 400)
    from PySide6.QtGui import QImage
    saved = QImage(str(path))
    assert (saved.width(), saved.height()) == (400, 300)
    # A cylinder unrolled: round it across, along it down (2 pi 1 km by 5 km).
    panel.preview_shape.setCurrentIndex(panel.preview_shape.findData("curved_cylinder"))
    panel.preview_size.setValue(1000)
    assert panel.projection.currentData() == "unrolled"
    panel.update_preview()
    assert panel.map_image.height() == 160
    assert abs(panel.map_image.width() - 160 * 2 * 3.14159 * 1000 / 5000) <= 1


def a_full_texture_set(project, tmp_path, name="Mossy Rock"):
    """A texture set with every map: colour, normal, roughness and metal (combined on import),
    ambient occlusion and emission."""
    from test_asset_library import grey_picture

    from midgard_editor import asset_library as lib
    folder = tmp_path / name.replace(" ", "")
    folder.mkdir()
    for file, value in (("color.png", 120), ("normal.png", 200), ("roughness.png", 90),
                        ("metallic.png", 30), ("ao.png", 160), ("emissive.png", 10)):
        grey_picture(folder, file, value)
    return lib.import_asset(project, [folder], "textures", name, category="stone",
                            licence="CC-BY-4.0", author="Ann Author", title=name, roles={
        "color.png": "color", "normal.png": "normal", "roughness.png": "roughness",
        "metallic.png": "metallic", "ao.png": "occlusion", "emissive.png": "emissive"})


def test_ground_layers_and_biomes_take_texture_sets_with_every_map(panel, project, tmp_path,
                                                                     qtbot):
    from midgard_editor.docks.asset_window import TextureSetPicker
    rock = a_full_texture_set(project, tmp_path)
    keys = panel._ground_keys
    assert keys == [("ground", k) for k in world_types.GROUND_LAYERS]
    assert panel.ground_textures.item(0, 1).text() == "(its colour)"
    # The picker: each set once, its maps said, its material shown as it's picked.
    picker = TextureSetPicker(project, "Texture Set")
    qtbot.addWidget(picker)
    assert [a.name for a in picker.sets] == ["Mossy Rock"]
    assert "colour, normal, metal/rough, AO, emissive" in picker.list.item(0).text()
    picker.pick(0)
    assert picker.player.asset.name == "Mossy Rock"
    assert picker.player.choose.currentData()[0] == "material"  # (the whole material)
    picker.reject()
    # Rock: all five maps, and a tile.
    panel.set_ground_texture(("ground", "rock"), rock)
    g = latest(project)["terrain"]["ground"]["rock"]
    assert sorted(g["textures"]) == sorted(world_types.GROUND_SLOTS)
    assert g["textures"]["base_color"].startswith("tex:library/textures/")
    assert g["tile"] == world_types.DEFAULT_TILE
    row = keys.index(("ground", "rock"))
    assert panel.ground_textures.item(row, 1).text() == "Mossy Rock"
    boxes = panel.ground_textures.cellWidget(row, 2).boxes
    assert all(b.isEnabled() and b.isChecked() for b in boxes.values())  # (every map, used)
    assert "<img" in panel.ground_textures.item(row, 1).toolTip()
    tile = panel.ground_textures.cellWidget(row, 3)
    tile.setValue(2.5)
    tile.editingFinished.emit()
    assert latest(project)["terrain"]["ground"]["rock"]["tile"] == 2.5
    # A biome's lush ground; editing the biome's cells keeps it.
    panel.add_biome()
    biome_row = panel._ground_keys.index(("biome", 0, "texture"))
    assert panel.ground_textures.item(biome_row, 0).text() == "Biome 1 (biome)"
    panel.set_ground_texture(("biome", 0, "texture"), rock)
    panel.biomes.item(0, 0).setText("Moor")
    biome = latest(project)["terrain"]["biomes"][0]
    assert biome["name"] == "Moor" and len(biome["texture"]["textures"]) == 5
    assert panel.ground_textures.item(biome_row, 0).text() == "Moor (biome)"
    assert len(world_types.ground_textures(latest(project))) == 5  # (the same set's maps)
    # Cleared: its colour again; each change one step to undo.
    panel.ground_textures.setCurrentCell(row, 0)
    panel.set_ground_texture(("ground", "rock"), None)
    assert "rock" not in latest(project)["terrain"].get("ground", {})
    project.undo_stack.undo()
    assert latest(project)["terrain"]["ground"]["rock"]["tile"] == 2.5
    assert panel.ground_textures.item(row, 1).text() == "Mossy Rock"


@needs_engine
def test_the_cook_packs_and_credits_the_grounds_textures(panel, project, tmp_path):
    import zipfile

    from test_world_types import HABITAT

    from midgard_editor.cook import cook
    rock = a_full_texture_set(project, tmp_path)
    panel.set_ground_texture(("ground", "grass"), rock)
    settings = project.scene_settings()
    settings["body"] = HABITAT
    settings["terrain"] = {"world_type": "wt_hills", "version": 1, "seed": 5}
    project.set_scene_settings(settings)
    result = cook(project, tmp_path / "pkg")
    with zipfile.ZipFile(result.out_dir / "game.pak") as pak:
        names = pak.namelist()
    used = world_types.ground_textures(latest(project))
    assert len(used) == 5
    ids = {c["id"] for c in result.manifest["content"]}
    assert set(used) <= ids  # (every map packed, the engine's check content)
    assert len([n for n in names if n.endswith(".ktx2")]) >= 5
    assert any("Ann Author" in c for c in result.credits)  # (CC BY: credited)
    assert not [w for w in result.warnings if "ground uses texture" in w]


def test_the_grounds_textures_are_sent_to_the_views_once_each(panel, project, tmp_path):
    rock = a_full_texture_set(project, tmp_path)
    panel.set_ground_texture(("ground", "rock"), rock)
    panel.set_ground_texture(("ground", "snow"), rock)  # (the same maps: sent once)

    class View:
        def __init__(self):
            self.sent = []

        def set_texture(self, texture, data):
            assert data[:4] == b"\xabKTX"  # (KTX2, as the cook makes them)
            self.sent.append(texture)

    view, sent = View(), {}
    settings = {"terrain": latest(project)["terrain"]}
    world_types.send_ground_textures(view, project, settings, sent)
    assert sorted(view.sent) == sorted(world_types.ground_textures(latest(project)))
    assert len(view.sent) == 5
    world_types.send_ground_textures(view, project, settings, sent)
    assert len(view.sent) == 5  # (not again)


@needs_engine
def test_the_preview_draws_the_grounds_textures(panel, project, tmp_path):
    from PySide6.QtGui import QColor, QImage

    from midgard_editor import asset_library as lib
    folder = tmp_path / "Red"
    folder.mkdir()
    image = QImage(16, 16, QImage.Format.Format_RGB32)
    image.fill(QColor(230, 20, 20))
    image.save(str(folder / "red_color.png"))
    red = lib.import_asset(project, [folder], "textures", "Red", category="ground",
                           roles={"red_color.png": "color"})

    def redness() -> float:
        settings = preview_settings(latest(project), "bounded_area", 4000.0, 1)
        panel.render_view(settings)
        shot = panel.view_image.pixmap().toImage()
        lower = [shot.pixelColor(x, y) for x in range(0, shot.width(), 8)
                 for y in range(shot.height() * 3 // 4, shot.height(), 8)]
        return sum(c.redF() - c.greenF() for c in lower) / len(lower)

    before = redness()
    for layer in world_types.GROUND_LAYERS:
        panel.set_ground_texture(("ground", layer), red)
    after = redness()
    assert after > before + 0.2, (before, after)  # (the ground's red now)


def test_ground_textures_are_found_from_the_world_menu_and_the_terrain_panel(window, project):
    project.undo_stack.push(world_types.SaveRecipeCommand(project, "wt_a", {"terrain": {}}, "A"))
    project.undo_stack.push(world_types.SaveRecipeCommand(project, "wt_b", {"terrain": {}}, "B"))
    settings = project.scene_settings()
    settings["terrain"] = {"world_type": "wt_b", "version": 1, "seed": 1}
    project.set_scene_settings(settings)
    window.set_project(project)
    panel = window.world_type_panel
    panel.tabs.setCurrentIndex(0)
    window.ground_textures_action.trigger()
    assert panel.tabs.tabText(panel.tabs.currentIndex()) == "Ground Textures"
    assert panel.world_type == "wt_b"  # (the scene's)
    assert window.panels["world_types"].isCurrentTab()
    panel.tabs.setCurrentIndex(0)
    window.terrain_panel.textures.click()
    assert panel.tabs.tabText(panel.tabs.currentIndex()) == "Ground Textures"


def test_a_layers_maps_can_be_left_out_and_used_again(panel, project, tmp_path):
    rock = a_full_texture_set(project, tmp_path)
    panel.set_ground_texture(("ground", "rock"), rock)
    row = panel._ground_keys.index(("ground", "rock"))
    panel.ground_textures.cellWidget(row, 2).boxes["emissive"].setChecked(False)
    panel.ground_textures.cellWidget(row, 2).boxes["metallic_roughness"].setChecked(False)
    g = latest(project)["terrain"]["ground"]["rock"]
    assert g["off"] == ["metallic_roughness", "emissive"]
    assert len(g["textures"]) == 5  # (kept, to use again)
    used = world_types.ground_textures(latest(project))
    assert len(used) == 3 and g["textures"]["emissive"] not in used  # (not drawn nor packed)
    boxes = panel.ground_textures.cellWidget(row, 2).boxes
    assert not boxes["emissive"].isChecked() and boxes["normal"].isChecked()
    # Used again; each change one step to undo.
    boxes["emissive"].setChecked(True)
    assert latest(project)["terrain"]["ground"]["rock"]["off"] == ["metallic_roughness"]
    project.undo_stack.undo()
    assert latest(project)["terrain"]["ground"]["rock"]["off"] == ["metallic_roughness",
                                                                    "emissive"]
    assert not panel.ground_textures.cellWidget(row, 2).boxes["emissive"].isChecked()
    # A set without a map: its box greyed.
    from test_asset_library import grey_picture

    from midgard_editor import asset_library as lib
    plain = tmp_path / "Plain"
    plain.mkdir()
    grey_picture(plain, "plain_color.png", 77)  # (a picture of its own)
    flat = lib.import_asset(project, [plain], "textures", "Plain", category="ground",
                            roles={"plain_color.png": "color"})
    panel.set_ground_texture(("ground", "sand"), flat)
    sand = panel.ground_textures.cellWidget(panel._ground_keys.index(("ground", "sand")), 2)
    assert sand.boxes["base_color"].isChecked() and not sand.boxes["normal"].isEnabled()


def test_the_world_types_paint_layers_are_named_textured_and_painted(panel, project, tmp_path,
                                                                     qtbot):
    from test_world_types import HABITAT

    from midgard_editor import terrain_stamps
    from midgard_editor.docks.terrain_panel import TerrainPanel
    keys = panel._ground_keys
    assert ("ground", "paint_1") in keys and ("ground", "paint_2") in keys
    row = keys.index(("ground", "paint_1"))
    name = panel.ground_textures.cellWidget(row, 0)
    name.setText("Cobbles")
    name.editingFinished.emit()
    assert latest(project)["terrain"]["ground"]["paint_1"] == {"name": "Cobbles"}
    # A texture set: the name kept; cleared: the name still kept.
    rock = a_full_texture_set(project, tmp_path)
    panel.set_ground_texture(("ground", "paint_1"), rock)
    g = latest(project)["terrain"]["ground"]["paint_1"]
    assert g["name"] == "Cobbles" and len(g["textures"]) == 5
    assert panel.ground_textures.item(row, 0).text() == "Cobbles (paint layer 1)"
    # The scene made with it: the Terrain panel's Paint list calls it so, and paints it.
    settings = project.scene_settings()
    settings["body"] = HABITAT
    settings["terrain"] = {"world_type": "wt_hills", "version": 1, "seed": 5}
    project.set_scene_settings(settings)
    assert world_types.paint_layer_names(project) == {"paint_1": "Cobbles",
                                                      "paint_2": "Paint layer 2"}
    terrain = TerrainPanel()
    qtbot.addWidget(terrain)
    terrain.set_project(project)
    i = terrain.paint.findData("paint_1")
    assert terrain.paint.itemText(i) == "Cobbles"
    terrain.tool.setCurrentIndex(terrain.tool.findData("paint"))
    terrain.paint.setCurrentIndex(i)
    terrain.sculpt.setChecked(True)
    assert terrain.brush()["paint"] == "paint_1"
    project.undo_stack.push(terrain_stamps.StampCommand(project, {
        **terrain.brush(), "face": 0, "points": [[0.5, 0.5]]}))
    assert terrain_stamps.load(project)[-1][1]["paint"] == "paint_1"
    # Unnamed again: "Paint layer 1".
    name = panel.ground_textures.cellWidget(row, 0)
    name.setText("")
    name.editingFinished.emit()
    assert "name" not in latest(project)["terrain"]["ground"]["paint_1"]
    assert world_types.paint_layer_names(project)["paint_1"] == "Paint layer 1"
