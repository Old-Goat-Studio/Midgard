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
    def on_room_enter(entity, came_from): ...  the player arrived in this scene (after on_start;
                                               came_from is the scene left, by id)
    def on_room_exit(entity, going_to): ...    the player is leaving this scene (the last call)
    def on_animation_event(entity, name): ...  its character's animation graph passed an event

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
    flying: bool
    """The player flies (a jetpack, a ship): where the camera looks, up with jump (Space), down
    with descend (Left Ctrl), with no gravity; set it False to walk again. Only the player
    flies (ValueError for another entity)."""
    speed: float
    """The player's walking speed, metres a second (running multiplies it). Only the player
    (ValueError for another entity)."""
    following: bool
    """True while the player is walking a route given to follow() (read-only)."""
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
    swimming: bool
    """True while the player swims: in the sea deeper than its shoulders, floating at the
    surface (False for every other entity; read-only)."""

    def move(self, x: float, y: float, z: float) -> None:
        """Moves the entity by (x, y, z) metres. A physics body jumps there with it."""

    def follow(self, points: list[Vector]) -> None:
        """The player walks through these points (in the scene's frame) at its speed, as if
        steered, instead of following the controls, until it reaches the last (a cutscene,
        click-to-move, a guided tour); [] stops it. Only the player (ValueError otherwise)."""

    def add_force(self, x: float, y: float, z: float) -> None:
        """Pushes a dynamic rigid body with a force in newtons, for the next physics step (call it
        every frame for a steady push). ValueError if it has no dynamic rigid body."""

    def add_impulse(self, x: float, y: float, z: float) -> None:
        """Gives a dynamic rigid body a sudden push in kg m/s: its speed changes by this divided
        by its mass. ValueError if it has no dynamic rigid body."""

    def set_animation(self, name: str, value: float) -> None:
        """Sets a parameter of the character's animation graph (True and False are 1 and 0).
        ValueError if it has no graph, or its graph no such parameter."""

    def trigger_animation(self, name: str) -> None:
        """Sets a trigger of the character's animation graph: the next transition it lets
        through uses it up. ValueError if its graph has no such trigger."""

    def animation(self, name: str) -> float:
        """A parameter of the character's animation graph ("speed", how fast it moves along
        the ground, is always there). ValueError if it has no graph playing."""

    def animation_state(self, layer: int = 0) -> str:
        """The state a layer of the character's animation graph is in ("" without a graph)."""

    def look_at(self, target: tuple[float, float, float] | Entity | None) -> None:
        """Turns the character's head to a point or an entity (a character's head), no further
        than its look limit; None: back to what its component's "ik" says."""

    def reach(self, hand: str, target: tuple[float, float, float] | Entity | None,
              weight: float = 1.0) -> None:
        """Reaches a hand ("left" or "right") to a point or where an entity is, by a weight (0 to
        1), easing in; None: lets it go. ValueError for another hand."""

    def play_sound(self, event: str, volume: float = 1.0) -> Sound | None:
        """Plays a sound event (by name or id) from this entity, following it as it moves, at
        volume times the event's. The Sound playing, or None (a silent game, an unknown event,
        one cooling down)."""

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


class Sound:
    """A sound playing (midgard.audio.play, Entity.play_sound)."""

    playing: bool
    """Whether it's still playing (False once it has finished or been stopped; read-only)."""

    def stop(self) -> None:
        """Stops it."""

    def set_volume(self, volume: float) -> None:
        """Its volume, times the event's (1 as it started)."""


class Audio:
    """Sound (midgard.audio): sound events, made in the editor, played by name or id. Every
    sound plays on a bus: "music", "effects", "voice", "ambience" or "interface", all under
    "master" (the whole game's volume). A game without sound (a server) plays nothing: play()
    gives None and enabled is False."""

    enabled: bool
    """Whether the game makes sound (read-only)."""
    playing: list[str]
    """The sound events playing now, by id: one for each sound playing (read-only)."""

    def play(self, event: str, volume: float = 1.0,
             at: Entity | Vector | None = None) -> Sound | None:
        """Plays a sound event (by name or id), at volume times the event's: from an entity
        (following it) or a place if `at` says (and the event is placed in the world: louder
        near, muffled behind walls), else heard the same everywhere. The Sound playing, or None
        (a silent game, an unknown event, one cooling down)."""

    def stop_all(self) -> None:
        """Stops every sound playing."""

    def set_volume(self, bus: str, volume: float) -> None:
        """A bus's volume (linear: 0.5 is half). ValueError for an unknown bus."""

    def volume(self, bus: str) -> float:
        """A bus's volume."""


audio: Audio
"""Sound."""


