# SPDX-License-Identifier: Apache-2.0
"""Point and spot lights dropped into a scene: Add ▸ Point Light / Spot Light, inside the
selected thing (a candle's flame), the Inspector's light rows, and the viewport's bulbs."""
import json

import pytest
from PySide6.QtWidgets import QAbstractItemView, QLabel

from midgard_editor import engine, meshes, transforms
from midgard_editor.docks.entity_form import EntityForm
from midgard_editor.docks.scene_panel import LIGHT_ABOVE, ScenePanel, describe
from midgard_editor.scene_model import Entity, parents_first

ON_ITEM = QAbstractItemView.DropIndicatorPosition.OnItem


@pytest.fixture
def panel(qtbot, project):
    p = ScenePanel()
    qtbot.addWidget(p)
    p.set_project(project)
    return p


def world_position(project, entity_id):
    by_id = {e.id: e for e in project.entities()}
    return transforms.position_of(transforms.world_matrix(by_id, entity_id))


def menu_action(panel, text):
    return next(a for a in panel.add.menu().actions() if text in a.text())


def a_candle(project, panel) -> str:
    panel.add_primitive("prim_cylinder")
    candle = project.entity(panel.selected_id())
    candle.components["transform"] = {"position": [4.0, 1.0, -2.0], "rotation": [0.0, 90.0, 0.0],
                                      "scale": [0.1, 0.3, 0.1]}
    project.put_entity(candle)
    return candle.id


def test_the_add_menu_drops_point_and_spot_lights(project, panel):
    menu_action(panel, "Point Light").trigger()
    point = project.entity(panel.selected_id())
    assert point.name == "Point Light" and point.parent is None
    light = point.components["light"]
    assert light["type"] == "point" and light["range"] > 0 and light["intensity"] > 0
    assert describe(point).startswith("💡")
    menu_action(panel, "Spot Light").trigger()
    spot = project.entity(panel.selected_id())
    assert spot.components["light"]["type"] == "spot" and describe(spot).startswith("🔦")
    # A spot shines down to begin with (along its -Z, turned down).
    m = transforms.compose(spot.components["transform"])
    assert m[1][2] == pytest.approx(1.0)  # -Z in the world: (0, -1, 0)
    # One step each on the undo stack.
    project.undo_stack.undo()
    assert project.entity(spot.id) is None


def test_a_light_added_with_a_candle_selected_goes_inside_it(project, panel):
    candle = a_candle(project, panel)
    panel.select(candle)
    flame = panel.add_light("point")
    entity = project.entity(flame)
    assert entity.parent == candle
    # Just above the candle's origin in the world, whatever its scale and turn.
    assert world_position(project, flame) == pytest.approx([4.0, 1.0 + LIGHT_ABOVE, -2.0])
    # It moves with the candle.
    moved = project.entity(candle)
    moved.components["transform"]["position"] = [0.0, 0.0, 0.0]
    project.put_entity(moved)
    assert world_position(project, flame) == pytest.approx([0.0, LIGHT_ABOVE, 0.0])


def test_a_light_dropped_onto_a_thing_goes_into_it_and_stays_put(project, panel):
    candle = a_candle(project, panel)
    panel.tree.setCurrentItem(None)  # (nothing selected: the light goes at the top)
    lamp = panel.add_light("point")
    assert project.entity(lamp).parent is None
    before = world_position(project, lamp)
    items = {panel.tree.topLevelItem(i).data(0, 256): panel.tree.topLevelItem(i)
             for i in range(panel.tree.topLevelItemCount())}
    # A light may go onto anything placed; other things only into folders and groups.
    assert panel.tree.drop_parent(items[candle], ON_ITEM, lamp) == (True, candle)
    assert panel.tree.drop_parent(items[lamp], ON_ITEM, candle) == (False, None)
    panel.move_entity(lamp, candle)
    assert project.entity(lamp).parent == candle
    assert world_position(project, lamp) == pytest.approx(before)  # (where it was)


