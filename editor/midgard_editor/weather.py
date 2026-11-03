# SPDX-License-Identifier: Apache-2.0
"""The project's weather types (milestone 5.15): kinds of weather, and what each does.

A weather type is a dict with "id", "name" and "weather" (as the weather_type table holds it;
schema/migrations/0018_weather_type.sql):

    {"clouds": 0, "darkness": 0, "tint": [1, 1, 1],   how it looks (0-1; the light's colour)
     "visibility": 0,                                  metres you can see; 0: no limit
     "falls": "none", "amount": 0,                     none, rain, snow, ash or sand; 0-1
     "lightning": 0,                                   flashes a minute
     "wind": 0, "gusts": 0,                            metres a second; 0-1
     "wetness": 0, "temperature": 0,                   0-1; degrees Celsius added
     "sounds": [{"event": "<sound event id>", "volume": 1}], "thunder": "<id>"}

A project starts with ten (clear, cloudy, rain, thunderstorm, snow, blizzard, fog, windy,
sandstorm, ash fall). The engine reads them in midgard/weather.h.
"""
import copy
import json
import re
from typing import Any

from PySide6.QtGui import QUndoCommand

from .project import Project

FALLS = ("none", "rain", "snow", "ash", "sand")
# Each number a weather type has: (least, most, when not given).
NUMBERS = {
    "clouds": (0.0, 1.0, 0.0),
    "darkness": (0.0, 1.0, 0.0),
    "visibility": (0.0, 1.0e6, 0.0),
    "amount": (0.0, 1.0, 0.5),
    "lightning": (0.0, 60.0, 0.0),
    "wind": (0.0, 100.0, 0.0),
    "gusts": (0.0, 1.0, 0.0),
    "wetness": (0.0, 1.0, 0.0),
    "temperature": (-100.0, 100.0, 0.0),
}


def load_all(project: Project) -> list[dict[str, Any]]:
    """Every weather type, by name (none in a project from before migration 18)."""
    with project.connect() as con:
        if not con.execute("SELECT 1 FROM sqlite_master WHERE name = 'weather_type'").fetchone():
            return []
        rows = con.execute("SELECT id, name, weather FROM weather_type")
        types = [{"id": i, "name": n, "weather": json.loads(w)} for i, n, w in rows]
    return sorted(types, key=lambda w: w["name"].lower())


def load(project: Project, type_id: str) -> dict[str, Any] | None:
    return next((w for w in load_all(project) if w["id"] == type_id), None)


def put(project: Project, weather_type: dict[str, Any]) -> None:
    with project.connect() as con:
        con.execute("INSERT INTO weather_type (id, name, weather) VALUES (?, ?, ?) "
                    "ON CONFLICT(id) DO UPDATE SET name = excluded.name, "
                    "weather = excluded.weather",
                    (weather_type["id"], weather_type["name"],
                     json.dumps(weather_type["weather"])))
    project.weather_changed.emit(weather_type["id"])


def delete(project: Project, type_id: str) -> None:
    with project.connect() as con:
        con.execute("DELETE FROM weather_type WHERE id = ?", (type_id,))
    project.weather_changed.emit("")


def new_id(project: Project, name: str) -> str:
    """An id for a new weather type named `name`, made unique."""
    stem = re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_") or "weather"
    taken = {w["id"] for w in load_all(project)}
    wid, n = stem, 2
    while wid in taken:
        wid, n = f"{stem}_{n}", n + 1
    return wid


