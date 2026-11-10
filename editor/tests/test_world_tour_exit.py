# SPDX-License-Identifier: Apache-2.0
"""Phase 5's exit (milestone 5.18 step 3), on the World Tour as the game runs it:

  - each world the game makes is the one the editor shows (the same world hash);
  - the player walks all the way round the cylinder and both spheres (at 40 m/s, on a route)
    with gravity holding them to the ground the whole way, and comes back where they started;
  - a full day and night passes on each of them meanwhile;
  - on the planet, the player rises out of the atmosphere into space and comes back down;
  - the player's character is animated in all three camera views.
"""
import json
import math
import subprocess

import pytest

from midgard_editor import about, engine, scenes, world_tour, world_types
from midgard_editor.cook import cook
from midgard_editor.scene_model import Entity

pytestmark = pytest.mark.skipif(
    engine.module() is None or not hasattr(engine.module(), "sample_world_types") or
    not about.runtime_candidates(), reason="needs the engine module and the runtime")

SPEED = 40.0  # metres a second: round a 2 km planet in about five minutes of game time

WALK = """import json
import math

import midgard

CENTRE = {centre}  # the world's centre (its axis runs along z)
SPEED = {speed}
STEP = 40.0  # metres between the route's points
state = {{}}


def route(start):
    # All the way round: the circle through the start about the axis through the centre.
    v = [start[i] - CENTRE[i] for i in range(3)]
    n = int(2 * math.pi * math.hypot(v[0], v[1]) / STEP) + 1
    points = []
    for k in range(1, n + 1):
        t = 2 * math.pi * k / n
        points.append((CENTRE[0] + v[0] * math.cos(t) - v[1] * math.sin(t),
                       CENTRE[1] + v[0] * math.sin(t) + v[1] * math.cos(t), start[2]))
    return points


def on_start(entity):
    state.update(waited=0.0, started=False, done=False, samples=0, grounded=0, low=1e12,
                 high=-1e12, quarters=[], day=False, night=False, air=0.0, longest_air=0.0)


def on_update(entity, dt):
    s = state
    player = midgard.player()
    if s["done"] or player is None:
        return
    if not s["started"]:  # (a moment to land first)
        s["waited"] += dt
        if s["waited"] > 1.5 and player.on_ground:
            s["start"] = player.position
            player.speed = SPEED
            length = 2 * math.pi * math.hypot(s["start"][0] - CENTRE[0],
                                              s["start"][1] - CENTRE[1])
            midgard.time.set_scale(86400.0 * 1.25 / (length / SPEED))  # a day and a bit
            player.follow(route(s["start"]))
            s["started"] = True
        return
    p = player.position
    r = math.hypot(p[0] - CENTRE[0], p[1] - CENTRE[1])
    s["samples"] += 1
    held = player.on_ground or player.swimming  # (across a sea it swims)
    s["grounded"] += 1 if held else 0
    s["air"] = 0.0 if held else s["air"] + dt  # (off the ground: over a crest)
    s["longest_air"] = max(s["longest_air"], s["air"])
    s["low"], s["high"] = min(s["low"], r), max(s["high"], r)
    quarter = int(midgard.time.solar_phase() * 4) % 4
    if quarter not in s["quarters"]:
        s["quarters"].append(quarter)
    if midgard.time.is_day():
        s["day"] = True
    else:
        s["night"] = True
    if not player.following:
        s["done"] = True
        print("TOUR " + json.dumps({{
            "samples": s["samples"], "grounded": s["grounded"],
            "longest_air": s["longest_air"], "low": s["low"],
            "high": s["high"], "quarters": sorted(s["quarters"]), "day": s["day"],
            "night": s["night"], "start": list(s["start"]), "end": list(p),
            "radius": math.hypot(s["start"][0] - CENTRE[0], s["start"][1] - CENTRE[1])}}))
"""

