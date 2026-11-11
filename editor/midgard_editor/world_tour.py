# SPDX-License-Identifier: Apache-2.0
"""The World Tour sample project (milestone 5.18): a hub with a door to each of four worlds,
one of each shape, each the engine's own sample world type at its own size and seed
(midgard/world_type.h), so the ground is the one the engine's tests know:

    Hub          a paved square; four doors (walk in) and a pad home in each world
    Archipelago  a bounded area 3 km square: islands in a sea, temperate (Archipelago)
    Habitat      inside a cylinder 1 km in radius and 5 km long: farmland, a dock out to space
    Planet       a planet 2 km in radius: climate bands, snow at the poles, a launch pad (a
                 jetpack) up through the atmosphere's ceiling into space and back
    Hollow World inside a sphere 2.5 km in radius: the far side overhead, an exit site to space

Each world has the Earth's day and night (its sun follows the clock, by its own shape's light
model), seasons from the standard calendar, its own weather and its own ambience; music plays
throughout. You walk as Midgard's sample character (Quaternius' Superhero, animated: idle, walk,
jog, sprint and jump; feet on slopes; first, third and top views with V), with footsteps on each
kind of ground; another strolls round the hub. The sounds are Midgard's own CC0 samples (Nox
Sound's footsteps and ambiences, yd's Desert Theme), put in the project's Asset Library.

Where you arrive in a world, the ground is flattened round you at its own height there (kept
above the sea), with the pad home behind you.

Milestone 6.0's exit: it opens at its title screen (New game, Settings, Quit) and has the
ready-made pause menu, settings (volume, music, the Jump and Use keys) and game over screens,
with the HUD's health and stamina bars, score and compass. Five coins in the hub raise the
score with a note each; the hub's fire pit burns health away, and at none the game over screen
offers Try again (back where you arrived, refilled).

    python -m midgard_editor.world_tour <folder>     makes <folder>/WorldTour
"""
import json
import sys
from pathlib import Path

from . import (
    ambience,
    asset_library,
    engine,
    environments,
    menus,
    music,
    scenes,
    sound_events,
    sounds,
    weather,
    world_types,
)
from .physics_dialogs import origin
from .project import MAIN_SCENE, Project
from .scene_model import Entity

TOUR = "WorldTour"
TITLE = "World Tour"
# The worlds: (scene name, the engine's sample world type, door colour).
WORLDS = (
    ("Archipelago", "wt_sample_archipelago", (0.3, 0.65, 0.85)),
    ("Habitat", "wt_sample_habitat", (0.8, 0.7, 0.3)),
    ("Planet", "wt_sample_temperate_planet", (0.35, 0.7, 0.35)),
    ("Hollow World", "wt_sample_hollow", (0.65, 0.4, 0.8)),
)
ABOVE_SEA = 3.0  # metres: the least height an arrival is flattened to (out of the water)
WEATHER = {
    "Archipelago": {"cell": 1000,  # (3 cells: the middle one at the origin)
                    "tables": [{"name": "Sea", "biomes": [], "seasons": [],
                                "weather": {"clear": 3, "cloudy": 3, "rain": 2, "fog": 1}},
                               {"name": "Winter", "biomes": [], "seasons": [0],
                                "weather": {"snow": 2, "cloudy": 2, "clear": 1}}],
                    "lasts": {"default": [1, 4]},
                    "fronts": {"every": [8, 20], "speed": [6, 12], "radius": [2, 3],
                               "lasts": [3, 6], "weather": {"thunderstorm": 1}}},
    "Habitat": {"controlled": [{"weather": "clear", "hours": 3}, {"weather": "rain", "hours": 1},
                               {"weather": "fog", "hours": 1}]},
    "Planet": {"cell": 340,
               "tables": [{"name": "Temperate", "biomes": [], "seasons": [],
                           "weather": {"clear": 3, "cloudy": 2, "rain": 2}}],
               "lasts": {"default": [0.5, 2]}},
    "Hollow World": {"cell": 500,
                     "tables": [{"name": "Inside", "biomes": [], "seasons": [],
                                 "weather": {"cloudy": 2, "fog": 2, "clear": 1}}],
                     "lasts": {"default": [1, 3]}},
}
PLANET_CEILING = 200.0  # metres up: the atmosphere's ceiling, and space beyond it