class Music:
    """Music (midgard.music): playlists, made in the editor, chosen by where the player is: the
    scene, its world type, the biome and the time of day. A change of playlist crossfades. A
    script can choose one itself until it lets go, and play stingers over the music."""

    playlist: str
    """The playlist playing, by id ("" for none; read-only)."""
    track: str
    """The sound event of the track playing ("" for none; read-only)."""

    def play(self, playlist: str) -> None:
        """Plays a playlist (by name or id), crossfading, whatever fits where the player is,
        until release(). ValueError for one the game hasn't."""

    def release(self) -> None:
        """Back to the playlist that fits where the player is."""

    def stop(self) -> None:
        """Fades the music out, until play() or release()."""

    def stinger(self, event: str, duck: float = 0.35) -> Sound | None:
        """Plays a sound event over the music (a fanfare, a discovery), which dips to `duck` of
        its level under it and comes back after. The Sound, or None."""


music: Music
"""Music."""


class Ambience:
    """Ambience (midgard.ambience): the background sound of a place, made in the editor as
    ambience sets and chosen like music by where the player is. Its layers fade in and out with
    the time of day; its sprinkles sound now and then around the listener."""

    playing: str
    """The ambience set playing, by id ("" for none; read-only)."""
    layers: list[str]
    """Its layers sounding now: their sound events (read-only)."""

    def play(self, set: str) -> None:
        """Plays an ambience set (by name or id), crossfading, until release(). ValueError for
        one the game hasn't."""

    def release(self) -> None:
        """Back to the ambience that fits where the player is."""

    def stop(self) -> None:
        """Fades the ambience out, until play() or release()."""


ambience: Ambience
"""Ambience."""


class Weather:
    """The weather somewhere (midgard.weather.current()): its weather type, and what it does.
    Compares equal to its id: `if midgard.weather.current() == "rain":`."""

    id: str
    """Its weather type's id ("" when there is no weather)."""
    name: str
    """Its weather type's name."""
    forced: bool
    """Whether a script is forcing it (midgard.weather.force)."""
    front: bool
    """Whether it is a front passing over."""
    clouds: float
    """0 a clear sky .. 1 overcast."""
    darkness: float
    """The daylight it takes, 0 .. 1."""
    visibility: float
    """Metres you can see through it (0: no limit)."""
    falls: str
    """What falls from the sky: "none", "rain", "snow", "ash" or "sand"."""
    amount: float
    """How much falls, 0 .. 1."""
    lightning: float
    """Flashes of lightning a minute."""
    wind: float
    """Wind, in metres a second."""
    gusts: float
    """0 steady .. 1 very gusty."""
    wetness: float
    """How wet things out in the open get, 0 .. 1."""
    temperature: float
    """Degrees Celsius it adds to the climate's temperature."""


class WeatherSpell:
    """A stretch of the same weather in a forecast (midgard.weather.forecast())."""

    id: str
    """Its weather type's id."""
    name: str
    """Its weather type's name."""
    start: int
    """When it starts, on the clock (as midgard.time.now().time)."""
    end: int
    """When the next weather starts."""
    in_hours: float
    """Hours from now until it starts (0 for the weather now)."""
    hours: float
    """How many hours it lasts."""
    forced: bool
    """A script's weather."""
    front: bool
    """A front passing over."""


class Exposure:
    """How the weather treats the player (midgard.weather.exposure())."""

    air: float
    """The air's temperature, degrees Celsius."""
    feels_like: float
    """How warm it feels, degrees Celsius: the air, made colder by wind and by being wet, and
    warmer by heat sources near."""
    wind: float
    """The wind where the player is, metres a second (walls cut it)."""
    heat: float
    """Degrees of warmth from heat sources near (campfires, stoves)."""
    wetness: float
    """How wet the player is: 0 dry .. 1 soaked."""
    roof: float
    """How much of the sky above the player is covered, 0 .. 1."""
    walls: float
    """How much is walled in round about, 0 .. 1."""
    sheltered: bool
    """Under a roof: rain and snow don't reach the player."""


class WeatherApi:
    """The weather (midgard.weather): what it is where the player is, the forecast, and a
    script's own weather. The weather changes by itself (by the world type's weather tables,
    the season and the biome); a script can force some for a while."""

    types: list[str]
    """The weather types' ids (read-only)."""
    wetness: float
    """How wet the ground is where the player is: 0 dry .. 1 soaked. A script can set it (a
    flood, a spell that dries the land); then the weather carries on from there."""
    snow: float
    """How much snow has settled where the player is: 0 none .. 1 covered. Settable too."""
    temperature: float
    """The temperature where the player is, in degrees Celsius: the climate's there (with the
    season), plus the weather's change (read-only)."""

    def current(self) -> Weather:
        """The weather where the player is now."""

    def forecast(self, hours: float = 24.0) -> list[WeatherSpell]:
        """The weather where the player is over the next so many hours, as it will come: the
        first starting now."""

    def on_change(self, callback: _Callable[[Weather, Weather], None]) -> None:
        """Calls callback(before, now) whenever the weather where the player is changes."""

    def force(self, weather: str, hours: float = 0.0) -> None:
        """Makes it this weather (a weather type's id) everywhere from now: for so many hours,
        then the weather there would have been (0: until release()). ValueError if there is no
        such weather type."""

    def release(self) -> None:
        """Back to the weather there would have been."""

    def exposure(self) -> Exposure:
        """How the weather treats the player now: sheltered or not, the wind reaching them, how
        wet they are, the warmth of fires near, and how warm it feels."""

    def set_player_wetness(self, wetness: float) -> None:
        """How wet the player is now, 0 .. 1 (they dried off by the fire, or fell in a river)."""


