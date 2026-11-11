# SPDX-License-Identifier: Apache-2.0
"""The World Tour sample (milestone 5.18 step 1): a hub and one world of each shape, each the
engine's own sample world type at its own size and seed."""
import json

import pytest

from midgard_editor import engine, samples, scenes, world_tour
from midgard_editor.cook import cook
from midgard_editor.project import MAIN_SCENE

needs_engine = pytest.mark.skipif(
    engine.module() is None or not hasattr(engine.module(), "sample_world_types"),
    reason="needs the engine module (its sample world types)")


@pytest.fixture(scope="module")
def tour(tmp_path_factory, qapp):
    if engine.module() is None:
        pytest.skip("needs the engine module")
    return world_tour.make_world_tour(tmp_path_factory.mktemp("tour"))


def by_name(project, scene):
    return {e.name: e for e in project.entities(scene)}


@needs_engine
def test_a_hub_and_a_world_of_each_shape_at_the_samples_sizes(tour):
    names = {s["id"]: s["name"] for s in scenes.load_all(tour)}
    assert set(names.values()) == {"Main", "Archipelago", "Habitat", "Planet", "Hollow World"}
    samples_by_id = {s["id"]: s for s in engine.module().sample_world_types()}
    shapes = set()
    for scene, name in names.items():
        if scene == MAIN_SCENE:
            continue
        settings = tour.scene_settings(scene)
        sample = samples_by_id[settings["terrain"]["world_type"]]
        body = json.loads(sample["body"])
        assert settings["terrain"]["seed"] == body["seed"]
        for key in ("shape", "radius", "length", "width"):
            assert settings["body"].get(key) == body.get(key), (name, key)
        shapes.add(settings["body"]["shape"])
        assert settings["weather"]  # (each its own)
        # The arrival stands on the ground, above the sea, with the pad home behind it.
        entities = by_name(tour, scene)
        arrival = entities["From the Hub"].components["transform"]["position"]
        pad = entities["Pad Home"].components
        assert arrival[1] >= world_tour.ABOVE_SEA + 1.0
        assert pad["scene_exit"]["scene"] == MAIN_SCENE
        assert abs(pad["transform"]["position"][1] - arrival[1]) < 0.01
        assert entities["Sun"].components["light"]["follows_clock"] is True
    assert shapes == {"bounded_area", "curved_cylinder", "sphere_outside", "sphere_inside"}
    assert tour.scene_settings(scenes.load_all(tour)[0]["id"]) is not None


@needs_engine
def test_every_door_and_pad_leads_to_an_arrival_there(tour):
    arrivals = {}
    for s in scenes.load_all(tour):
        for e in tour.entities(s["id"]):
            if "arrival" in e.components:
                arrivals.setdefault(s["id"], set()).add(e.id)
    exits = 0
    for s in scenes.load_all(tour):
        for e in tour.entities(s["id"]):
            leads = e.components.get("scene_exit")
            if not leads:
                continue
            exits += 1
            target = s["id"] if leads["scene"] == "@space" else leads["scene"]
            assert leads["arrival"] in arrivals[target], (s["name"], e.name)
    assert exits == 4 + 4 + 2  # (four doors, four pads home, the dock and the exit site)
    planet = next(s["id"] for s in scenes.load_all(tour) if s["name"] == "Planet")
    assert tour.scene_settings(planet)["ceiling"] == world_tour.PLANET_CEILING
    assert "player.flying = True" in (tour.root / "scripts" / "launch_pad.py").read_text("utf-8")
    assert by_name(tour, planet)["Launch Pad"].components["script"]["module"] == "launch_pad"


@needs_engine
def test_the_tour_cooks_and_the_engine_loads_it(tour, tmp_path):
    result = cook(tour, tmp_path / "pkg")  # (the engine's own check: raises if it can't load)
    # (The hollow world's sample ground is a little over the streaming budget: step 4.)
    assert [w for w in result.warnings if "costs" not in w] == []


def test_a_sample_gets_a_name_not_taken(tmp_path):
    assert samples.free_name(tmp_path, "WorldTour") == "WorldTour"
    (tmp_path / "WorldTour").mkdir()
    (tmp_path / "WorldTour2").mkdir()
    assert samples.free_name(tmp_path, "WorldTour") == "WorldTour3"
    assert [label for label, *_ in samples.samples()] == ["World Tour", "Scene Tour"]


@needs_engine
def test_file_new_sample_project_makes_and_opens_it(window, settings, tmp_path):
    settings.set("projects_dir", str(tmp_path / "projects"))
    project = window.new_sample(world_tour.TOUR)
    assert project is not None and project.name == "WorldTour"
    assert project.root == tmp_path / "projects" / "WorldTour"
    again = window.new_sample(world_tour.TOUR)
    assert again.name == "WorldTour2"
    window.new_sample(samples.TOUR)
    assert window.project.name == "SceneTour"