# The character (Midgard's samples, in the project's library once added).
HERO = "lib:models/superhero/superhero_male.glb"
HERO_SKIN = "tex:library/models/superhero/superhero_male_basecolor.png"
MOVES = "lib:models/universal_animation_library/animation_library.glb"
# The player's animation graph: idle, walk, jog and sprint blended by speed, and jumping.
LOCOMOTION = {
    "parameters": {"grounded": True}, "triggers": [], "root_motion": False,
    "layers": [{
        "name": "Base", "start": "Locomotion", "any_pos": [-220, -120],
        "states": {
            "Locomotion": {"blend": "speed", "loop": True, "pos": [0, 0],
                           "clips": [["Idle_Loop", 0], ["Walk_Loop", 1.5],
                                     ["Jog_Fwd_Loop", 4], ["Sprint_Loop", 8]]},
            "JumpStart": {"clip": "Jump_Start", "loop": False, "pos": [260, -120]},
            "Fall": {"clip": "Jump_Loop", "loop": True, "pos": [520, 0]},
            "Land": {"clip": "Jump_Land", "loop": False, "pos": [260, 120]}},
        "transitions": [
            {"from": "Locomotion", "to": "JumpStart",
             "when": [["grounded", "<", 0.5], ["vertical_speed", ">", 1]], "blend": 0.1},
            {"from": "Locomotion", "to": "Fall",
             "when": [["grounded", "<", 0.5], ["vertical_speed", "<", -1]], "blend": 0.2},
            {"from": "JumpStart", "to": "Fall", "after": 1, "blend": 0.15},
            {"from": "JumpStart", "to": "Land", "when": [["grounded", ">", 0.5]], "blend": 0.1},
            {"from": "Fall", "to": "Land", "when": [["grounded", ">", 0.5]], "blend": 0.1},
            {"from": "Land", "to": "Locomotion", "when": [["speed", ">", 0.5]], "blend": 0.15},
            {"from": "Land", "to": "Locomotion", "after": 1, "blend": 0.2}]}]}
# The walker's: idle and walk by speed.
STROLL = {"parameters": {}, "triggers": [], "root_motion": False,
          "layers": [{"name": "Base", "start": "Move", "any_pos": [-220, -120],
                      "states": {"Move": {"blend": "speed", "loop": True, "pos": [0, 0],
                                          "clips": [["Idle_Loop", 0], ["Walk_Loop", 1.4]]}},
                      "transitions": []}]}
# Each world's ambience: (sample sound, times of day, volume) layers.
AMBIENCE = {
    "Hub": [("Meadow birds", ["dawn", "day"], 0.45), ("Night", ["dusk", "night"], 0.5)],
    "Archipelago": [("Sea", ["dawn", "day", "dusk", "night"], 0.5),
                    ("Forest birds", ["dawn", "day"], 0.3), ("Night", ["night"], 0.4)],
    "Habitat": [("Sci-fi bunker", ["dawn", "day", "dusk", "night"], 0.3),
                ("Meadow birds", ["dawn", "day"], 0.3)],
    "Planet": [("Calm wind", ["dawn", "day", "dusk", "night"], 0.5),
               ("Night", ["night"], 0.35)],
    "Hollow World": [("Eerie drone", ["dawn", "day", "dusk", "night"], 0.35),
                     ("Calm wind", ["dawn", "day", "dusk", "night"], 0.3)],
}