FLY = """import midgard

said = {"up": False, "down": False}


def on_update(entity, dt):
    player = midgard.player()
    if player is not None and not player.flying and not said["up"] and player.on_ground:
        player.flying = True
        said["up"] = True
        print("launched")
    if said["up"] and not said["down"] and midgard.rooms.in_space:
        said["down"] = True
"""


@pytest.fixture(scope="module")
def tour(tmp_path_factory, qapp):
    """A World Tour with a walking script in each round world and a flight on the planet."""
    project = world_tour.make_world_tour(tmp_path_factory.mktemp("exit"))
    scripts = project.root / "scripts"
    for s in scenes.load_all(project):
        body = project.scene_settings(s["id"]).get("body", {})
        if body.get("shape") not in ("curved_cylinder", "sphere_outside", "sphere_inside"):
            continue
        module = f"exit_walk_{s['id']}"
        (scripts / f"{module}.py").write_text(
            WALK.format(centre=tuple(body["origin"]), speed=SPEED), encoding="utf-8")
        project.put_entity(Entity(project.next_entity_id(), "Exit Walk", None, {
            "transform": {}, "script": {"module": module}}), s["id"])
    return project


def scene_named(project, name: str) -> str:
    return next(s["id"] for s in scenes.load_all(project) if s["name"] == name)


def run_game(package, frames: int, *extra) -> tuple[dict, list[str]]:
    run = subprocess.run([str(about.runtime_candidates()[0]), "--package", str(package),
                          "--simulate", str(frames), "--dump-scene", *extra],
                         capture_output=True, text=True, timeout=1800)
    assert run.returncode == 0, run.stderr[-2000:]
    out = run.stdout
    dump = json.loads(out[out.rindex("\n{") + 1:]) if "\n{" in out else json.loads(out)
    return dump, out.splitlines()


@pytest.mark.parametrize("world", ["Habitat", "Planet", "Hollow World"])
def test_the_exit_all_the_way_round_with_gravity_through_a_day_and_a_night(tour, tmp_path,
                                                                           world):
    scene = scene_named(tour, world)
    body = tour.scene_settings(scene)["body"]
    radius = body["radius"]
    package = cook(tour, tmp_path / "pkg", entry_scene=scene).out_dir
    frames = int(2 * math.pi * radius / SPEED * 60 * 1.35) + 600
    dump, lines = run_game(package, frames)
    report = [json.loads(line[5:]) for line in lines if line.startswith("TOUR ")]
    assert report, f"the walk didn't finish in {frames} frames: {lines[-5:]}"
    r = report[0]
    # All the way round: back where it started (within a few steps).
    assert math.dist(r["start"], r["end"]) < 25.0, r
    # Held to the ground the whole way: at 40 m/s it leaves the ground over each crest, but
    # gravity brings it straight back down, and it is never far from the surface (the
    # ground's hills and seabed are within about 120 m of it on these worlds).
    assert r["grounded"] / r["samples"] > 0.5, r
    # (The longest hop: a crest at 40 m/s on a 2 km planet, whose ground curves away beneath
    # it, launches it for some seconds; it always lands again.)
    assert r["longest_air"] < 8.0, r
    assert r["low"] > radius - 130.0 and r["high"] < radius + 130.0, r
    # A full day and night.
    assert r["quarters"] == [0, 1, 2, 3] and r["day"] and r["night"], r
    assert dump["scene"] == scene


def test_the_exit_each_world_the_game_makes_is_the_one_the_editor_shows(tour, tmp_path):
    for s in scenes.load_all(tour):
        settings = tour.scene_settings(s["id"])
        if "terrain" not in settings:
            continue
        editor = engine.module().world_hash(json.dumps(world_types.resolved_settings(
            tour, settings)))
        package = cook(tour, tmp_path / s["id"], entry_scene=s["id"]).out_dir
        dump, _lines = run_game(package, 5)
        assert dump["world_hash"] == editor, s["name"]


