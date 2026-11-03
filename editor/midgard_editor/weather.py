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
