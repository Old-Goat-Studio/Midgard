# SPDX-License-Identifier: Apache-2.0
"""The midgard module: the engine, for game scripts (API v1).

A script is a Python module in the project's scripts/ folder, attached to an entity with a
script component ({"module": "door"} runs scripts/door.py). Every function is optional:

    import midgard

    def on_start(entity): ...                  once, when the script starts
    def on_update(entity, dt): ...             every frame; dt is the frame time in seconds
    def on_trigger_enter(entity, other): ...   the player entered this entity's trigger
    def on_trigger_exit(entity, other): ...    ... or left it
    def on_reload(entity): ...                 the script was edited and reloaded while playing

The player's own script also gets on_trigger_enter(player, trigger) and on_trigger_exit. A
script that raises an error is paused, and the error (with its file and line) is shown in the
editor, until the script is saved again. print() writes to the game's log.

Game scripts are pure Python: the standard library is available, compiled extension modules are
not (they would need building for every platform).
"""

Vector = tuple[float, float, float]
Color = tuple[float, float, float, float]


class Entity:
    """An entity of the running scene. Using one that has been removed raises ReferenceError."""

    id: str
    """Its stable id, such as "e000004" (read-only)."""
    name: str
    """Its name, as shown in the editor's Scene panel."""
    exists: bool
    """False once the entity has been removed from the scene (read-only)."""
    position: Vector
    """Position in metres, relative to its parent."""
    rotation: Vector
    """Rotation in degrees (pitch, yaw, roll), relative to its parent."""
    scale: Vector
    """Scale on each axis."""
    color: Color
    """Its material colour (r, g, b, a) from 0 to 1. Assign 3 or 4 numbers."""

    def move(self, x: float, y: float, z: float) -> None:
        """Moves the entity by (x, y, z) metres."""

    def __eq__(self, other: object) -> bool: ...
    def __hash__(self) -> int: ...


def log(*values: object) -> None:
    """Writes a line to the game's log (the editor's Log panel while playing)."""


def warn(*values: object) -> None:
    """Writes a warning to the game's log."""


def entity(id: str) -> Entity | None:
    """The entity with this id, or None."""


def find(name: str) -> Entity | None:
    """The first entity with this name, or None."""


def entities() -> list[Entity]:
    """Every entity in the scene."""


def player() -> Entity | None:
    """The entity the player controls, or None."""


def time() -> float:
    """Seconds since the game's scripts started."""


def action(name: str) -> float:
    """An input action's value this frame (-1 to 1 for movement; see the Input editor)."""


def down(name: str) -> bool:
    """True while an input action is held."""


def pressed(name: str) -> bool:
    """True in the frame an input action goes down."""


def sky() -> Vector:
    """The sky colour (r, g, b)."""


def set_sky(r: float, g: float, b: float) -> None:
    """Changes the sky colour."""
