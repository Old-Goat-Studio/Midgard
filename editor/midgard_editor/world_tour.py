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
model), seasons from the standard calendar and its own weather. Where you arrive in a world, the
ground is flattened round you at its own height there (kept above the sea), with the pad home
behind you.

    python -m midgard_editor.world_tour <folder>     makes <folder>/WorldTour
"""
import json
import sys
from pathlib import Path

from . import engine, environments, scenes, world_types
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

    def build(self) -> Project:
        p = self.p
        self.scene_ids: dict[str, str] = {}
        for name, *_rest in WORLDS:
            cmd = scenes.AddSceneCommand(p, name)
            p.undo_stack.push(cmd)
            self.scene_ids[name] = cmd.scene
        p.undo_stack.clear()  # (a new project: nothing to undo)
        self.scene_names = {v: k for k, v in self.scene_ids.items()}
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
        player = self.named(MAIN_SCENE, "Player")
        player.components["transform"]["position"] = [0.0, 0.5, 6.0]
        p.put_entity(player, MAIN_SCENE)
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
        return p

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
