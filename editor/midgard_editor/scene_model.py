# SPDX-License-Identifier: Apache-2.0
"""Scenes and entities in {project}.db (component shapes: schema/components.md)."""
import json
import math
from dataclasses import dataclass, field
from typing import Any

MAIN_SCENE = "scn_main"


@dataclass
class Entity:
    id: str
    name: str
    parent: str | None = None
    components: dict[str, Any] = field(default_factory=dict)

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


def default_entities() -> list[Entity]:
    """What a new project's main scene starts with."""
    eye = (4.0, 3.0, 6.0)
    return [
        Entity("e000001", "Camera", None, {
            "transform": {"position": list(eye), "rotation": look_at_rotation(eye, (0, 0.5, 0))},
            "camera": {"fov": 55, "near": 0.05, "far": 500}}),
        Entity("e000002", "Sun", None, {
            # From behind the default camera's shoulder, so the faces it sees are lit.
            "transform": {"rotation": [-50.0, 35.0, 0.0]},
            "light": {"type": "directional", "color": [1.0, 0.97, 0.9], "intensity": 1.0}}),
        Entity("e000003", "Ground", None, {
            "transform": {"position": [0.0, -0.1, 0.0], "scale": [10.0, 0.2, 10.0]},
            "mesh": {"asset": "prim_cube"}, "material": {"color": [0.22, 0.32, 0.18, 1.0]}}),
        Entity("e000004", "Cube", None, {
            "transform": {"position": [0.0, 0.5, 0.0]},
            "mesh": {"asset": "prim_cube"}, "material": {"color": [0.79, 0.64, 0.15, 1.0]}}),
    ]


DEFAULT_SETTINGS = {"sky": [0.08, 0.11, 0.18], "ambient": [0.18, 0.2, 0.26]}


def row_to_entity(row: tuple) -> Entity:
    return Entity(row[0], row[1], row[2], json.loads(row[3]))
