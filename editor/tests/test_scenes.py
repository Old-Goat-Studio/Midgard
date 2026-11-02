# SPDX-License-Identifier: Apache-2.0
"""A game's scenes (milestone 5.11a): new scenes start with the default entities; duplicates
copy entities (with new ids), settings and terrain stamps; scenes are renamed and deleted (not
the starting scene, nor the last); the starting scene is chosen; every change is one undoable
step; the cook packs every scene's content and starts the game in the starting scene, and the
engine's checks cover every scene."""
import json
import subprocess

import pytest

from midgard_editor import engine, scenes, terrain_stamps
from midgard_editor.about import runtime_candidates
from midgard_editor.cook import cook
from midgard_editor.docks.scenes_panel import START_MARK, ScenesPanel
from midgard_editor.project import MAIN_SCENE
from midgard_editor.scene_model import Entity

needs_engine = pytest.mark.skipif(engine.module() is None, reason="engine module not built")


def rows(project, scene):
    with project.connect() as con:
        return {
            "entities": con.execute("SELECT id, name, parent, components FROM entity WHERE "
                                    "scene = ? ORDER BY id", (scene,)).fetchall(),
            "stamps": con.execute("SELECT id, stamp FROM terrain_stamp WHERE scene = ? "
                                  "ORDER BY id", (scene,)).fetchall(),
            "settings": con.execute("SELECT settings FROM scene WHERE id = ?",
                                    (scene,)).fetchone()}


def test_a_new_scene_has_the_default_entities_with_their_own_ids(project):
    before = {e.id for e in project.all_entities()}
    command = scenes.AddSceneCommand(project, "Cave")
    project.undo_stack.push(command)
    assert command.scene == "scn_cave"
    assert [s["name"] for s in scenes.load_all(project)] == ["Main", "Cave"]
    made = project.entities("scn_cave")
    assert {e.name for e in made} == {e.name for e in project.entities(MAIN_SCENE)}
    assert not before & {e.id for e in made}  # ids are the project's: new ones
    world = next(e for e in made if e.name == "World")
    assert all(e.parent == world.id for e in made if e is not world)
    project.undo_stack.undo()
    assert [s["id"] for s in scenes.load_all(project)] == [MAIN_SCENE]
    assert {e.id for e in project.all_entities()} == before
    project.undo_stack.redo()
    assert scenes.name(project, "scn_cave") == "Cave"


def test_a_duplicate_copies_entities_settings_and_stamps(project):
    project.set_scene_settings({**project.scene_settings(), "sky": [0.5, 0.2, 0.1]})
    terrain_stamps._insert(project, MAIN_SCENE, [(None, {"tool": "raise", "radius": 5.0})])
    command = scenes.AddSceneCommand(project, "Main Copy", copy_of=MAIN_SCENE)
    project.undo_stack.push(command)
    copy, main = rows(project, command.scene), rows(project, MAIN_SCENE)
    assert copy["settings"] == main["settings"]
    assert [s[1] for s in copy["stamps"]] == [s[1] for s in main["stamps"]]
    assert [(e[1], e[3]) for e in copy["entities"]] == [(e[1], e[3]) for e in main["entities"]]
    ids = {e[0] for e in copy["entities"]}
    assert not ids & {e[0] for e in main["entities"]}
    assert all(e[2] in ids for e in copy["entities"] if e[2])  # parents are the copies
    project.undo_stack.undo()
    assert rows(project, command.scene)["settings"] is None


