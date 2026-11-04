# SPDX-License-Identifier: Apache-2.0
"""Sample projects. The Scene Tour (milestone 5.11a) shows a game moving between scenes:

    Main    a door ahead (walk into it) to the Cave, and a glowing teleport pad to the right
            (stand on it and press Use: E, or a gamepad's X) to the Island
    Cave    dark; a way out behind where you arrive, back to the Main by its door
    Island  bright; a pad beside where you arrive, back to the Main by its pad

Every exit arrives at an arrival point a few steps clear of the way back, facing away from it.

    python -m midgard_editor.samples <folder>     makes <folder>/SceneTour
"""
import sys
from pathlib import Path

from . import scenes
from .project import MAIN_SCENE, Project
from .scene_model import Entity

TOUR = "SceneTour"


def _put(project: Project, scene: str, name: str, parent: str | None, components: dict) -> str:
    entity_id = project.next_entity_id()
    project.put_entity(Entity(entity_id, name, parent, components), scene)
    return entity_id


def _dress(project: Project, scene: str, sky: list, ambient: list, ground: list) -> str:
    """The scene's sky and a larger ground in its colours; returns its World folder."""
    world = None
    for e in project.entities(scene):
        if e.is_folder:
            world = e.id
        if "sky" in e.components:
            e.components["sky"] = {**e.components["sky"], "color": sky, "ambient": ambient}
            project.put_entity(e, scene)
        elif e.name == "Ground":
            e.components["transform"]["scale"] = [40.0, 0.2, 40.0]
            e.components["material"] = {**e.components.get("material", {}),
                                         "color": [*ground, 1.0]}
            project.put_entity(e, scene)
    return world


def _door(project: Project, scene: str, parent: str, name: str, at: list, leads_to: str,
          arrival: str) -> str:
    """A doorway: two posts and a lintel, with a scene exit filling the opening."""
    x, y, z = at
    for part, position, scale in (("Post", [x - 1.1, 1.25, z], [0.3, 2.5, 0.3]),
                                  ("Post", [x + 1.1, 1.25, z], [0.3, 2.5, 0.3]),
                                  ("Lintel", [x, 2.6, z], [2.5, 0.3, 0.3])):
        _put(project, scene, f"{name} {part}", parent, {
            "transform": {"position": position, "scale": scale},
            "mesh": {"asset": "prim_cube"}, "material": {"color": [0.45, 0.32, 0.2, 1.0]},
            "collider": {"shape": "box"}})
    return _put(project, scene, name, parent, {
        "transform": {"position": [x, 1.25, z]},
        "scene_exit": {"scene": leads_to, "arrival": arrival, "activation": "enter",
                       "size": [1.9, 2.5, 0.6], "fade": 0.5}})


def _pad(project: Project, scene: str, parent: str, name: str, at: list, leads_to: str,
         arrival: str) -> str:
    """A glowing teleport pad: stand on it and press Use."""
    x, _y, z = at
    _put(project, scene, f"{name} Disc", parent, {
        "transform": {"position": [x, 0.05, z], "scale": [2.0, 0.1, 2.0]},
        "mesh": {"asset": "prim_cylinder"},
        "material": {"color": [0.3, 0.8, 1.0, 1.0], "emissive": [0.2, 0.7, 1.0],
                     "emissive_strength": 2.0}})
    return _put(project, scene, name, parent, {
        "transform": {"position": [x, 1.0, z]},
        "scene_exit": {"scene": leads_to, "arrival": arrival, "activation": "use",
                       "size": [2.0, 2.0, 2.0], "fade": 0.8}})


def _arrival(project: Project, scene: str, parent: str, name: str, at: list, yaw: float) -> str:
    return _put(project, scene, name, parent, {
        "transform": {"position": at, "rotation": [0.0, yaw, 0.0]}, "arrival": {"name": name}})


def make_scene_tour(parent_dir: Path, name: str = TOUR) -> Project:
    """The Scene Tour sample project in parent_dir (see the module's docstring)."""
    project = Project.create(Path(parent_dir), name, "system_map", "Scene Tour")
    for name in ("Cave", "Island"):
        project.undo_stack.push(scenes.AddSceneCommand(project, name))
    project.undo_stack.clear()  # (a new project: nothing to undo)
    cave, island = "scn_cave", "scn_island"
    main_world = _dress(project, MAIN_SCENE, [0.35, 0.55, 0.85], [0.3, 0.32, 0.36],
                        [0.22, 0.32, 0.18])
    cave_world = _dress(project, cave, [0.02, 0.02, 0.03], [0.08, 0.07, 0.07], [0.2, 0.18, 0.17])
    island_world = _dress(project, island, [0.55, 0.8, 0.95], [0.45, 0.45, 0.42],
                          [0.76, 0.7, 0.48])
    # Arrivals: a few steps clear of the way back (all of them on its +Z side), facing away
    # from it (+Z: yaw 180, as -Z is ahead at yaw 0).
    in_cave = _arrival(project, cave, cave_world, "From the Main", [0.0, 0.5, 2.0], 180.0)
    in_island = _arrival(project, island, island_world, "From the Pad", [0.0, 0.5, 3.0], 180.0)
    from_cave = _arrival(project, MAIN_SCENE, main_world, "From the Cave", [0.0, 0.5, -3.5],
                         180.0)
    from_island = _arrival(project, MAIN_SCENE, main_world, "From the Island",
                           [5.0, 0.5, 3.0], 180.0)
    _door(project, MAIN_SCENE, main_world, "Cave Door", [0.0, 0.0, -6.0], cave, in_cave)
    _pad(project, MAIN_SCENE, main_world, "Teleport Pad", [5.0, 0.0, 0.0], island, in_island)
    _door(project, cave, cave_world, "Way Out", [0.0, 0.0, -0.5], MAIN_SCENE, from_cave)
    _pad(project, island, island_world, "Pad Home", [0.0, 0.0, 0.0], MAIN_SCENE, from_island)
    # The cave is lit by a lamp, not the sun.
    for e in project.entities(cave):
        if "light" in e.components and e.components["light"].get("type") == "directional":
            e.components["light"]["intensity"] = 0.15
            project.put_entity(e, cave)
    _put(project, cave, "Lamp", cave_world, {
        "transform": {"position": [0.0, 3.0, 0.0]},
        "light": {"type": "point", "color": [1.0, 0.7, 0.4], "intensity": 4.0, "range": 15.0}})
    return project


# The samples File ▸ New Sample Project offers: (label, folder name, maker).
def samples() -> list[tuple[str, str, object]]:
    from . import world_tour
    return [("World Tour", world_tour.TOUR, world_tour.make_world_tour),
            ("Scene Tour", TOUR, make_scene_tour)]


def free_name(parent_dir: Path, name: str) -> str:
    """name, or name2, name3, ... : the first that isn't a folder in parent_dir already."""
    found, n = name, 2
    while (Path(parent_dir) / found).exists():
        found, n = f"{name}{n}", n + 1
    return found


if __name__ == "__main__":  # pragma: no cover
    if len(sys.argv) != 2:
        sys.exit("usage: python -m midgard_editor.samples <folder>")
    made = make_scene_tour(Path(sys.argv[1]))
    print(f"Made {made.config_path}")
