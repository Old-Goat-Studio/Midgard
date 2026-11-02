# SPDX-License-Identifier: Apache-2.0
"""A game's scenes (milestone 5.11a): new scenes start with the default entities; duplicates
copy entities (with new ids), settings and terrain stamps; scenes are renamed and deleted (not
the starting scene, nor the last); the starting scene is chosen; every change is one undoable
step; the cook packs every scene's content and starts the game in the starting scene, and the
engine's checks cover every scene."""
import json
import subprocess

import pytest
from test_scripting import needs_scripting, simulate, write_script

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


# ---- one scene open at a time (step 2) ---------------------------------------------------------
def test_edits_go_to_the_open_scene_and_undo_finds_their_own(project):
    from midgard_editor.undo import AddEntityCommand, DeleteEntityCommand, SetSceneSettingsCommand
    command = scenes.AddSceneCommand(project, "Cave")
    project.undo_stack.push(command)
    cave = command.scene
    main_ids = {e.id for e in project.entities(MAIN_SCENE)}
    project.open_scene(cave)
    assert project.scene == cave
    assert {e.id for e in project.entities()} == {e.id for e in project.entities(cave)}
    rock = Entity(project.next_entity_id(), "Rock", None, {"transform": {}})
    project.undo_stack.push(AddEntityCommand(project, rock))
    project.undo_stack.push(SetSceneSettingsCommand(
        project, {**project.scene_settings(), "sky": [1.0, 0.0, 0.0]}, "sky"))
    assert project.entity_scene(rock.id) == cave
    assert project.scene_settings(cave)["sky"] == [1.0, 0.0, 0.0]
    assert project.scene_settings(MAIN_SCENE)["sky"] != [1.0, 0.0, 0.0]
    # Back in the main scene, a deletion there, then undo everything: each in its own scene.
    project.open_scene(MAIN_SCENE)
    player = next(e for e in project.entities() if e.name == "Player")
    project.undo_stack.push(DeleteEntityCommand(project, player))
    project.open_scene(cave)
    project.undo_stack.undo()  # the player comes back to the main scene, not the cave
    assert project.entity_scene(player.id) == MAIN_SCENE
    project.undo_stack.undo()
    project.undo_stack.undo()
    assert project.entity(rock.id) is None
    assert {e.id for e in project.entities(MAIN_SCENE)} == main_ids
    project.undo_stack.redo()  # the rock goes back into the cave
    assert project.entity_scene(rock.id) == cave


def test_stamps_keep_their_scene_and_a_deleted_open_scene_closes(project):
    command = scenes.AddSceneCommand(project, "Cave")
    project.undo_stack.push(command)
    project.open_scene(command.scene)
    project.undo_stack.push(terrain_stamps.StampCommand(project, {"tool": "raise",
                                                                  "radius": 4.0}))
    assert len(terrain_stamps.load(project, command.scene)) == 1
    assert terrain_stamps.load(project, MAIN_SCENE) == []
    opened = []
    project.scene_opened.connect(opened.append)
    project.undo_stack.push(scenes.DeleteSceneCommand(project, command.scene))
    assert project.scene == MAIN_SCENE and opened == [MAIN_SCENE]  # the starting scene
    project.undo_stack.undo()
    project.open_scene(command.scene)
    project.undo_stack.undo()  # the stroke
    project.undo_stack.undo()  # making the cave: it's open, so the starting scene opens
    assert project.scene == MAIN_SCENE


def test_the_window_edits_the_scene_it_opens_and_plays_it(window, project, monkeypatch,
                                                         tmp_path):
    from midgard_editor import main_window as main_window_module
    from midgard_editor.docks.scene_panel import ID_ROLE
    assert window.open_project(project.config_path)
    opened = window.project
    window.scenes_panel.new_scene("Cave")
    cave = "scn_cave"
    assert not window.scenes_panel.buttons["Open"].isHidden()
    window.scenes_panel.list.setCurrentRow(1)
    window.scenes_panel.open_selected()
    assert opened.scene == cave
    assert window.windowTitle() == "Midgard - HelloCube - Cave"
    assert window.scenes_panel.list.item(1).font().italic()  # marked as open
    assert not window.scenes_panel.list.item(0).font().italic()
    assert "Cave" in window.scene_panel.scene_name.text()
    tree_ids = set()
    root = window.scene_panel.tree.invisibleRootItem()
    stack = [root.child(i) for i in range(root.childCount())]
    while stack:
        item = stack.pop()
        tree_ids.add(item.data(0, ID_ROLE))
        stack += [item.child(i) for i in range(item.childCount())]
    assert tree_ids == {e.id for e in opened.entities(cave)}
    # Play starts in the open scene, keeping its own place for Continue.
    cooked = []
    monkeypatch.setattr(main_window_module, "runtime_candidates", lambda: [tmp_path / "rt"])
    monkeypatch.setattr(window, "cook_project",
                        lambda entry_scene=None: cooked.append(entry_scene) or tmp_path / "pkg")
    started = []
    monkeypatch.setattr(window.session, "start",
                        lambda runtime, package, extra=None: started.append(extra))
    window.play()
    assert cooked == [cave]
    assert window.play_folder() == opened.work_dir / "play" / cave
    # Renaming the open scene renames the title.
    window.scenes_panel.rename("Crystal Cave")
    assert window.windowTitle() == "Midgard - HelloCube - Crystal Cave"