def test_the_inspector_edits_a_lights_colour_reach_type_and_cone(qtbot, project, panel):
    lamp = panel.add_light("point")
    form = EntityForm(project, lamp)
    qtbot.addWidget(form)
    assert form.light_type.currentData() == "point"
    assert form.light_range is not None and form.inner_angle is None
    form.set_light_color([0.2, 0.4, 1.0])
    assert project.entity(lamp).components["light"]["color"] == [0.2, 0.4, 1.0]
    form.light_range.setValue(20.0)
    assert project.entity(lamp).components["light"]["range"] == 20.0
    form.intensity.setValue(7.5)
    assert project.entity(lamp).components["light"]["intensity"] == 7.5
    # Made a spot: it gets a cone (and the form, its cone rows).
    rebuilt = []
    form.rebuild.connect(rebuilt.append)
    form.light_type.setCurrentIndex(form.light_type.findData("spot"))
    light = project.entity(lamp).components["light"]
    assert light["type"] == "spot" and light["outer_angle"] >= light["inner_angle"]
    assert rebuilt == [lamp]
    spot_form = EntityForm(project, lamp)
    qtbot.addWidget(spot_form)
    spot_form.outer_angle.setValue(15.0)
    spot_form.inner_angle.setValue(40.0)  # (never wider than the outer)
    light = project.entity(lamp).components["light"]
    assert light["outer_angle"] == 15.0 and light["inner_angle"] == 15.0
    assert any("inner" in w.text() for w in spot_form.findChildren(QLabel))
    # Each change is a step to undo.
    project.undo_stack.undo()
    assert project.entity(lamp).components["light"]["inner_angle"] != 15.0


def test_the_sun_keeps_its_own_rows(qtbot, project):
    sun = next(e for e in project.entities() if e.components.get("light", {}).get("type")
               == "directional")
    form = EntityForm(project, sun.id)
    qtbot.addWidget(form)
    assert form.light_type is None and form.light_range is None
    assert form.follows_clock is not None and form.light_color is not None


# ---- what the viewport shows -----------------------------------------------------------
def scene_view(project, width=160, height=120):
    module = engine.module()
    if module is None:
        pytest.skip("needs the engine module")
    view = module.Viewport()
    try:
        view.attach_offscreen(width, height)
    except RuntimeError as e:
        pytest.skip(f"no offscreen rendering here: {e}")
    view.set_settings(json.dumps(project.scene_settings()))
    for entity in parents_first(project.entities()):
        asset = entity.components.get("mesh", {}).get("asset", "")
        if asset in meshes.PRIMITIVES:
            view.set_mesh(asset, meshes.to_glb(meshes.primitive(asset)))
        view.put_entity(json.dumps({"id": entity.id, "name": entity.name,
                                    "parent": entity.parent, "components": entity.components}))
    return view


def pixels(view) -> bytes:
    view.render()
    return bytes(view.read_pixels())


def test_the_viewport_shows_a_bulb_where_each_light_is_even_inside_a_candle(project, panel):
    lamp = panel.add_light("point")
    entity = project.entity(lamp)
    entity.components["transform"] = {"position": [0.0, 1.0, 0.0]}
    entity.components["light"]["color"] = [1.0, 0.1, 0.1]  # (red: easy to find)
    project.put_entity(entity)
    view = scene_view(project)
    view.set_camera([0.0, 1.0, 4.0], [0.0, 1.0, 0.0], 55.0)
    assert not view.show_lights  # (off unless asked for: the small previews have lamps)
    without = pixels(view)
    view.show_lights = True
    with_bulb = pixels(view)
    assert with_bulb != without

    def reddish(data: bytes) -> int:
        return sum(1 for i in range(0, len(data), 4)
                   if data[i] > 200 and data[i + 1] < 190 and data[i + 2] < 190)

    assert reddish(with_bulb) > reddish(without) + 10
    # Inside a box (a candle's wax): faint, but still there.
    box = Entity(project.next_entity_id(), "Wax", None, {
        "transform": {"position": [0.0, 1.0, 0.0], "scale": [1.0, 1.0, 1.0]},
        "mesh": {"asset": "prim_cube"}, "material": {"color": [0.2, 0.2, 0.2, 1.0]}})
    view.set_mesh("prim_cube", meshes.to_glb(meshes.primitive("prim_cube")))
    view.put_entity(json.dumps({"id": box.id, "name": box.name, "parent": None,
                                "components": box.components}))
    hidden = pixels(view)
    view.show_lights = False
    assert hidden != pixels(view)  # (the ghost)
    # Selected, it shows its reach too.
    view.show_lights = True
    view.selected = lamp
    assert view.selected == lamp
    assert pixels(view) != hidden