CHECKLIST = '''# The World Tour: things to try

A hub and four worlds, one of each shape Midgard makes, each its own size, with its own day and
night, seasons, weather and sounds. Tick each box as you try it.

**Controls:** W A S D walk · mouse looks · hold Left Shift to run · Space jumps · V changes the
view (behind, through her eyes, from above) · mouse wheel zooms · E uses a pad · F3 shows the
information overlay · Escape ends the game. **Flying** (the Planet's launch pad): Space climbs,
Left Ctrl sinks, W A S D go where you look, Shift goes faster.

## In the editor

- [ ] The **Scenes** panel lists five scenes: Main (the hub), Archipelago, Habitat, Planet and
      Hollow World. Double-click one to open it in the Viewport.
- [ ] **World ▸ World Shape…** in each world shows its shape and size: an area 3 km square, a
      cylinder 1 km in radius and 5 km long, a planet 2 km in radius (its atmosphere 200 m
      up), a hollow world 2.5 km in radius.
- [ ] The **World Types** panel shows each world's recipe (the engine's own samples).
- [ ] **Assets ▸ Asset Library**: the character, its animations, the footsteps, ambience loops
      and the tune, all free (CC0) with their credits.
- [ ] The **Audio** panel: the footsteps on each kind of ground, each world's ambience by time
      of day, and the music.
- [ ] **Calendar** panel: drag the scrubber; the sun moves through the day.

## Playing (press F5)

- [ ] It opens at its **title screen**: New game, Settings, Quit. **Settings**: volume, music, and
      the Jump and Use keys (click one, press a key). **New game** starts.
- [ ] **Coins:** five gold coins round the hub. Walk through one: **Score** goes up 10 and a note
      says so.
- [ ] **The fire pit** (the red glow beside the hub): stand in it and **Health** burns away; at
      none, **You died**: **Try again** puts you back where you arrived, health full.
- [ ] **Esc** (or a controller's Start) opens the **pause menu**; Esc again goes back.
- [ ] **The hub:** a paved square, four coloured doorways, birdsong by day; another walker
      strolls round. Walk into a doorway to go to that world.
- [ ] Wherever you arrive, the **glowing pad** behind you takes you back to the hub (stand on
      it, press E). The first time you arrive, the sky is clear for an hour; then the world's
      own weather comes in.
- [ ] **Archipelago** (blue door): islands in a sea, and the sound of the sea. Walk to the
      shore: sand underfoot sounds different from grass.
- [ ] **Habitat** (yellow door): inside a spinning cylinder. Look up: the land curves up both
      sides and meets overhead. Walk across it and all the way round: you come back where you
      started. The **Dock** (the pale pad: stand on it, press E) takes you outside into space,
      the habitat's hull beside you; hold Left Ctrl to go back down into it, by the Dock.
- [ ] **Planet** (green door): you arrive on a snowy mountain near the pole. Walk away down the
      slopes into warmer land. The orange **Launch Pad** is a jetpack: hold Space (and Shift)
      to climb out through the atmosphere (200 m) into space, the whole planet below; hold
      Left Ctrl to come back down, landing on the ground below you.
- [ ] **Hollow World** (purple door): inside a sphere, the far side overhead through the haze,
      an eerie hum. The **Exit Site** takes you outside.
- [ ] Press **V** in each world: behind her, through her eyes (her arms), from above. Her feet
      stay on the slopes.
- [ ] Wait (or drag the Calendar's scrubber while playing): the day turns to night, with the
      night's sounds, on every world.

## On your phone

1. Get the **Android runtime template** that matches your Midgard (from the Midgard team, or
   from the build server's runtime-template-android-arm64 download).
2. **Build ▸ Export…**, choose **Android**, **Install Template…** and pick it.
3. Export the World Tour, copy the .apk to your phone and install it.
- [ ] It opens at the title screen: tap **New game**. Then the on-screen touch controls: the
      stick bottom left, drag on the right half to look, Jump, Use, Run, View and **Menu**
      bottom right (or connect a game controller: the controls go away until you touch the
      screen).
- [ ] Pick up a coin, burn in the fire pit and **Try again**, all by touch. **Menu** (or the
      phone's Back button) opens the pause menu; in **Settings** drag the volume.
- [ ] Visit each world; it runs smoothly, with sound.
'''

WELCOME = '''"""A fair welcome: the first time you arrive in a world, its sky is clear for an hour
(of game time); then its own weather comes back (midgard.weather.force)."""
import midgard

welcomed = set()


def welcome():
    scene = midgard.rooms.current
    if scene not in welcomed:
        welcomed.add(scene)
        midgard.weather.force("clear", hours=1.0)


def on_start(entity):  # (the game starting here)
    welcome()


def on_room_enter(entity, came_from):  # (arriving through a door or a pad)
    welcome()
'''