@needs_engine
def test_play_cooks_the_open_scene_as_the_start_and_recook_keeps_it(project, tmp_path):
    from midgard_editor.cook import recook
    command = scenes.AddSceneCommand(project, "Cave")
    project.undo_stack.push(command)
    result = cook(project, tmp_path / "pkg", entry_scene=command.scene)
    assert result.manifest["entry"]["scene"] == command.scene
    again, _changed = recook(project, result.out_dir, database=True)
    assert again.manifest["entry"]["scene"] == command.scene
    assert cook(project, tmp_path / "pkg2").manifest["entry"]["scene"] == MAIN_SCENE


# ---- scene exits and arrival points (step 3) -------------------------------------------------
def test_scene_exits_and_arrivals_are_added_and_edited_in_the_inspector(project, qtbot):
    from midgard_editor.docks.entity_form import EntityForm
    from midgard_editor.docks.scene_panel import ScenePanel
    panel = ScenePanel()
    qtbot.addWidget(panel)
    project.undo_stack.push(scenes.AddSceneCommand(project, "Cave"))
    project.open_scene("scn_cave")
    panel.set_project(project)
    arrival = panel.add_arrival()
    assert project.entity(arrival).components["arrival"] == {"name": "Arrival"}
    project.open_scene(MAIN_SCENE)
    door = panel.add_scene_exit()
    assert project.entity_scene(door) == MAIN_SCENE
    assert project.entity(door).components["scene_exit"]["scene"] == ""

    form = EntityForm(project, door)
    qtbot.addWidget(form)
    assert form.exit_arrival.count() == 1  # no scene yet: only "the player start"
    form.exit_scene.setCurrentIndex(form.exit_scene.findData("scn_cave"))
    exit_ = project.entity(door).components["scene_exit"]
    assert exit_["scene"] == "scn_cave" and exit_["arrival"] == ""
    assert form.exit_arrival.findData(arrival) > 0  # the cave's arrival point is offered
    form.exit_arrival.setCurrentIndex(form.exit_arrival.findData(arrival))
    form.exit_activation.setCurrentIndex(form.exit_activation.findData("use"))
    form.exit_fade.setValue(1.5)
    exit_ = project.entity(door).components["scene_exit"]
    assert (exit_["arrival"], exit_["activation"], exit_["fade"]) == (arrival, "use", 1.5)
    for _ in range(4):
        project.undo_stack.undo()
    assert project.entity(door).components["scene_exit"]["scene"] == ""
    assert form.exit_scene.currentData() == ""  # the form follows the undo


@needs_engine
def test_the_cook_refuses_an_exit_to_nowhere_and_takes_a_sound_one(project, tmp_path):
    from midgard_editor.cook import CookError
    exit_entity = Entity(project.next_entity_id(), "Door", None, {
        "transform": {}, "scene_exit": {"scene": "scn_nowhere", "arrival": "",
                                        "activation": "enter"}})
    project.put_entity(exit_entity)
    with pytest.raises(CookError, match="scn_nowhere"):
        cook(project, tmp_path / "bad")
    command = scenes.AddSceneCommand(project, "Cave")
    project.undo_stack.push(command)
    arrival = Entity(project.next_entity_id(), "In", None,
                     {"transform": {}, "arrival": {"name": "In"}})
    project.put_entity(arrival, command.scene)
    exit_entity.components["scene_exit"] = {"scene": command.scene, "arrival": arrival.id,
                                            "activation": "use"}
    project.put_entity(exit_entity)
    assert cook(project, tmp_path / "good").manifest