def test_rename_delete_and_the_starting_scene(project):
    project.undo_stack.push(scenes.AddSceneCommand(project, "Cave"))
    project.undo_stack.push(scenes.RenameSceneCommand(project, "scn_cave", "Deep Cave"))
    assert scenes.name(project, "scn_cave") == "Deep Cave"
    project.undo_stack.undo()
    assert scenes.name(project, "scn_cave") == "Cave"
    # The starting scene can't be deleted, nor the last one.
    with pytest.raises(scenes.SceneError):
        scenes.DeleteSceneCommand(project, MAIN_SCENE)
    project.undo_stack.push(scenes.SetStartSceneCommand(project, "scn_cave"))
    assert scenes.start_scene(project) == "scn_cave"
    assert scenes.load_all(project)[0]["id"] == "scn_cave"  # listed first
    terrain_stamps._insert(project, MAIN_SCENE, [(None, {"tool": "lower", "radius": 3.0})])
    kept = rows(project, MAIN_SCENE)
    project.undo_stack.push(scenes.DeleteSceneCommand(project, MAIN_SCENE))
    assert [s["id"] for s in scenes.load_all(project)] == ["scn_cave"]
    with pytest.raises(scenes.SceneError):
        scenes.DeleteSceneCommand(project, "scn_cave")
    project.undo_stack.undo()
    assert rows(project, MAIN_SCENE) == kept  # all of it back, as it was
    assert scenes.name(project, MAIN_SCENE) == "Main"
    project.undo_stack.undo()  # (the stamp was made outside the stack)
    assert scenes.start_scene(project) == MAIN_SCENE


def test_the_panel_lists_scenes_and_edits_them(project, qtbot, monkeypatch):
    panel = ScenesPanel()
    qtbot.addWidget(panel)
    panel.set_project(project)
    assert panel.list.count() == 1
    assert panel.list.item(0).text() == "Main" + START_MARK
    assert not panel.buttons["Delete"].isEnabled()  # the starting scene
    made = panel.new_scene("Town")
    assert panel.selected() == made and panel.list.count() == 2
    assert panel.buttons["Delete"].isEnabled()
    panel.rename("Old Town")
    assert scenes.name(project, made) == "Old Town"
    copy = panel.duplicate("Old Town at Night")
    assert panel.selected() == copy
    panel.make_start()
    assert scenes.start_scene(project) == copy
    assert panel.list.item(0).text() == "Old Town at Night" + START_MARK
    panel.list.setCurrentRow(1)
    assert panel.selected() == MAIN_SCENE
    assert panel.delete(confirm=False)
    assert [s["id"] for s in scenes.load_all(project)] == [copy, made]
    for _ in range(5):
        project.undo_stack.undo()
    assert [s["id"] for s in scenes.load_all(project)] == [MAIN_SCENE]
    assert panel.list.count() == 1


@needs_engine
def test_the_cook_packs_every_scene_and_starts_in_the_starting_one(project, tmp_path):
    command = scenes.AddSceneCommand(project, "Cave")
    project.undo_stack.push(command)
    cave = command.scene
    # Something only the cave has: a mesh the cook must pack, and an object it lacks.
    project.put_entity(Entity(project.next_entity_id(), "Pillar", None,
                              {"transform": {}, "mesh": {"asset": "prim_cylinder"}}), cave)
    project.put_entity(Entity(project.next_entity_id(), "Statue", None,
                              {"transform": {}, "object": {"id": "obj_missing"}}), cave)
    project.undo_stack.push(scenes.SetStartSceneCommand(project, cave))
    result = cook(project, tmp_path / "pkg")
    manifest = json.loads((result.out_dir / "manifest.json").read_text("utf-8"))
    assert manifest["entry"]["scene"] == cave
    assert "prim_cylinder" in {c["id"] for c in manifest["content"]}
    assert any("Statue" in w or "obj_missing" in w for w in result.warnings)
    # The engine checks every scene, naming it.
    report = engine.validate(result.out_dir)
    assert any(w.startswith(f"scene '{cave}': ") and "obj_missing" in w
               for w in report["warnings"]), report["warnings"]
    # The game starts in the cave.
    runtime = next((r for r in runtime_candidates() if r.is_file()), None)
    if runtime is None:
        pytest.skip("no runtime built")
    run = subprocess.run([str(runtime), "--package", str(result.out_dir), "--simulate", "1",
                          "--dump-scene"], capture_output=True, text=True, timeout=120)
    assert run.returncode == 0, run.stderr
    dumped = json.loads([x for x in run.stdout.splitlines() if x.startswith("{")][-1])
    assert "Pillar" in {e["name"] for e in dumped["entities"]}