WALKER = '''"""The Walker: strolls round the hub, finding the way on the navigation mesh
(midgard.nav.path), pausing at each corner. Its walk animation follows how fast it moves."""
import math

import midgard

CORNERS = [(-7.0, 0.0, -2.0), (7.0, 0.0, -2.0), (7.0, 0.0, 8.0), (-7.0, 0.0, 8.0)]
SPEED = 1.4  # metres a second
walkers = {}


def on_start(entity):
    walkers[entity.id] = {"corner": 0, "path": [], "wait": 1.0}


def on_update(entity, dt):
    s = walkers.setdefault(entity.id, {"corner": 0, "path": [], "wait": 1.0})
    if s["wait"] > 0.0:
        s["wait"] -= dt
        return
    if not s["path"]:
        target = CORNERS[s["corner"] % len(CORNERS)]
        s["corner"] += 1
        path = midgard.nav.path(entity.position, target, partial=True)
        if not path:
            s["wait"] = 1.0  # (the navigation mesh is still being made)
            return
        s["path"] = list(path[1:]) or [target]
    x, y, z = entity.position
    tx, ty, tz = s["path"][0]
    dx, dz = tx - x, tz - z
    d = math.hypot(dx, dz)
    step = SPEED * dt
    if d <= step:
        entity.position = (tx, ty, tz)
        s["path"].pop(0)
        if not s["path"]:
            s["wait"] = 2.0  # a pause at each corner
        return
    entity.position = (x + dx / d * step, ty, z + dz / d * step)
    # Facing the way it walks (characters face -Z at no turn).
    entity.rotation = (0.0, math.degrees(math.atan2(-dx, -dz)), 0.0)
'''

COIN = '''"""A coin: walk through it for 10 points (the HUD's Score) and a note. It goes (far
below the ground) once picked up."""
import midgard


def on_trigger_enter(entity, other):
    midgard.stats.add("score", 10)
    midgard.ui.notify("Picked up a coin (+10)")
    x, y, z = entity.position
    entity.position = (x, y - 1000.0, z)
'''

FIRE = '''"""The fire pit: standing in it burns Health away (the game over screen shows at none)."""
import midgard

burning = {"on": False}


def on_trigger_enter(entity, other):
    burning["on"] = True
    midgard.ui.notify("Too hot!")


def on_trigger_exit(entity, other):
    burning["on"] = False


def on_update(entity, dt):
    if burning["on"]:
        midgard.stats.add("health", -40.0 * dt)
'''

LAUNCH_PAD = '''"""The Planet's launch pad: a jetpack. Step on it and you fly.

    Space      climb (hold Shift too, to go faster)
    Left Ctrl  sink
    W A S D    go where you look

Climb past the atmosphere's ceiling and you're in space: the planet below, the stars above.
Sink back down through it and you land on the ground below where you came down, walking again.
"""
import midgard


def on_trigger_enter(entity, other):
    player = midgard.player()
    if player is None or player.flying:
        return
    player.flying = True
    print(f"Jetpack on! Hold Space to climb. Space begins {midgard.rooms.ceiling:.0f} m up; "
          "Left Ctrl sinks, W A S D go where you look.")
'''