# ---- changing scenes in the game (step 4) ----------------------------------------------------
def run_game(package, frames: int) -> dict:
    runtime = next((r for r in runtime_candidates() if r.is_file()), None)
    if runtime is None:
        pytest.skip("no runtime built")
    run = subprocess.run([str(runtime), "--package", str(package), "--simulate", str(frames),
                          "--dump-scene"], capture_output=True, text=True, timeout=120)
    assert run.returncode == 0, run.stderr
    return json.loads([x for x in run.stdout.splitlines() if x.startswith("{")][-1])


@needs_engine
def test_the_game_goes_through_a_scene_exit_to_the_arrival(project, tmp_path):
    command = scenes.AddSceneCommand(project, "Cave")
    project.undo_stack.push(command)
    cave = command.scene
    arrival = Entity(project.next_entity_id(), "From the Main", None,
                     {"transform": {"position": [6.0, 0.6, -2.0]}, "arrival": {"name": "In"}})
    project.put_entity(arrival, cave)
    # The main scene's player starts above a pit whose bottom is the exit: it falls in.
    player = next(e for e in project.entities(MAIN_SCENE) if e.name == "Player")
    player.components["transform"]["position"] = [0.0, 4.0, 0.0]
    project.put_entity(player)
    project.put_entity(Entity(project.next_entity_id(), "Pit", None, {
        "transform": {"position": [0.0, 0.75, 0.0]},
        "scene_exit": {"scene": cave, "arrival": arrival.id, "activation": "enter",
                       "size": [2.0, 1.5, 2.0], "fade": 0.25}}))
    package = cook(project, tmp_path / "pkg").out_dir
    before = run_game(package, 5)  # still falling
    assert before["scene"] == MAIN_SCENE
    after = run_game(package, 150)  # fell in, faded out and in
    assert after["scene"] == cave
    cave_player = next(e for e in after["entities"] if e["name"] == "Player")
    assert abs(cave_player["position"][0] - 6.0) < 0.3
    assert abs(cave_player["position"][2] + 2.0) < 0.3


# ---- scripts move between scenes (step 5) ---------------------------------------------------
GUIDE = '''
import midgard

gone = False  # (the module stays loaded across scenes: once is enough)

def on_start(entity):
    midgard.log("main start; in", midgard.rooms.current, "from", repr(midgard.rooms.previous))
    try:
        midgard.rooms.change("scn_nowhere")
    except ValueError as e:
        midgard.log("refused:", e)

def on_update(entity, dt):
    global gone
    if midgard.time() > 0.3 and not gone:
        gone = True
        midgard.rooms.change("scn_cave", ARRIVAL, fade=0.1)

def on_room_exit(entity, going_to):
    midgard.log("leaving main for", going_to)
'''

CAVE = '''
import midgard

def on_start(entity):
    midgard.log("cave start")

def on_room_enter(entity, came_from):
    midgard.log("arrived in", midgard.rooms.current, "from", came_from)
    midgard.rooms.take_exit(midgard.find("Lever"))  # a script-only exit, straight back
'''


@needs_scripting
def test_scripts_change_scenes_and_hear_about_it(project, tmp_path):
    command = scenes.AddSceneCommand(project, "Cave")
    project.undo_stack.push(command)
    cave = command.scene
    arrival = Entity(project.next_entity_id(), "In", None,
                     {"transform": {"position": [5.0, 0.6, 5.0]}, "arrival": {"name": "In"}})
    project.put_entity(arrival, cave)
    project.put_entity(Entity(project.next_entity_id(), "Cave Script", None,
                              {"transform": {}, "script": {"module": "cave"}}), cave)
    project.put_entity(Entity(project.next_entity_id(), "Lever", None, {
        "transform": {"position": [9.0, 1.0, 9.0]},
        "scene_exit": {"scene": MAIN_SCENE, "activation": "script", "fade": 0.1}}), cave)
    player = next(e for e in project.entities(MAIN_SCENE) if e.name == "Player")
    player.components["script"] = {"module": "guide"}
    project.put_entity(player)
    write_script(project, "guide", GUIDE.replace("ARRIVAL", repr(arrival.id)))
    write_script(project, "cave", CAVE)
    package = cook(project, tmp_path / "pkg").out_dir
    dumped, run = simulate(package, 90)
    assert run.returncode == 0, run.stderr
    log = run.stdout
    assert "main start; in scn_main from ''" in log
    assert "refused: there is no scene 'scn_nowhere' in the game" in log
    assert "leaving main for scn_cave" in log
    assert log.index("cave start") < log.index("arrived in scn_cave from scn_main")
    # The cave's script took its lever straight back: main started again.
    assert log.count("main start; in scn_main") == 2
    assert dumped["scene"] == MAIN_SCENE