def problems_of(weather: Any, events: list[dict[str, Any]]) -> list[str]:
    """What's wrong with one weather type's settings, in words (without its name)."""
    if not isinstance(weather, dict):
        return ["its settings aren't a list of settings"]
    known = {e["id"] for e in events} | {e["name"] for e in events}
    out = []
    for key, (least, most, _) in NUMBERS.items():
        value = weather.get(key)
        if value is None:
            continue
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            out.append(f"its {key} is '{value}', not a number")
        elif not least <= value <= most:
            out.append(f"its {key} is {value:g}; it is between {least:g} and {most:g}, so "
                       f"{max(least, min(most, value)):g} is used")
    tint = weather.get("tint")
    if tint is not None and not (isinstance(tint, list) and len(tint) == 3
                                 and all(isinstance(v, (int, float)) for v in tint)):
        out.append("its tint isn't three numbers (red, green, blue)")
    falls = weather.get("falls", "none")
    if falls not in FALLS:
        out.append(f"'{falls}' falls from its sky: it can be none, rain, snow, ash or sand")
    sounds = weather.get("sounds", [])
    if not isinstance(sounds, list):
        out.append("its sounds aren't a list")
        sounds = []
    for sound in sounds:
        event = sound if isinstance(sound, str) else (sound or {}).get("event", "")
        if event and event not in known:
            out.append(f"it plays sound event '{event}', which the project doesn't have")
    thunder = weather.get("thunder", "")
    if thunder and thunder not in known:
        out.append(f"its thunder is sound event '{thunder}', which the project doesn't have")
    return out


def problems(project: Project, events: list[dict[str, Any]]) -> list[str]:
    """What's wrong with the project's weather types, one sentence each."""
    return [f"Weather '{w['name']}': {p}." for w in load_all(project)
            for p in problems_of(w["weather"], events)]


def settings_problems(settings: Any, type_ids: set[str]) -> list[str]:
    """What's wrong with a world's weather settings ("weather" in a world type's recipe:
    midgard/weather_sim.h): weather that isn't one of the types, tables without weather. The
    engine says what it can't read at all as the game is cooked."""
    if not isinstance(settings, dict):
        return []
    out: list[str] = []

    def known(weather: Any, where: str) -> None:
        sentence = f"{where} has weather '{weather}', which isn't one of the weather types"
        if weather not in type_ids and sentence not in out:
            out.append(sentence)

    for table in settings.get("tables") or []:
        if not isinstance(table, dict):
            continue
        where = f"weather table '{table.get('name', '')}'"
        weights = table.get("weather") or {}
        if not any(isinstance(w, (int, float)) and w > 0 for w in weights.values()):
            out.append(f"{where} has no weather")
        for weather in weights:
            known(weather, where)
        for before, after in (table.get("after") or {}).items():
            known(before, where)
            for weather in after or {}:
                known(weather, where)
    for weather in settings.get("lasts") or {}:
        if weather != "default":
            known(weather, "how long the weather lasts")
    for weather in (settings.get("fronts") or {}).get("weather") or {}:
        known(weather, "the weather fronts")
    for step in settings.get("controlled") or []:
        if isinstance(step, dict):
            known(step.get("weather"), "the controlled weather")
    return out


def world_problems(project: Project) -> list[str]:
    """What's wrong with the weather of each world type (its latest version), one sentence each."""
    from . import world_types

    type_ids = {w["id"] for w in load_all(project)}
    out = []
    for world_type in world_types.load_all(project):
        found = world_types.recipe(project, world_type["id"]) or {}
        out += [f"World type '{world_type['name']}': {p}."
                for p in settings_problems(found.get("weather"), type_ids)]
    return out


class WeatherCommand(QUndoCommand):
    """Adds (old None), changes or deletes (new None) a weather type; changes to the same
    setting in a row merge into one step."""
    ID = 5151

    def __init__(self, project: Project, old: dict | None, new: dict | None, label: str,
                 field: str = ""):
        weather_type = new or old or {}
        verb = "Add" if old is None else "Delete" if new is None else "Change"
        super().__init__(f"{verb} weather {weather_type.get('name', '')} {label}".strip())
        self.project = project
        self.old, self.new = copy.deepcopy(old), copy.deepcopy(new)
        self.wid, self.field = weather_type.get("id", ""), field

    def id(self) -> int:
        return self.ID

    def mergeWith(self, other: QUndoCommand) -> bool:
        if (not isinstance(other, WeatherCommand) or not self.field or other.wid != self.wid
                or other.field != self.field or self.new is None or other.new is None):
            return False
        self.new = other.new
        return True

    def _set(self, weather_type: dict | None) -> None:
        if weather_type is None:
            delete(self.project, self.wid)
        else:
            put(self.project, weather_type)

    def redo(self) -> None:
        self._set(self.new)

    def undo(self) -> None:
        self._set(self.old)