# ---- step 2: life ------------------------------------------------------------------------------
@needs_engine
def test_the_sounds_are_midgards_cc0_samples_in_the_projects_library(tour):
    from midgard_editor import asset_library as lib
    assets, problems = lib.scan(tour)
    assert problems == []
    by_name = {a.name: a for a in assets}
    for name in ("Footsteps on grass", "Footsteps on snow", "Sea", "Calm wind", "Desert Theme",
                 "Superhero", "Universal Animation Library"):
        assert by_name[name].licence == "CC0-1.0", name
    assert by_name["Desert Theme"].author == "yd"
    assert by_name["Sea"].author == "Nox Sound"


@needs_engine
def test_each_scene_has_the_character_and_its_own_ambience(tour):
    from midgard_editor import ambience, music
    sets = {a["ambience"]["scenes"][0]: a for a in ambience.load_all(tour)}
    for s in scenes.load_all(tour):
        player = by_name(tour, s["id"])["Player"].components
        assert player["character"]["model"] == world_tour.HERO
        assert player["character"]["ik"] == {"feet": True}
        assert set(player["footsteps"]["sounds"]) >= {"grass", "rock", "sand", "snow"}
        assert by_name(tour, s["id"])["Camera"].components["camera"]["view"] == "third"
        assert s["id"] in sets, s["name"]
    [playlist] = music.load_all(tour)
    assert playlist["playlist"]["scenes"] == []  # (everywhere)
    assert "Walker" in by_name(tour, MAIN_SCENE)


@needs_engine
def test_the_running_tour_plays_its_music_and_ambience(tour, tmp_path):
    import subprocess

    from midgard_editor import about
    if not about.runtime_candidates():
        pytest.skip("the runtime isn't built here")
    press_new_game(tour)
    out = cook(tour, tmp_path / "pkg").out_dir
    run = subprocess.run([str(about.runtime_candidates()[0]), "--package", str(out),
                          "--simulate", "300", "--dump-scene"],
                         capture_output=True, text=True, timeout=120)
    assert run.returncode == 0, run.stderr
    dump = json.loads(run.stdout[run.stdout.index("{"):run.stdout.rindex("}") + 1])
    audio = dump["audio"]
    assert audio["music"]["playlist"] == "mus_tour"
    assert audio["ambience"]["set"] == f"amb_{MAIN_SCENE}"
    # The walker has set off, and its steps sound on the hub's stone.
    walker = next(e for e in dump["entities"] if e.get("name") == "Walker")
    assert walker["position"][0] > -6.5
    assert any(f["surface"] == "stone" for f in audio["footsteps"])


@needs_engine
def test_a_world_greets_you_with_a_clear_sky(tour, tmp_path):
    """The first time you arrive, an hour of clear weather (then the world's own)."""
    import subprocess

    from midgard_editor import about
    if not about.runtime_candidates():
        pytest.skip("the runtime isn't built here")
    archipelago = next(s["id"] for s in scenes.load_all(tour) if s["name"] == "Archipelago")
    out = cook(tour, tmp_path / "pkg", entry_scene=archipelago).out_dir
    run = subprocess.run([str(about.runtime_candidates()[0]), "--package", str(out),
                          "--simulate", "60", "--dump-scene"],
                         capture_output=True, text=True, timeout=120)
    assert run.returncode == 0, run.stderr
    dump = json.loads(run.stdout[run.stdout.rindex("\n{") + 1:])
    assert dump["weather"]["now"] == "clear" and dump["weather"]["forced"] is True


@needs_engine
def test_the_tour_comes_with_its_checklist(tour):
    checklist = (tour.root / "CHECKLIST.md").read_text("utf-8")
    for world in ("Archipelago", "Habitat", "Planet", "Hollow World", "On your phone"):
        assert world in checklist


# (The title and game over screens pause the game: on_update isn't called while they show, so
# the tester presses their buttons from on_start, which still runs, and from a stats callback.)
EXIT_SCRIPT = """
import midgard

s = {"frame": 0, "died": False}


def health_changed(stat, before, after):
    if stat == "health" and after <= 0.0 and not s["died"]:
        s["died"] = True
        print("died", midgard.ui.screens, after)
        midgard.ui.click("scr_game_over", "again")
        s["frame"] = 1000


def on_start(entity):
    print("opens at", midgard.ui.screens)
    midgard.ui.click("scr_title", "play")
    print("new game", midgard.ui.screens)
    midgard.stats.on_change(health_changed)


def on_update(entity, dt):
    s["frame"] += 1
    f = s["frame"]
    if f == 1:
        midgard.player().position = midgard.find("Coin 1").position
    elif f == 30:
        print("score", midgard.stats.get("score"))
        midgard.player().position = midgard.find("Fire Pit").position
    elif f == 1010:
        print("again", midgard.ui.screens, midgard.stats.get("health"))
"""


START_SCRIPT = """
import midgard


def on_start(entity):
    midgard.ui.click("scr_title", "play")  # (New game: the title pauses the game till then)
"""


def press_new_game(tour) -> None:
    """A script that presses the title's New game as the game starts (as a player would)."""
    from midgard_editor.scene_model import Entity
    hub = next(e.id for e in tour.entities(MAIN_SCENE) if e.is_folder)
    tour.put_entity(Entity(tour.next_entity_id(), "Starter", hub, {
        "transform": {}, "script": {"module": "starter"}}), MAIN_SCENE)
    (tour.root / "scripts" / "starter.py").write_text(START_SCRIPT, encoding="utf-8")