class _Builder:
    def __init__(self, project: Project):
        self.p = project
        self.samples = {s["id"]: s for s in engine.module().sample_world_types()}

    # ---- helpers ----------------------------------------------------------------------------
    def put(self, scene: str, name: str, parent: str | None, components: dict) -> str:
        entity_id = self.p.next_entity_id()
        self.p.put_entity(Entity(entity_id, name, parent, components), scene)
        return entity_id

    def world_folder(self, scene: str) -> str:
        return next(e.id for e in self.p.entities(scene) if e.is_folder)

    def named(self, scene: str, name: str) -> Entity:
        return next(e for e in self.p.entities(scene) if e.name == name)

    def box(self, scene, parent, name, at, size, color, mesh="prim_cube", solid=True,
            glow=0.0) -> str:
        c = {"transform": {"position": list(at), "scale": list(size)},
             "mesh": {"asset": mesh}, "material": {"color": [*color, 1.0]}}
        if glow:
            c["material"].update({"emissive": list(color), "emissive_strength": glow})
        if solid:
            c["collider"] = {"shape": "box"}
        return self.put(scene, name, parent, c)

    def arrival(self, scene, parent, name, at, yaw=180.0) -> str:
        return self.put(scene, name, parent, {
            "transform": {"position": list(at), "rotation": [0.0, yaw, 0.0]},
            "arrival": {"name": name}})

    def door(self, scene, parent, name, at, leads_to, arrival, color) -> str:
        x, y, z = at
        for part, position, scale in (("Post", [x - 1.1, y + 1.25, z], [0.3, 2.5, 0.3]),
                                      ("Post", [x + 1.1, y + 1.25, z], [0.3, 2.5, 0.3]),
                                      ("Lintel", [x, y + 2.6, z], [2.5, 0.3, 0.3])):
            self.box(scene, parent, f"{name} {part}", position, scale, color, glow=0.3)
        return self.put(scene, name, parent, {
            "transform": {"position": [x, y + 1.25, z]},
            "scene_exit": {"scene": leads_to, "arrival": arrival, "activation": "enter",
                           "size": [1.9, 2.5, 0.6], "fade": 0.6}})

    def pad(self, scene, parent, name, at, leads_to, arrival, color) -> str:
        """A glowing pad: stand on it and press Use (E)."""
        x, y, z = at
        # (A thick disc, its top just above the ground and the rest in it: flush with the
        # ground even where it's a little lower than the clearing's height.)
        self.put(scene, f"{name} Disc", parent, {
            "transform": {"position": [x, y - 0.95, z], "scale": [2.0, 2.0, 2.0]},
            "mesh": {"asset": "prim_cylinder"},
            "material": {"color": [*color, 1.0], "emissive": [c * 0.8 for c in color],
                         "emissive_strength": 2.0}})
        self.put(scene, f"{name} Glow", parent, {
            "transform": {"position": [x, y + 1.0, z]},
            "light": {"type": "point", "color": list(color), "intensity": 2.0, "range": 6.0}})
        return self.put(scene, name, parent, {
            "transform": {"position": [x, y + 1.0, z]},
            "scene_exit": {"scene": leads_to, "arrival": arrival, "activation": "use",
                           "size": [2.2, 4.0, 2.2], "fade": 0.8}})

    def lamp(self, scene, parent, name, at, color=(1.0, 0.85, 0.6)) -> None:
        x, y, z = at
        self.box(scene, parent, f"{name} Post", [x, y + 1.2, z], [0.15, 2.4, 0.15],
                 (0.25, 0.25, 0.27))
        self.box(scene, parent, f"{name} Light", [x, y + 2.5, z], [0.3, 0.3, 0.3], color,
                 solid=False, glow=3.0)
        self.put(scene, name, parent, {
            "transform": {"position": [x, y + 2.5, z]},
            "light": {"type": "point", "color": list(color), "intensity": 2.5, "range": 9.0}})

    def daylight(self, scene: str, environment: str = "env_earth") -> None:
        """The sun follows the clock; the sky and light come from an environment."""
        for e in self.p.entities(scene):
            if e.name == "Sun":
                e.components["light"]["follows_clock"] = True
                self.p.put_entity(e, scene)
            elif "sky" in e.components:
                e.components["sky"] = {**e.components["sky"], "environment": environment}
                self.p.put_entity(e, scene)

    # ---- the worlds -------------------------------------------------------------------------
    def ground_height(self, scene: str) -> float:
        """The ground's own height at the scene's origin (as the editor's view makes it)."""
        view = engine.module().Viewport()
        view.set_settings(json.dumps(world_types.resolved_settings(
            self.p, self.p.scene_settings(scene))))
        found = view.terrain_ground([0.0, 0.0, 0.0])
        return 0.0 if found is None else -float(found[0])

    def world(self, scene: str, sample_id: str, ceiling: float | None = None) -> float:
        """A scene of one of the engine's sample worlds: its world type (as it is), shape, size
        and seed; the ground flattened round the origin at its own height there (above the
        sea); its weather. Returns that height."""
        sample = self.samples[sample_id]
        recipe = json.loads(sample["recipe"])
        world_types.SaveRecipeCommand(self.p, sample_id, recipe, sample["name"]).redo()
        body = json.loads(sample["body"])
        seed = int(body.pop("seed"))
        body = {**body, "id": "world", "gravity": 9.81}
        body["origin"] = origin(body["shape"], body)
        ground = self.named(scene, "Ground")
        self.p.delete_entity(ground.id)  # (the terrain is the ground)
        settings = self.p.scene_settings(scene)
        settings["body"] = body
        settings["terrain"] = {"world_type": sample_id, "version": 1, "seed": seed}
        settings["weather"] = WEATHER[self.name_of(scene)]
        if ceiling is not None:
            settings["ceiling"] = ceiling
        self.p.set_scene_settings(settings, scene)
        height = round(max(self.ground_height(scene), ABOVE_SEA), 1)
        face, u, v, _h = engine.module().Body(json.dumps(body)).surface(0.0, 0.0, 0.0)
        stamp = {"tool": "flatten", "radius": 40.0, "strength": 1.0, "height": height,
                 "face": face, "points": [[u, v]]}
        with self.p.connect() as con:
            con.execute("INSERT INTO terrain_stamp (scene, stamp) VALUES (?, ?)",
                        (scene, json.dumps(stamp)))
        player = self.named(scene, "Player")
        player.components["transform"]["position"] = [0.0, height + 1.0, 3.0]
        self.p.put_entity(player, scene)
        return height

    def name_of(self, scene: str) -> str:
        return self.scene_names[scene]

    # ---- life: sounds, the player, the walker -----------------------------------------------
    def sounds(self) -> None:
        """Midgard's CC0 sample sounds into the project's library, and the sound events, music
        and weather sounds made from them."""
        added, _left = asset_library.add_samples(self.p)
        self.library = {a.name: a for a in added}

        def event(asset: str, name: str, **settings) -> str:
            files = sorted(p for p in self.library[asset].folder.iterdir()
                           if p.suffix == ".ogg")
            e = sound_events.event_for_sound(self.p, files[0], name)
            if len(files) > 1:  # (one of them each time)
                e["event"]["sounds"] = [
                    {"sound": sounds.ref(f.relative_to(self.p.root).as_posix()), "weight": 1}
                    for f in files]
            e["event"].update(settings)
            sound_events.put(self.p, e)
            return e["id"]

        steps = {surface: event(f"Footsteps on {surface}", f"Steps on {surface}",
                                volume=[0.45, 0.65], pitch=[0.95, 1.05])
                 for surface in ("grass", "rock", "sand", "snow", "metal")}
        self.footsteps = {"every": 1.6, "sounds": {"default": steps["grass"], **steps,
                                                   "stone": steps["rock"]}}
        self.loops = {name: event(name, name) for name in
                      ("Meadow birds", "Forest birds", "Night", "Sea", "Calm wind",
                       "Calm rain", "Sci-fi bunker", "Eerie drone")}
        tune = event("Desert Theme", "Desert Theme")
        music.put(self.p, {"id": "mus_tour", "name": "World Tour", "playlist": {
            "tracks": [tune], "scenes": [], "crossfade": 3}})
        for type_id in ("rain", "thunderstorm"):  # rain you can hear
            found = weather.load(self.p, type_id)
            if found is not None:
                found["weather"]["sounds"] = [{"event": self.loops["Calm rain"], "volume": 0.8}]
                weather.put(self.p, found)

    def ambience(self, scene: str, name: str) -> None:
        layers = [{"event": self.loops[sound], "times": times, "volume": volume}
                  for sound, times, volume in AMBIENCE[name]]
        ambience.put(self.p, {"id": f"amb_{scene}", "name": name, "ambience": {
            "layers": layers, "scenes": [scene], "crossfade": 4}})

    def player(self, scene: str, at: list) -> None:
        """The player as the animated character, the camera behind her in third person."""
        player = self.named(scene, "Player")
        player.components = {
            "transform": {"position": list(at), "rotation": [0.0, 0.0, 0.0]},
            "material": {"color": [1.0, 1.0, 1.0, 1.0], "textures": {"base_color": HERO_SKIN}},
            "player": {"speed": 4.0},
            "character": {"model": HERO, "clips": MOVES, "clip": "Idle_Loop", "speed": 1.0,
                          "graph": LOCOMOTION, "ik": {"feet": True}, "first_person": {}},
            "footsteps": dict(self.footsteps)}
        self.p.put_entity(player, scene)
        camera = self.named(scene, "Camera")
        camera.components["camera"] = {"fov": 55, "near": 0.05, "far": 8000, "view": "third",
                                       "views": {"third": {"distance": 5.0, "height": 0.6}}}
        camera.components["transform"] = {"position": [at[0], at[1] + 2.5, at[2] + 5.0],
                                          "rotation": [-15.0, 0.0, 0.0]}
        self.p.put_entity(camera, scene)

    def welcome(self, scene: str, parent: str) -> None:
        (self.p.root / "scripts").mkdir(exist_ok=True)
        (self.p.root / "scripts" / "fair_welcome.py").write_text(WELCOME, encoding="utf-8")
        self.put(scene, "Fair Welcome", parent, {"transform": {},
                                                 "script": {"module": "fair_welcome"}})

    def walker(self, scene: str, parent: str) -> None:
        (self.p.root / "scripts").mkdir(exist_ok=True)
        (self.p.root / "scripts" / "walker.py").write_text(WALKER, encoding="utf-8")
        self.put(scene, "Walker", parent, {
            "transform": {"position": [-7.0, 0.0, -2.0]},
            "material": {"color": [0.55, 0.7, 1.0, 1.0], "textures": {"base_color": HERO_SKIN}},
            "character": {"model": HERO, "clips": MOVES, "clip": "Idle_Loop", "speed": 1.0,
                          "graph": STROLL, "ik": {"feet": True}},
            "footsteps": dict(self.footsteps),
            "script": {"module": "walker"}})

    def build(self) -> Project:
        p = self.p
        self.scene_ids: dict[str, str] = {}
        for name, *_rest in WORLDS:
            cmd = scenes.AddSceneCommand(p, name)
            p.undo_stack.push(cmd)
            self.scene_ids[name] = cmd.scene
        p.undo_stack.clear()  # (a new project: nothing to undo)
        self.scene_names = {v: k for k, v in self.scene_ids.items()}
        self.sounds()
        # The calendar: the standard one, starting on a spring morning (seasons come round).
        with p.connect() as con:
            con.execute("UPDATE calendar SET start_month = 4, start_day = 20, "
                        "start_minute = 540 WHERE standard = 1")
        # Inside worlds: the Inside-World Atmosphere (the far side through haze).
        for env_id, name, radius in (("env_inside_habitat", "Inside the Habitat", 1000.0),
                                     ("env_inside_hollow", "Inside the Hollow World", 2500.0)):
            environments.put(p, {"id": env_id, "name": name,
                                 **environments.inside_atmosphere(radius)})

        # The hub: a paved square with a door to each world, and where each comes back.
        hub = self.world_folder(MAIN_SCENE)
        ground = self.named(MAIN_SCENE, "Ground")
        ground.components["transform"]["scale"] = [40.0, 0.2, 40.0]
        ground.components["material"] = {"color": [0.55, 0.53, 0.5, 1.0], "roughness": 0.9}
        p.put_entity(ground, MAIN_SCENE)
        self.daylight(MAIN_SCENE)
        ground.components["physics_material"] = {"friction": 0.6, "surface": "stone"}
        p.put_entity(ground, MAIN_SCENE)
        self.player(MAIN_SCENE, [0.0, 0.9, 6.0])
        self.walker(MAIN_SCENE, hub)
        self.ambience(MAIN_SCENE, "Hub")
        for i, (name, sample_id, color) in enumerate(WORLDS):
            scene = self.scene_ids[name]
            x = -9.0 + 6.0 * i
            world = self.world_folder(scene)
            height = self.world(scene, sample_id,
                                ceiling=PLANET_CEILING if name == "Planet" else None)
            back = self.arrival(MAIN_SCENE, hub, f"From the {name}", [x, 0.5, -9.0])
            there = self.arrival(scene, world, "From the Hub", [0.0, height + 1.0, 3.0])
            self.door(MAIN_SCENE, hub, f"{name} Door", [x, 0.0, -12.0], scene, there, color)
            self.lamp(MAIN_SCENE, hub, f"{name} Lamp", [x + 1.9, 0.0, -11.3])
            inside = {"Habitat": "env_inside_habitat", "Hollow World": "env_inside_hollow"}
            self.daylight(scene, inside.get(name, "env_earth"))
            self.player(scene, [0.0, height + 1.0, 3.0])
            self.ambience(scene, name)
            if "controlled" not in WEATHER[name]:  # (the habitat's is set by its keepers)
                self.welcome(scene, world)
            self.pad(scene, world, "Pad Home", [0.0, height, -3.0], MAIN_SCENE, back, color)
            self.lamp(scene, world, "Pad Lamp", [2.5, height, -3.0])
            if name in ("Habitat", "Hollow World"):  # a way out into space
                exit_name = "Dock" if name == "Habitat" else "Exit Site"
                back_in = self.arrival(scene, world, f"Back in by the {exit_name}",
                                       [8.0, height + 1.0, 3.0])
                self.pad(scene, world, exit_name, [8.0, height, -3.0], "@space", back_in,
                         (0.95, 0.9, 0.5))
            if name == "Planet":  # a jetpack up through the atmosphere into space
                self.pad_launch(scene, world, height)
        self.coins_and_fire(hub)
        self.screens()
        self.checklist()
        return p

    def coins_and_fire(self, hub: str) -> None:
        """Five coins round the hub (a score a script raises, a pick-up note) and a fire pit
        (health that runs out: game over, and respawning)."""
        scripts = self.p.root / "scripts"
        scripts.mkdir(exist_ok=True)
        (scripts / "coin.py").write_text(COIN, encoding="utf-8")
        (scripts / "fire_pit.py").write_text(FIRE, encoding="utf-8")
        for i, (x, z) in enumerate(((-6.0, 2.0), (-3.0, 9.0), (3.0, 9.0), (6.0, 2.0),
                                    (0.0, -4.0))):
            coin = self.put(MAIN_SCENE, f"Coin {i + 1}", hub, {
                "transform": {"position": [x, 1.0, z]},
                "trigger": {"size": [1.2, 2.0, 1.2]}, "script": {"module": "coin"}})
            self.put(MAIN_SCENE, f"Coin {i + 1} Disc", coin, {
                "transform": {"rotation": [90.0, 0.0, 0.0], "scale": [0.6, 0.12, 0.6]},
                "mesh": {"asset": "prim_cylinder"},
                "material": {"color": [1.0, 0.8, 0.2, 1.0], "metallic": 1.0, "roughness": 0.3,
                             "emissive": [1.0, 0.7, 0.1], "emissive_strength": 0.6}})
        self.box(MAIN_SCENE, hub, "Fire Pit Glow", [12.0, 0.12, 6.0], [3.0, 0.1, 3.0],
                 (1.0, 0.25, 0.05), solid=False, glow=4.0)
        self.put(MAIN_SCENE, "Fire Pit", hub, {
            "transform": {"position": [12.0, 1.0, 6.0]},
            "trigger": {"size": [3.0, 2.0, 3.0]}, "script": {"module": "fire_pit"}})

    def screens(self) -> None:
        """The ready-made splash, title, pause, settings and game over screens; the splash and
        the title say the tour's name, and the title starts with New game."""
        for kind in ("splash", "title", "pause", "game_over"):
            menus.save_all(self.p, menus.from_template(self.p, kind))
        screens = menus.load_all(self.p)
        for screen in screens:
            if screen["screen"].get("role") == "splash":
                for e in screen["screen"]["elements"]:
                    if e["id"] == "title":
                        e["text"] = TITLE
            if screen["screen"].get("role") != "title":
                continue
            for e in screen["screen"]["elements"]:
                if e["id"] == "title":
                    e["text"] = TITLE
                if e["id"] == "play":
                    e["text"] = "New game"
        menus.save_all(self.p, screens)

    def checklist(self) -> None:
        """CHECKLIST.md in the project: what to try, in the editor, playing and on a phone."""
        (self.p.root / "CHECKLIST.md").write_text(CHECKLIST, encoding="utf-8")

    def pad_launch(self, scene: str, world: str, height: float) -> None:
        (self.p.root / "scripts").mkdir(exist_ok=True)
        (self.p.root / "scripts" / "launch_pad.py").write_text(LAUNCH_PAD, encoding="utf-8")
        self.put(scene, "Launch Pad Disc", world, {
            "transform": {"position": [-4.5, height - 0.95, -3.0], "scale": [2.6, 2.0, 2.6]},
            "mesh": {"asset": "prim_cylinder"},
            "material": {"color": [1.0, 0.45, 0.15, 1.0], "emissive": [1.0, 0.4, 0.1],
                         "emissive_strength": 2.5}})
        self.put(scene, "Launch Pad", world, {
            "transform": {"position": [-4.5, height + 1.0, -3.0]},
            "trigger": {"size": [2.6, 4.0, 2.6]},
            "script": {"module": "launch_pad"}})


def make_world_tour(parent_dir: Path, name: str = TOUR) -> Project:
    """The World Tour sample project in parent_dir (see the module's docstring)."""
    if engine.module() is None:
        raise RuntimeError("the World Tour needs the engine module (its sample world types)")
    project = Project.create(Path(parent_dir), name, "system_map", TITLE)
    return _Builder(project).build()


if __name__ == "__main__":  # pragma: no cover
    if len(sys.argv) != 2:
        sys.exit("usage: python -m midgard_editor.world_tour <folder>")
    made = make_world_tour(Path(sys.argv[1]))
    print(f"Made {made.config_path}")