class StatsApi:
    """The player's stats, scores and counters (midgard.stats), as the Stats panel defines
    them, each by its id or its name: midgard.stats.get("health"), midgard.stats["Score"].

    A stat (Health, Stamina) stays between its least and most and may refill by itself; a
    score is a whole number of points; a counter a whole number up to its target, if it has
    one. KeyError for a stat the project doesn't have."""

    ids: list[str]
    """The stats' ids, in the Stats panel's order (read-only)."""

    def get(self, stat: str) -> float | int:
        """A stat's value (an int for scores and counters)."""

    def set(self, stat: str, value: float) -> float | int:
        """Sets a stat, kept within its range (whole for scores and counters); its new value."""

    def add(self, stat: str, amount: float = 1.0) -> float | int:
        """Adds to a stat (a negative amount takes away); its new value."""

    def min(self, stat: str) -> float | int:
        """A stat's least."""

    def max(self, stat: str) -> float | int | None:
        """A stat's most, or a counter's target (None: it has none, as a score never does)."""

    def set_max(self, stat: str, max: float) -> None:
        """Changes a stat's most (more health as the player grows) or a counter's target; the
        value is kept within it. ValueError for a score, or a most not above the least."""

    def reset(self, stat: str | None = None) -> None:
        """A stat back to its start and its designed most (every stat, with none given)."""

    def name(self, stat: str) -> str:
        """A stat's name, as the player sees it."""

    def kind(self, stat: str) -> str:
        """Its kind: "stat", "score" or "counter"."""

    def on_change(self, callback: _Callable[[str, float, float], None]) -> None:
        """Calls callback(stat, before, now) after a frame in which a stat changed (by itself,
        refilling, or by a script): its id and its value before and after."""

    def __getitem__(self, stat: str) -> float | int: ...
    def __setitem__(self, stat: str, value: float) -> None: ...
    def __contains__(self, stat: str) -> bool: ...


class UiApi:
    """The HUD (midgard.ui): short notes that fade, a prompt, timers, and whether the HUD and
    its crosshair show. The stats show on it by themselves (midgard.stats; the Stats panel says
    which)."""

    hud: bool
    """Whether the HUD shows (False hides it all, as for a cut scene)."""
    crosshair: bool | None
    """True shows the crosshair, False hides it, None (as it starts) shows it in first person
    only."""

    compass: bool
    """Whether the compass shows at the top of the screen."""
    minimap: bool
    """Whether the minimap shows at the bottom right."""
    heading: float
    """The way the camera faces, in degrees from north (0 north, 90 east, 180 south, 270
    west), as the compass shows it on the world's shape (read-only)."""

    def mark(self, entity: Entity, label: str = "") -> None:
        """Marks an entity on the minimap (a quest's goal, a lost friend), with a label if
        given, until unmark()."""

    def unmark(self, entity: Entity) -> bool:
        """Takes an entity's mark off the minimap. False if it had none."""

    screens: list[str]
    """The screens (made in the Menu editor) shown now, bottom to top (read-only)."""

    def show(self, screen: str) -> None:
        """Shows a screen made in the Menu editor (by its id), on top of any others."""

    def hide(self, screen: str) -> bool:
        """Takes a screen away. False if it wasn't shown."""

    def shown(self, screen: str) -> bool:
        """Whether a screen is shown."""

    def on_click(self, screen: str, element: str, callback: _Callable[[], None]) -> None:
        """Calls callback() when the player clicks a button on a screen."""

    def on_change(self, screen: str, element: str,
                  callback: _Callable[[float | bool], None]) -> None:
        """Calls callback(value) when the player moves a slider (its value) or ticks a check
        (True or False) on a screen."""

    def set_enabled(self, screen: str, element: str, enabled: bool) -> None:
        """Lets the player use an element (a button, slider, check or option), or not:
        disabled, it shows its disabled picture (or greyed) and can't be used."""

    def enabled(self, screen: str, element: str) -> bool:
        """Whether the player can use an element."""

    def click(self, screen: str, element: str) -> None:
        """Presses a button as the player would: it does what it says (but doesn't quit the
        game), and on_click hears it at the next update."""

    def set_text(self, screen: str, element: str, text: str) -> None:
        """Changes the text of an element (text, a button, a check) on a screen."""

    def value(self, screen: str, element: str) -> float | bool:
        """A slider's value, or whether a check is ticked, on a screen."""

    def set_value(self, screen: str, element: str, value: float | bool) -> None:
        """Sets a slider's value (or ticks a check: True or False) on a screen."""

    def notify(self, text: str, seconds: float = 4.0) -> None:
        """A short note on the HUD ("Picked up a key") for so many seconds; it fades as it
        goes. Five show at most: the oldest goes first."""

    def prompt(self, text: str | None = None) -> None:
        """The prompt at the bottom of the screen ("Press E to open") until another, or none
        with no text."""

    def timer(self, id: str, seconds: float, label: str = "", count: str = "down",
              on_done: _Callable[[], None] | None = None) -> None:
        """A timer on the HUD, by id (one with the same id starts over): counting "down" from
        seconds to 0, or "up" from 0 (to seconds, or for ever with 0). on_done() is called when
        it ends."""

    def stop_timer(self, id: str) -> bool:
        """Takes a timer away (its on_done isn't called). False if there wasn't one."""

    def timer_value(self, id: str) -> float | None:
        """A timer's seconds: left (counting down) or passed (counting up); None if there is no
        such timer."""