@needs_engine
def test_the_exit_the_tour_opens_at_its_title_scores_dies_and_respawns(tmp_path):
    """6.0's exit: the title screen, New game; a coin raises the score with a note; the fire
    pit burns health away to the game over screen; Try again respawns, refilled."""
    import subprocess

    from midgard_editor import about, menus
    from midgard_editor.scene_model import Entity
    if not about.runtime_candidates():
        pytest.skip("the runtime isn't built here")
    tour = world_tour.make_world_tour(tmp_path / "exit")
    roles = {s["screen"].get("role"): s["id"] for s in menus.load_all(tour)}
    assert roles == {"splash": "scr_splash", "title": "scr_title", "settings": "scr_settings",
                     "pause": "scr_pause", "game_over": "scr_game_over"}
    assert menus.problems(tour) == []
    title = menus.load(tour, "scr_title")["screen"]
    assert {e["id"]: e.get("text") for e in title["elements"]}["play"] == "New game"
    hub = next(e.id for e in tour.entities(MAIN_SCENE) if e.is_folder)
    tour.put_entity(Entity(tour.next_entity_id(), "Exit Tester", hub, {
        "transform": {}, "script": {"module": "exit_tester"}}), MAIN_SCENE)
    (tour.root / "scripts" / "exit_tester.py").write_text(EXIT_SCRIPT, encoding="utf-8")
    out = cook(tour, tmp_path / "pkg").out_dir
    run = subprocess.run([str(about.runtime_candidates()[0]), "--package", str(out),
                          "--simulate", "255", "--dump-scene"],
                         capture_output=True, text=True, timeout=180)
    assert run.returncode == 0, run.stderr
    lines = run.stdout.splitlines()
    assert "opens at ['scr_title']" in lines
    assert "new game []" in lines
    assert "score 10" in lines
    died = next(line for line in lines if line.startswith("died"))
    assert "['scr_game_over']" in died and died.endswith(" 0.0")
    assert "again [] 100.0" in lines
    # (the coin's note, just after it's picked up: notes fade after a few seconds)
    run = subprocess.run([str(about.runtime_candidates()[0]), "--package", str(out),
                          "--simulate", "25", "--dump-scene"],
                         capture_output=True, text=True, timeout=180)
    dump = json.loads(run.stdout[run.stdout.rindex("\n{") + 1:])
    assert "Picked up a coin (+10)" in dump["hud"]["notes"]


def test_the_exit_of_6_0b_the_tours_ground_is_textured_with_a_path(tour, tmp_path):
    """6.0b's exit: every world's ground layers have texture sets made with the texture
    generator (colour, normal, occlusion, metal/roughness; the Hollow World's moss glows), the
    Planet's biomes their own, a cobbled path painted where you arrive; cooked, packed and
    credited, with no warnings."""
    import zipfile

    from midgard_editor import asset_library, terrain_stamps, world_types
    sets = {a.name: a for a in asset_library.scan(tour)[0] if a.type == "textures"}
    assert set(sets) == {name for name, *_ in world_tour.GROUND_SETS}
    assert all(a.licence == "CC0-1.0" for a in sets.values())
    for _world, sample_id, _colour in world_tour.WORLDS:
        ground = world_types.recipe(tour, sample_id)["terrain"]["ground"]
        assert set(ground) == set(world_tour.GROUND_OF)
        for layer in ground.values():
            assert {"base_color", "normal", "occlusion", "metallic_roughness"} <= set(
                layer["textures"])
        assert ground["paint_1"]["name"] == "Path"
    hollow = world_types.recipe(tour, "wt_sample_hollow")["terrain"]["ground"]["grass"]
    assert "emissive" in hollow["textures"]  # (glowing moss)
    planet = world_types.recipe(tour, "wt_sample_temperate_planet")["terrain"]
    assert all("texture" in b and "dry_texture" in b for b in planet["biomes"])
    for name, *_ in world_tour.WORLDS:
        scene = next(s["id"] for s in scenes.load_all(tour) if s["name"] == name)
        paints = [s for _id, s in terrain_stamps.load(tour, scene) if s.get("paint") == "paint_1"]
        assert paints and len(paints[0]["points"]) >= 2, name
    # Cooked: every map packed (the engine's check finds them all), and credited.
    archipelago = next(s["id"] for s in scenes.load_all(tour) if s["name"] == "Archipelago")
    result = cook(tour, tmp_path / "pkg", entry_scene=archipelago)
    assert not [w for w in result.warnings if "texture" in w.lower()]
    used = {t for _who, t in world_types.used_ground_textures(tour)}
    assert used <= {c["id"] for c in result.manifest["content"]}
    with zipfile.ZipFile(result.out_dir / "game.pak") as pak:
        assert sum(n.endswith(".ktx2") for n in pak.namelist()) >= len(used)
    credits = (result.out_dir / "CREDITS.txt").read_text("utf-8")
    assert "Cobbles" in credits and "Glowmoss" in credits
