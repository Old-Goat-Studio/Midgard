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

from collections.abc import Callable as _Callable

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
    velocity: Vector
    """Its speed and direction in metres per second (0, 0, 0 if it has no physics). Assign one
    to a dynamic rigid body to set it moving; the player walks, so assigning its velocity raises
    ValueError."""
    on_ground: bool
    """True while the player stands on something (False for every other entity; read-only)."""

    def move(self, x: float, y: float, z: float) -> None:
        """Moves the entity by (x, y, z) metres. A physics body jumps there with it."""

    def add_force(self, x: float, y: float, z: float) -> None:
        """Pushes a dynamic rigid body with a force in newtons, for the next physics step (call it
        every frame for a steady push). ValueError if it has no dynamic rigid body."""

    def add_impulse(self, x: float, y: float, z: float) -> None:
        """Gives a dynamic rigid body a sudden push in kg m/s: its speed changes by this divided
        by its mass. ValueError if it has no dynamic rigid body."""

    def __eq__(self, other: object) -> bool: ...
    def __hash__(self) -> int: ...


class Hit:
    """What a raycast met (read-only)."""

    entity: Entity | None
    """The entity whose collider was hit."""
    point: Vector
    """Where, in metres."""
    normal: Vector
    """The surface's direction there, facing the ray."""
    distance: float
    """Metres from the ray's origin."""


class Camera:
    """The player's camera: use midgard.camera. It has three views, "first" (person), "third"
    (person) and "top" (down), which the player switches between with the view action; scripts
    can change the view too, shake the camera and take it briefly for a scripted look (a
    conversation's close-up), after which the player's own view comes back. In a scene without
    a player, or without a camera, it does nothing (active is False)."""

    active: bool
    """True when the scene has a player and a camera to follow it (read-only)."""
    view: str
    """The view, "first", "third" or "top". Assigning one blends to it (ValueError for any other
    name); the player keeps facing the same way."""
    position: Vector
    """Where the camera is, in metres, after blends but without its shake (read-only)."""
    forward: Vector
    """Which way it looks (read-only)."""
    up: Vector
    """Its up direction, from the world's shape (read-only)."""
    fov: float
    """Its vertical field of view in degrees (read-only)."""
    blending: bool
    """True while it blends from one view, or a scripted look, to another (read-only)."""
    scripted: bool
    """True from look() until the player's view is fully back (read-only)."""

    def set_view(self, view: str, blend: bool = True) -> None:
        """Changes the view ("first", "third" or "top"), blended, or at once with blend=False."""

    def shake(self, strength: float = 0.2, seconds: float = 0.5) -> None:
        """Shakes the camera by up to strength metres, fading out over the seconds. A new shake
        replaces one still going."""

    def look(self, position: Vector, target: Vector, seconds: float = 3.0) -> None:
        """Blends the camera to look from position at target and keeps it there until the
        seconds (counted from now, the blend included) have passed, then blends back to the
        player's view. The player can still move meanwhile."""

    def release(self) -> None:
        """Ends a scripted look early: the player's view comes back, blended."""


camera: Camera
"""The player's camera."""


def raycast(origin: Vector, direction: Vector, distance: float = 1000.0, *,
            ignore: Entity | None = None, triggers: bool = False) -> Hit | None:
    """The first collider a ray from origin along direction meets within distance metres, or
    None. ignore skips one entity (often the one casting the ray); triggers=True includes
    triggers."""


def overlap_sphere(centre: Vector, radius: float, *, triggers: bool = False) -> list[Entity]:
    """Every entity whose collider is at least partly inside a sphere, in id order."""


def overlap_box(centre: Vector, size: Vector, rotation: Vector = (0.0, 0.0, 0.0), *,
                triggers: bool = False) -> list[Entity]:
    """Every entity whose collider is at least partly inside a box (its full size, turned by
    rotation in degrees), in id order."""


def gravity(position: Vector) -> Vector:
    """Gravity at a point in m/s², from the world's shape (towards a planet's centre, outwards
    in a hollow world or a cylinder)."""


def up(position: Vector) -> Vector:
    """Which way is up at a point, from the world's shape."""


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


class GameTime:
    """A moment as a calendar shows it (read-only). Names are in English."""

    year: int
    month: int
    """1-based."""
    day: int
    """1-based, in the month."""
    hour: int
    minute: int
    second: int
    day_of_year: int
    """0-based."""
    weekday: str
    """The weekday's name, such as "Óðinsdagr"."""
    month_name: str
    season: str
    """The season's name, or "" for a calendar without seasons."""
    time: int
    """The clock: game seconds since year 1, month 1, day 1, 00:00."""


class Time:
    """The game's clock: use midgard.time. One clock runs for the whole world, in game seconds;
    each calendar shows it its own way (the standard calendar, and the calendar of the body the
    player is on, which is the standard one until bodies have their own). Called, it gives the
    seconds since the game's scripts started (real time), as in API v1."""

    seconds: int
    """The clock, in game seconds (read-only)."""
    scale: float
    """Game seconds per real second (read-only; see set_scale)."""

    def __call__(self) -> float:
        """Seconds since the game's scripts started (real time)."""

    def now(self) -> GameTime:
        """The date and time on the player's body."""

    def standard(self) -> GameTime:
        """The date and time on the standard calendar."""

    def season(self) -> str:
        """The season on the player's body ("" without seasons)."""

    def solar_phase(self) -> float:
        """Where the day is: 0 sunrise, 0.25 noon, 0.5 sunset, 0.75 midnight, whatever the day's
        length or its share of daylight."""

    def is_day(self) -> bool:
        """True between sunrise and sunset."""

    def set_scale(self, scale: float) -> None:
        """Game seconds per real second: 0 stops time (a cutscene), the calendar's own makes a
        day last its real minutes. ValueError if negative."""

    def advance(self, *, days: float = 0, hours: float = 0, minutes: float = 0,
                seconds: float = 0, skip: str = "all") -> None:
        """Moves the clock on at once (sleeping, waiting), in the player's calendar's days and
        hours. skip says which of the signals passed are delivered (on the next frame): "all",
        "latest" (the last of each kind) or "none". ValueError for going backwards."""

    def on(self, signal: str, callback: _Callable[[], None]) -> None:
        """Calls callback() each time the clock passes a signal on the player's body: "hour",
        "day", "month", "season", "year", "dawn" (sunrise) or "dusk" (sunset). Once for each,
        however much time passes in a frame. A callback that raises pauses its script, like its
        other functions; when its script is edited, the new version of the function is called.
        ValueError for an unknown signal."""


time: Time
"""The game's clock."""


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
