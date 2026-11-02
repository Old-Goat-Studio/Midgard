# SPDX-License-Identifier: Apache-2.0
"""Navigation for scripts (milestone 5.12): midgard.nav finds paths round the scenery of a flat
scene and over a world's terrain, in the game the editor cooks."""

from test_scripting import needs_scripting, simulate, write_script

from midgard_editor import world_types
from midgard_editor.cook import cook
from midgard_editor.project import MAIN_SCENE
from midgard_editor.scene_model import Entity

FLAT = '''
import midgard

def on_start(entity):
    way = midgard.nav.path((0, 0, -4), (0, 0, 4))
    midgard.log("ready", midgard.nav.ready)
    midgard.log("round the wall", len(way) > 2, [round(c, 1) for c in way[0]],
                [round(c, 1) for c in way[-1]])
    midgard.log("nearest", [round(c, 1) for c in midgard.nav.nearest((3, 1.2, 3))])
    midgard.log("off the ground", midgard.nav.path((0, 0, -4), (60, 0, 60)))
    midgard.log("too far", midgard.nav.nearest((0, 30, 0)))
'''

HILLS = '''
import midgard

asked = False

def on_update(entity, dt):
    global asked
    if asked or not midgard.nav.ready or not entity.on_ground:  # (it starts in the air)
        return
    asked = True
    x, y, z = entity.position
    way = midgard.nav.path((x, y, z), (x + 40, y, z + 20))
    midgard.log("over the hills", way is not None and len(way) >= 10)
'''


@needs_scripting
def test_scripts_find_paths_round_the_scenery(project, tmp_path):
    # The default scene's ground (10 x 10 m), with a wall across it (most of the way).
    project.put_entity(Entity(project.next_entity_id(), "Wall", None, {
        "transform": {"position": [-1.0, 1.0, 0.0]},
        "collider": {"shape": "box", "size": [8.0, 2.0, 0.5]}}))
    player = project.entity("e000004")
    player.components["script"] = {"module": "flat"}
    project.put_entity(player)
    write_script(project, "flat", FLAT)
    _dumped, run = simulate(cook(project, tmp_path / "pkg").out_dir, 2)
    assert run.returncode == 0, run.stderr
    log = run.stdout
    assert "ready True" in log
    # (Heights on the mesh are a little over the floor: Recast's 10 cm voxels.)
    assert "round the wall True [0.0, 0.1, -4.0] [0.0, 0.1, 4.0]" in log, log
    assert "nearest [3.0, 0.1, 3.0]" in log
    assert "off the ground None" in log
    assert "too far None" in log


@needs_scripting
def test_scripts_find_paths_over_a_worlds_terrain(project, tmp_path):
    project.undo_stack.push(world_types.SaveRecipeCommand(
        project, "wt_hills", {"terrain": {"height_scale": 6, "base": 6}}, "Hills"))
    settings = project.scene_settings(MAIN_SCENE)
    settings["body"] = {"id": "land", "shape": "bounded_area", "width": 2000, "length": 2000,
                        "gravity": 9.81, "origin": [0, 0, 0]}
    settings["terrain"] = {"world_type": "wt_hills", "version": 1, "seed": 3}
    project.set_scene_settings(settings)
    for e in project.entities(MAIN_SCENE):  # (terrain is the ground)
        if e.name == "Ground":
            project.delete_entity(e.id)
    player = project.entity("e000004")
    player.components["transform"]["position"] = [0.0, 20.0, 0.0]
    player.components["script"] = {"module": "hills"}
    project.put_entity(player)
    write_script(project, "hills", HILLS)
    _dumped, run = simulate(cook(project, tmp_path / "pkg").out_dir, 240)
    assert run.returncode == 0, run.stderr
    assert "over the hills True" in run.stdout, run.stdout