def test_the_viewport_dock_turns_the_bulbs_on_and_follows_the_selection(qtbot, settings,
                                                                         project):
    from midgard_editor.docks.viewport import ViewportPanel
    if engine.module() is None:
        pytest.skip("needs the engine module")
    dock = ViewportPanel()
    qtbot.addWidget(dock)
    assert dock.lights_action.isChecked()
    if dock.view is None:
        pytest.skip("no 3D view here")
    assert dock.view.show_lights
    dock.set_selected("e000002")
    assert dock.view.selected == "e000002"
    dock.set_selected(None)
    assert dock.view.selected == ""
    dock.set_show_lights(False)
    assert not dock.view.show_lights and not dock.lights_action.isChecked()


def test_the_game_has_the_light(project, panel, tmp_path):
    """Cooked, a light is only its light (the runtime draws no bulbs)."""
    import sqlite3

    from midgard_editor.cook import cook
    panel.add_light("spot")
    result = cook(project, tmp_path / "out")
    db = next(result.out_dir.glob("*.db"))
    with sqlite3.connect(db) as c:
        rows = [json.loads(r[0]) for r in c.execute("SELECT components FROM entity")]
    assert any(r.get("light", {}).get("type") == "spot" for r in rows)


def test_a_light_flickers_like_a_candle_a_torch_or_a_campfire(qtbot, project, panel):
    from midgard_editor.docks.entity_form import FLICKERS, flicker_kind
    lamp = panel.add_light("point")
    form = EntityForm(project, lamp)
    qtbot.addWidget(form)
    assert form.flicker.currentData() == "steady"
    assert not form.flicker_speed.isEnabled()  # (no flicker: nothing to be fast)
    # A kind sets how much and how fast together: one step to undo.
    form.flicker.setCurrentIndex(form.flicker.findData("torch"))
    light = project.entity(lamp).components["light"]
    assert (light["flicker"], light["flicker_speed"]) == FLICKERS["torch"][1:]
    assert form.flicker_amount.value() == FLICKERS["torch"][1]
    assert form.flicker_speed.isEnabled()
    project.undo_stack.undo()
    assert "flicker" not in project.entity(lamp).components["light"]
    assert form.flicker.currentData() == "steady"
    # Its own: changed one by one, it's a custom flicker.
    form.flicker.setCurrentIndex(form.flicker.findData("candle"))
    form.flicker_amount.setValue(0.45)
    assert project.entity(lamp).components["light"]["flicker"] == 0.45
    assert form.flicker.currentData() == "custom"
    form.flicker_speed.setValue(3.0)
    assert project.entity(lamp).components["light"]["flicker_speed"] == 3.0
    # Steady again: no flicker left in the scene's file.
    form.flicker.setCurrentIndex(form.flicker.findData("steady"))
    light = project.entity(lamp).components["light"]
    assert "flicker" not in light and "flicker_speed" not in light
    assert flicker_kind({"flicker": 0.7, "flicker_speed": 2.2}) == "fire"
    # A spot can flicker too (a torch's beam); the sun can't.
    spot = panel.add_light("spot")
    spot_form = EntityForm(project, spot)
    qtbot.addWidget(spot_form)
    assert spot_form.flicker is not None
    sun = next(e for e in project.entities()
               if e.components.get("light", {}).get("type") == "directional")
    sun_form = EntityForm(project, sun.id)
    qtbot.addWidget(sun_form)
    assert sun_form.flicker is None


def test_a_flickering_light_goes_into_the_game(project, panel, tmp_path):
    import sqlite3

    from midgard_editor.cook import cook
    lamp = project.entity(panel.add_light("point"))
    lamp.components["light"].update(flicker=0.3, flicker_speed=1.0)
    project.put_entity(lamp)
    result = cook(project, tmp_path / "out")
    db = next(result.out_dir.glob("*.db"))
    with sqlite3.connect(db) as c:
        rows = [json.loads(r[0]) for r in c.execute("SELECT components FROM entity")]
    assert any(r.get("light", {}).get("flicker") == 0.3 for r in rows)
