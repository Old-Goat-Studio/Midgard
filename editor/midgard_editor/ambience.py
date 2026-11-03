# SPDX-License-Identifier: Apache-2.0
"""The project's ambience sets (milestone 5.14): the background sound of places.

A set is a dict with "id", "name" and "ambience" (as the ambience table holds it;
schema/migrations/0016_ambience.sql):

    {"layers": [{"event": "<sound event id>", "times": ["day"], "volume": 1}],
     "sprinkles": [{"event": "<id>", "times": ["night"], "every": [10, 30],
                    "distance": [5, 30]}],
     "scenes": [], "world_types": [], "biomes": [],   (empty: anywhere)
     "priority": 0, "crossfade": 4}

The game plays the set that fits where the player is best (as music: midgard/ambience.h). Its
layers loop, each at its times of day (none: always), fading in and out as dawn and dusk come;
its sprinkles are one-shots now and then at a random spot around the listener.
"""
import copy
import json
import re
from typing import Any

from PySide6.QtGui import QUndoCommand

from .music import TIMES
from .project import Project


def load_all(project: Project) -> list[dict[str, Any]]:
    """Every ambience set, by name (none in a project from before migration 16)."""
    with project.connect() as con:
        if not con.execute("SELECT 1 FROM sqlite_master WHERE name = 'ambience'").fetchone():
            return []
        rows = con.execute("SELECT id, name, ambience FROM ambience")
        sets = [{"id": i, "name": n, "ambience": json.loads(a)} for i, n, a in rows]
    return sorted(sets, key=lambda a: a["name"].lower())


def load(project: Project, set_id: str) -> dict[str, Any] | None:
    return next((a for a in load_all(project) if a["id"] == set_id), None)


def put(project: Project, ambience_set: dict[str, Any]) -> None:
    with project.connect() as con:
        con.execute("INSERT INTO ambience (id, name, ambience) VALUES (?, ?, ?) "
                    "ON CONFLICT(id) DO UPDATE SET name = excluded.name, "
                    "ambience = excluded.ambience",
                    (ambience_set["id"], ambience_set["name"],
                     json.dumps(ambience_set["ambience"])))
    project.sound_events_changed.emit("")  # (the Audio panel shows them all)


def delete(project: Project, set_id: str) -> None:
    with project.connect() as con:
        con.execute("DELETE FROM ambience WHERE id = ?", (set_id,))
    project.sound_events_changed.emit("")


def new_id(project: Project, name: str) -> str:
    """An id for a new set named `name`: amb_<name>, made unique."""
    stem = "amb_" + (re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_") or "ambience")
    taken = {a["id"] for a in load_all(project)}
    aid, n = stem, 2
    while aid in taken:
        aid, n = f"{stem}_{n}", n + 1
    return aid


def problems(project: Project, events: list[dict[str, Any]]) -> list[str]:
    """What's wrong with the sets: events that aren't there, times of day that aren't."""
    known = {e["id"] for e in events} | {e["name"] for e in events}
    out = []
    for a in load_all(project):
        settings = a["ambience"]
        parts = [("layer", layer) for layer in settings.get("layers", [])]
        parts += [("sprinkle", s) for s in settings.get("sprinkles", [])]
        for kind, part in parts:
            event = part if isinstance(part, str) else part.get("event", "")
            if event not in known:
                out.append(f"Ambience '{a['name']}' has a {kind} playing sound event '{event}', "
                           "which the project doesn't have.")
            for time in ([] if isinstance(part, str) else part.get("times", [])):
                if time not in TIMES:
                    out.append(f"Ambience '{a['name']}' has a {kind} at '{time}': a time of "
                               "day is dawn, day, dusk or night.")
        if not parts:
            out.append(f"Ambience '{a['name']}' has no layers or sprinkles; it is silent.")
    return out


class AmbienceCommand(QUndoCommand):
    """Adds (old None), changes or deletes (new None) an ambience set; changes to the same
    setting in a row merge into one step."""
    ID = 5143

    def __init__(self, project: Project, old: dict | None, new: dict | None, label: str,
                 field: str = ""):
        ambience_set = new or old or {}
        verb = "Add" if old is None else "Delete" if new is None else "Change"
        super().__init__(f"{verb} ambience {ambience_set.get('name', '')} {label}".strip())
        self.project = project
        self.old, self.new = copy.deepcopy(old), copy.deepcopy(new)
        self.aid, self.field = ambience_set.get("id", ""), field

    def id(self) -> int:
        return self.ID

    def mergeWith(self, other: QUndoCommand) -> bool:
        if (not isinstance(other, AmbienceCommand) or not self.field or other.aid != self.aid
                or other.field != self.field or self.new is None or other.new is None):
            return False
        self.new = other.new
        return True

    def _set(self, ambience_set: dict | None) -> None:
        if ambience_set is None:
            delete(self.project, self.aid)
        else:
            put(self.project, ambience_set)

    def redo(self) -> None:
        self._set(self.new)

    def undo(self) -> None:
        self._set(self.old)
