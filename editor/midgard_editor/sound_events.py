# SPDX-License-Identifier: Apache-2.0
"""The project's sound events (milestone 5.14): the named sounds its games play.

An event is a dict with "id", "name" and "event" (its settings, as the sound_event table holds
them; schema/migrations/0014_sound_event.sql):

    {"sounds": [{"sound": "snd:<path>", "weight": 1}], "bus": "effects",
     "volume": [min, max], "pitch": [min, max], "loop": false, "cooldown": 0,
     "max_voices": 0, "spatial": true, "min_distance": 1, "max_distance": 40}

Each time it plays, one of its sounds is picked by weight, and its volume and pitch within
their ranges (midgard/sound_events.h). Entities play one with a sound component
({"event": "<id>", "autoplay": true, "volume": 1}); scripts by name or id (midgard.audio.play).
"""
import copy
import json
import re
from pathlib import Path
from typing import Any

from PySide6.QtGui import QUndoCommand

from . import sounds
from .project import Project

BUSES = {"music": "Music", "effects": "Effects", "voice": "Voice", "ambience": "Ambience",
         "interface": "Interface"}
# A library sound's type, its event's bus (and whether it loops).
LIBRARY_BUSES = {"music": ("music", True), "ambiance": ("ambience", True),
                 "effects": ("effects", False)}


def load_all(project: Project) -> list[dict[str, Any]]:
    """Every sound event, by name (none in a project from before migration 14)."""
    with project.connect() as con:
        if not con.execute("SELECT 1 FROM sqlite_master WHERE name = 'sound_event'").fetchone():
            return []
        rows = con.execute("SELECT id, name, event FROM sound_event")
        events = [{"id": i, "name": n, "event": json.loads(e)} for i, n, e in rows]
    return sorted(events, key=lambda e: e["name"].lower())


def load(project: Project, event_id: str) -> dict[str, Any] | None:
    return next((e for e in load_all(project) if e["id"] == event_id), None)


def put(project: Project, event: dict[str, Any]) -> None:
    with project.connect() as con:
        con.execute("INSERT INTO sound_event (id, name, event) VALUES (?, ?, ?) "
                    "ON CONFLICT(id) DO UPDATE SET name = excluded.name, event = excluded.event",
                    (event["id"], event["name"], json.dumps(event["event"])))
    project.sound_events_changed.emit(event["id"])


def delete(project: Project, event_id: str) -> None:
    with project.connect() as con:
        con.execute("DELETE FROM sound_event WHERE id = ?", (event_id,))
    project.sound_events_changed.emit("")


def new_id(project: Project, name: str) -> str:
    """An id for a new event named `name`: sfx_<name>, made unique."""
    stem = "sfx_" + (re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_") or "sound")
    taken = {e["id"] for e in load_all(project)}
    eid, n = stem, 2
    while eid in taken:
        eid, n = f"{stem}_{n}", n + 1
    return eid


def sounds_of(event: dict[str, Any]) -> list[str]:
    """The "snd:" ids an event plays."""
    return sounds.sounds_in(event.get("event", {}).get("sounds", []))


def event_for_sound(project: Project, file: Path, name: str = "") -> dict[str, Any]:
    """A new event that plays one sound file (in the project): on the bus its library type
    suggests (music and ambiance loop), named after it."""
    relative = Path(file).resolve().relative_to(Path(project.root).resolve()).as_posix()
    parts = Path(relative).parts
    kind = parts[1] if parts[:1] == ("library",) and len(parts) > 1 else ""
    if parts[:1] == ("audio",) and len(parts) > 2:
        kind = parts[1]
    bus, loop = LIBRARY_BUSES.get(kind, ("effects", False))
    name = name or Path(relative).stem.replace("_", " ").strip().capitalize() or "Sound"
    settings: dict[str, Any] = {"sounds": [{"sound": sounds.ref(relative), "weight": 1}],
                                "bus": bus}
    if loop:
        settings["loop"] = True
    if bus == "music":
        settings["spatial"] = False  # (music is heard the same everywhere)
    return {"id": new_id(project, name), "name": name, "event": settings}


def find_for_sound(project: Project, sound: str) -> dict[str, Any] | None:
    """An event that plays just this sound, if the project has one."""
    return next((e for e in load_all(project) if sounds_of(e) == [sound]), None)


class SoundEventCommand(QUndoCommand):
    """Adds (old None), changes or deletes (new None) a sound event. Changes to the same
    setting of the same event in a row (a slider dragged) merge into one step."""
    ID = 5141

    def __init__(self, project: Project, old: dict | None, new: dict | None, label: str,
                 field: str = ""):
        event = new or old or {}
        verb = "Add" if old is None else "Delete" if new is None else "Change"
        super().__init__(f"{verb} sound {event.get('name', '')} {label}".strip())
        self.project = project
        self.old, self.new = copy.deepcopy(old), copy.deepcopy(new)
        self.eid, self.field = event.get("id", ""), field

    def id(self) -> int:
        return self.ID

    def mergeWith(self, other: QUndoCommand) -> bool:
        if (not isinstance(other, SoundEventCommand) or not self.field or other.eid != self.eid
                or other.field != self.field or self.new is None or other.new is None):
            return False
        self.new = other.new
        return True

    def _set(self, event: dict | None) -> None:
        if event is None:
            delete(self.project, self.eid)
        else:
            put(self.project, event)

    def redo(self) -> None:
        self._set(self.new)

    def undo(self) -> None:
        self._set(self.old)


# ---- the buses' volumes (migration 17) ------------------------------------------------------
BUS_NAMES = {"master": "Master", **BUSES}


def load_buses(project: Project) -> dict[str, float]:
    """Each bus's volume as games start (1 for one not set; nothing before migration 17)."""
    volumes = {bus: 1.0 for bus in BUS_NAMES}
    with project.connect() as con:
        if con.execute("SELECT 1 FROM sqlite_master WHERE name = 'audio_bus'").fetchone():
            for bus, volume in con.execute("SELECT bus, volume FROM audio_bus"):
                volumes[bus] = float(volume)
    return volumes


def put_buses(project: Project, volumes: dict[str, float]) -> None:
    with project.connect() as con:
        con.execute("DELETE FROM audio_bus")
        con.executemany("INSERT INTO audio_bus (bus, volume) VALUES (?, ?)",
                        [(b, round(float(v), 4)) for b, v in volumes.items()
                         if b in BUS_NAMES and v != 1.0])
    project.sound_events_changed.emit("")


class BusesCommand(QUndoCommand):
    """The buses' volumes changed (a fader dragged: its moves merge into one step)."""
    ID = 5144

    def __init__(self, project: Project, old: dict[str, float], new: dict[str, float],
                 bus: str):
        super().__init__(f"Change the {BUS_NAMES.get(bus, bus).lower()} volume")
        self.project, self.old, self.new, self.bus = project, dict(old), dict(new), bus

    def id(self) -> int:
        return self.ID

    def mergeWith(self, other: QUndoCommand) -> bool:
        if not isinstance(other, BusesCommand) or other.bus != self.bus:
            return False
        self.new = other.new
        return True

    def redo(self) -> None:
        put_buses(self.project, self.new)

    def undo(self) -> None:
        put_buses(self.project, self.old)


def audio_message(project: Project) -> dict[str, Any]:
    """Everything the Audio panel edits, as the live link's audio.put sends it."""
    from . import ambience, music
    return {"events": load_all(project), "playlists": music.load_all(project),
            "ambience": ambience.load_all(project), "buses": load_buses(project)}
