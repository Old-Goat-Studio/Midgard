# SPDX-License-Identifier: Apache-2.0
"""Scenes and entities in {project}.db (component shapes: schema/components.md)."""
import json
import math
from dataclasses import dataclass, field
from typing import Any

MAIN_SCENE = "scn_main"
WORLD_FOLDER = "World"  # the folder holding a new scene's camera, sky, light, ground and player


@dataclass
class Entity:
    id: str
    name: str
    parent: str | None = None
    components: dict[str, Any] = field(default_factory=dict)

    @property
    def is_folder(self) -> bool:
        """A folder in the Scene panel: it groups entities and stays at the origin."""
        return "folder" in self.components

    def transform(self) -> dict[str, list[float]]:
        t = self.components.get("transform", {})
        return {"position": t.get("position", [0.0, 0.0, 0.0]),
                "rotation": t.get("rotation", [0.0, 0.0, 0.0]),
                "scale": t.get("scale", [1.0, 1.0, 1.0])}


def look_at_rotation(eye: tuple[float, float, float],
                     target: tuple[float, float, float]) -> list[float]:
    """Euler degrees [pitch, yaw, 0] that point an entity's -Z axis from eye towards target
    (R = Ry · Rx · Rz, as in schema/components.md)."""
    dx, dy, dz = (target[i] - eye[i] for i in range(3))
    yaw = math.degrees(math.atan2(-dx, -dz))
    pitch = math.degrees(math.atan2(dy, math.hypot(dx, dz)))
    return [round(pitch, 3), round(yaw, 3), 0.0]


DEFAULT_SETTINGS = {"sky": [0.08, 0.11, 0.18], "ambient": [0.18, 0.2, 0.26]}


def sky_component(settings: dict[str, Any] | None = None) -> dict[str, Any]:
    """A sky component with a scene's settings' colours (or the defaults)."""
    settings = {**DEFAULT_SETTINGS, **(settings or {})}
    return {"color": list(settings["sky"]), "ambient": list(settings["ambient"])}


def default_entities() -> list[Entity]:
    """What a new project's main scene starts with: the World folder first (its children refer
    to it), then the camera, light, ground, player and sky inside it."""
    eye = (4.0, 3.0, 6.0)
    world = "e000006"
    return [
        Entity(world, WORLD_FOLDER, None, {"folder": {}}),
        Entity("e000001", "Camera", world, {
            "transform": {"position": list(eye), "rotation": look_at_rotation(eye, (0, 0.5, 0))},
            "camera": {"fov": 55, "near": 0.05, "far": 500}}),
        Entity("e000002", "Sun", world, {
            # From behind the default camera's shoulder, so the faces it sees are lit.
            "transform": {"rotation": [-50.0, 35.0, 0.0]},
            "light": {"type": "directional", "color": [1.0, 0.97, 0.9], "intensity": 1.0}}),
        Entity("e000003", "Ground", world, {
            "transform": {"position": [0.0, -0.1, 0.0], "scale": [10.0, 0.2, 10.0]},
            "mesh": {"asset": "prim_cube"}, "material": {"color": [0.22, 0.32, 0.18, 1.0]}}),
        Entity("e000004", "Player", world, {
            "transform": {"position": [0.0, 0.5, 0.0]},
            "mesh": {"asset": "prim_cube"}, "material": {"color": [0.79, 0.64, 0.15, 1.0]},
            "player": {"speed": 4.0}}),  # the camera follows it in third person
        Entity("e000005", "Sky", world, {"sky": sky_component()}),
    ]


def parents_first(entities: list[Entity]) -> list[Entity]:
    """The entities with every parent before its children (otherwise in the given order), as
    the engine needs them when they are added one by one."""
    by_id = {e.id: e for e in entities}
    placed: set[str] = set()
    out: list[Entity] = []

    def place(e: Entity, seen: frozenset[str] = frozenset()) -> None:
        if e.id in placed:
            return
        parent = by_id.get(e.parent) if e.parent else None
        if parent is not None and parent.id not in seen:  # (a loop is placed as it is)
            place(parent, seen | {e.id})
        placed.add(e.id)
        out.append(e)

    for e in entities:
        place(e)
    return out


def descendants(entities: list[Entity], entity_id: str) -> list[Entity]:
    """Every entity inside entity_id (children, their children, ...), parents first."""
    inside: set[str] = {entity_id}
    grew = True
    while grew:
        grew = False
        for e in entities:
            if e.parent in inside and e.id not in inside:
                inside.add(e.id)
                grew = True
    return [e for e in parents_first(entities) if e.id in inside and e.id != entity_id]


def row_to_entity(row: tuple) -> Entity:
    return Entity(row[0], row[1], row[2], json.loads(row[3]))
