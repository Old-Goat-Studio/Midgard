# SPDX-License-Identifier: Apache-2.0
"""The project's music playlists (milestone 5.14): which music plays where.

A playlist is a dict with "id", "name" and "playlist" (as the music_playlist table holds it;
schema/migrations/0015_music_playlist.sql):

    {"tracks": ["<sound event id>", ...], "order": "in_order" or "shuffle",
     "scenes": [], "world_types": [], "biomes": [], "times": [],   (empty: anywhere, any time)
     "priority": 0, "crossfade": 3, "gap": [0, 0]}

The game plays the playlist that fits where the player is best (the one naming the most of
scene, world type, biome and time of day that all hold, then the highest priority), crossfading
when that changes (midgard/music.h). Scripts can choose one (midgard.music.play).
"""
import copy
import json
import re
from typing import Any

from PySide6.QtGui import QUndoCommand

from .project import Project

TIMES = {"dawn": "Dawn", "day": "Day", "dusk": "Dusk", "night": "Night"}


def load_all(project: Project) -> list[dict[str, Any]]:
    """Every playlist, by name (none in a project from before migration 15)."""
    with project.connect() as con:
        if not con.execute(
                "SELECT 1 FROM sqlite_master WHERE name = 'music_playlist'").fetchone():
            return []
        rows = con.execute("SELECT id, name, playlist FROM music_playlist")
        lists = [{"id": i, "name": n, "playlist": json.loads(p)} for i, n, p in rows]
    return sorted(lists, key=lambda p: p["name"].lower())


def load(project: Project, playlist_id: str) -> dict[str, Any] | None:
    return next((p for p in load_all(project) if p["id"] == playlist_id), None)


def put(project: Project, playlist: dict[str, Any]) -> None:
    with project.connect() as con:
        con.execute("INSERT INTO music_playlist (id, name, playlist) VALUES (?, ?, ?) "
                    "ON CONFLICT(id) DO UPDATE SET name = excluded.name, "
                    "playlist = excluded.playlist",
                    (playlist["id"], playlist["name"], json.dumps(playlist["playlist"])))
    project.sound_events_changed.emit("")  # (the Audio panel shows both)


def delete(project: Project, playlist_id: str) -> None:
    with project.connect() as con:
        con.execute("DELETE FROM music_playlist WHERE id = ?", (playlist_id,))
    project.sound_events_changed.emit("")


def new_id(project: Project, name: str) -> str:
    """An id for a new playlist named `name`: mus_<name>, made unique."""
    stem = "mus_" + (re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_") or "music")
    taken = {p["id"] for p in load_all(project)}
    pid, n = stem, 2
    while pid in taken:
        pid, n = f"{stem}_{n}", n + 1
    return pid


def problems(project: Project, events: list[dict[str, Any]]) -> list[str]:
    """What's wrong with the playlists: tracks naming no sound event, times of day that aren't."""
    known = {e["id"] for e in events} | {e["name"] for e in events}
    out = []
    for p in load_all(project):
        settings = p["playlist"]
        for track in settings.get("tracks", []):
            if track not in known:
                out.append(f"Playlist '{p['name']}' plays sound event '{track}', which the "
                           "project doesn't have.")
        for time in settings.get("times", []):
            if time not in TIMES:
                out.append(f"Playlist '{p['name']}' plays at '{time}': a time of day is dawn, "
                           "day, dusk or night.")
        if not settings.get("tracks"):
            out.append(f"Playlist '{p['name']}' has no tracks; it never plays.")
    return out


class PlaylistCommand(QUndoCommand):
    """Adds (old None), changes or deletes (new None) a playlist; changes to the same setting in
    a row merge into one step."""
    ID = 5142

    def __init__(self, project: Project, old: dict | None, new: dict | None, label: str,
                 field: str = ""):
        playlist = new or old or {}
        verb = "Add" if old is None else "Delete" if new is None else "Change"
        super().__init__(f"{verb} playlist {playlist.get('name', '')} {label}".strip())
        self.project = project
        self.old, self.new = copy.deepcopy(old), copy.deepcopy(new)
        self.pid, self.field = playlist.get("id", ""), field

    def id(self) -> int:
        return self.ID

    def mergeWith(self, other: QUndoCommand) -> bool:
        if (not isinstance(other, PlaylistCommand) or not self.field or other.pid != self.pid
                or other.field != self.field or self.new is None or other.new is None):
            return False
        self.new = other.new
        return True

    def _set(self, playlist: dict | None) -> None:
        if playlist is None:
            delete(self.project, self.pid)
        else:
            put(self.project, playlist)

    def redo(self) -> None:
        self._set(self.new)

    def undo(self) -> None:
        self._set(self.old)