def test_the_exit_up_out_of_the_planets_atmosphere_and_back_down(tour, tmp_path):
    planet = scene_named(tour, "Planet")
    (tour.root / "scripts" / "exit_fly.py").write_text(FLY, encoding="utf-8")
    walk = next(e for e in tour.entities(planet) if e.name == "Exit Walk")
    walk.components["script"] = {"module": "exit_fly"}  # (this time: a flight)
    tour.put_entity(walk, planet)
    package = cook(tour, tmp_path / "pkg", entry_scene=planet).out_dir
    # Up (Space held: the arrival is on a mountain top, where a jetpack starts slowly).
    up, lines = run_game(package, 1900, "--hold", "Space:30:1800")
    assert "launched" in lines
    assert up["scene"] == "@space" and up["rooms"]["space_from"] == planet
    # And back down (Left Ctrl held): onto the ground below, walking.
    down, lines = run_game(package, 2600, "--hold", "Space:30:1800",
                           "--hold", "Left Ctrl:1850:2600")
    assert down["scene"] == planet and down["rooms"]["previous"] == "@space"
    assert abs(down["rooms"]["altitude"]) < 140.0  # (on the ground: hills and all)
    walk.components["script"] = {"module": f"exit_walk_{planet}"}
    tour.put_entity(walk, planet)


def test_the_exit_the_character_in_all_three_views(tour, tmp_path):
    """V changes the view (third person, first, from above): the character is animated in each,
    its feet on the planet's ground."""
    package = cook(tour, tmp_path / "pkg", entry_scene=scene_named(tour, "Planet")).out_dir
    for view, presses in (("third", 0), ("first", 1), ("top", 2)):
        extra = []
        for i in range(presses):
            extra += ["--hold", f"V:{40 + 30 * i}:{42 + 30 * i}"]
        dump, _lines = run_game(package, 160, *extra)
        assert dump["camera"] == view
        player = next(e for e in dump["entities"] if e.get("name") == "Player")
        assert player["character"]["state"], view
        assert abs(dump["rooms"]["altitude"]) < 140.0  # (standing on the ground)
        # With the planet's own ambience (its wind) and the music.
        planet = scene_named(tour, "Planet")
        assert dump["audio"]["ambience"]["set"] == f"amb_{planet}"
        assert dump["audio"]["music"]["playlist"] == "mus_tour"


def test_the_exit_the_tour_ships_as_a_game(tour, tmp_path):
    """The World Tour exported (from a runtime template made from this build) runs on its own."""
    import sys
    from pathlib import Path

    from midgard_editor import exporter
    root = Path(__file__).resolve().parents[2]
    preset = "windows-release" if sys.platform == "win32" else "linux-release"
    target = "windows-x86_64" if sys.platform == "win32" else "linux-x86_64"
    exe = "midgard_runtime.exe" if sys.platform == "win32" else "midgard_runtime"
    if not (root / "build" / preset / "engine" / exe).is_file():
        pytest.skip(f"no {preset} build of the engine here")
    sys.path.insert(0, str(root / "tools"))
    try:
        from make_template import make_template
    finally:
        sys.path.remove(str(root / "tools"))
    settings = tmp_path / "settings"
    exporter.install_template(make_template(preset, tmp_path / "templates"), settings)
    archive = exporter.export(tour, target, tmp_path / "out", settings)
    folder = tmp_path / "out" / f"{tour.name}-{target}" / tour.name
    game = folder / (f"{tour.name}.exe" if sys.platform == "win32" else tour.name)
    assert archive.is_file() and game.is_file()
    run = subprocess.run([str(game), "--simulate", "120", "--dump-scene"],
                         capture_output=True, text=True, timeout=300, cwd=folder)
    assert run.returncode == 0, run.stderr[-2000:]
    dump = json.loads(run.stdout[run.stdout.rindex("\n{") + 1:])
    assert dump["scene"] == "scn_main"
    assert dump["audio"]["music"]["playlist"] == "mus_tour"
    assert (folder / "CREDITS.txt").is_file()  # (the CC0 samples, thanked)