stats: StatsApi
ui: UiApi
weather: WeatherApi
"""The weather."""


class Rooms:
    """Moving between scenes (midgard.rooms). The player is in one scene at a time; a change
    fades the picture out, swaps the scene (the one left stops: its scripts get on_room_exit,
    the new one's start, then get on_room_enter) and fades in."""

    current: str
    """The scene the player is in, by id (read-only)."""
    previous: str
    """The scene the player came from, or "" before the first change (read-only)."""
    changing: bool
    """True while a change is under way (read-only)."""

    def change(self, scene: str, arrival: str = "", fade: float = 0.5) -> None:
        """Takes the player to a scene (its id, such as "scn_cave"), standing at an arrival point
        there (its entity id; "": where the scene puts the player), fading out and in over `fade`
        seconds. ValueError if the game has no such scene. Ignored while the picture is already
        fading out for another change; asked while it fades in (in on_room_enter, say), the
        player goes on to this scene."""

    def take_exit(self, exit: Entity) -> None:
        """Takes a scene exit (an entity with a scene_exit), whatever its activation: how a door
        set to "Only from a script" opens. ValueError if the entity isn't one."""

    def enter_space(self, fade: float = 1.0) -> None:
        """Into space: above the planet the player is on (from where they are), or outside the
        habitat or hollow world. ValueError on a world with no outside (a bounded area, a
        flat-wrap cylinder)."""

    in_space: bool
    """Whether the player is in space (read-only)."""
    space_from: str
    """In space: the scene left for it ("" when not in space; read-only)."""
    ceiling: float
    """A planet's atmosphere ceiling, metres above its sea level: fly above it and you're in
    space; fly back down through it and you land below (0 off a planet; read-only)."""
    altitude: float
    """How high the player is above the world's sea level, in metres (read-only)."""


rooms: Rooms
"""Moving between scenes."""


class Nav:
    """Where agents can walk (midgard.nav): paths over the scene's navigation mesh. A scene
    without terrain has one from its scenery (static colliders); a world with terrain has one
    made round the player as they go (about 100 m each way), across a cylinder's join and a
    planet's face edges."""

    ready: bool
    """Whether the navigation round the player is all made (read-only)."""

    def path(self, start: Vector, end: Vector, *, partial: bool = False) -> list[Vector] | None:
        """The way from start to end: points a few metres apart on the ground, the first at the
        start. None if there is none: either place too far from where agents can walk, or no
        way between them (with partial=True, the way as near the end as it gets instead)."""

    def nearest(self, point: Vector, reach: float = 2.0) -> Vector | None:
        """The nearest place agents can walk to a point, within `reach` metres, or None."""


nav: Nav
"""Where agents can walk."""


def action(name: str) -> float:
    """An input action's value this frame (-1 to 1 for movement; see the Input editor)."""


def down(name: str) -> bool:
    """True while an input action is held."""


def pressed(name: str) -> bool:
    """True in the frame an input action goes down."""


def input_context() -> str:
    """The input context now: 'on_foot', 'flying', 'menu', 'vehicle' (actions of others read
    nothing)."""


def set_input_context(context: str | None) -> None:
    """Chooses the input context ('vehicle' in a car, say); None: the game's own again."""


def sky() -> Vector:
    """The sky colour (r, g, b)."""


def set_sky(r: float, g: float, b: float) -> None:
    """Changes the sky colour."""
