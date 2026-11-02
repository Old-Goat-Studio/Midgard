# SPDX-License-Identifier: Apache-2.0
"""A game's scenes (milestone 5.11a): the play spaces a game moves between (a town, a cave, a
ship's deck, another world), each with its own entities, settings (sky, world shape, terrain)
and terrain stamps. The game starts in the starting scene ({project}.config "start_scene";
the main scene if none is set) and changes scene through Scene Exits and scripts.

Every change here is an undoable command on the project's undo stack."""
import json
import re
import sqlite3
from typing import Any

from PySide6.QtGui import QUndoCommand

from .project import MAIN_SCENE, Project
from .scene_model import DEFAULT_SETTINGS, default_entities

START_KEY = "start_scene"


class SceneError(Exception):
    """A scene change the project refuses (deleting the starting scene or the last one)."""


def load_all(project: Project) -> list[dict[str, Any]]:
    """Every scene: {"id", "name"}, the starting scene first, then by name."""
    with project.connect() as con:
        rows = con.execute("SELECT scene.id, COALESCE(string.english, scene.id) FROM scene "
                           "LEFT JOIN string ON string.key = scene.name_key").fetchall()
    start = start_scene(project)
    rows.sort(key=lambda r: (r[0] != start, r[1].lower(), r[0]))
    return [{"id": r[0], "name": r[1]} for r in rows]


def name(project: Project, scene: str) -> str:
    return next((s["name"] for s in load_all(project) if s["id"] == scene), scene)


def start_scene(project: Project) -> str:
    """The scene the game starts in."""
    chosen = project.config.get(START_KEY)
    if chosen:
        with project.connect() as con:
            if con.execute("SELECT 1 FROM scene WHERE id = ?", (chosen,)).fetchone():
                return chosen
    return MAIN_SCENE


def new_id(project: Project, scene_name: str) -> str:
    base = "scn_" + (re.sub(r"[^a-z0-9]+", "_", scene_name.lower()).strip("_") or "scene")
    with project.connect() as con:
        taken = {r[0] for r in con.execute("SELECT id FROM scene")}
    candidate, n = base, 2
    while candidate in taken:
        candidate, n = f"{base}_{n}", n + 1
    return candidate


# ---- a scene's rows, kept by the commands to put it back -------------------------------------
def _snapshot(con: sqlite3.Connection, scene: str) -> dict[str, Any]:
    row = con.execute("SELECT name_key, settings FROM scene WHERE id = ?", (scene,)).fetchone()
    if row is None:
        raise SceneError(f"There is no scene {scene}.")
    name_row = con.execute("SELECT english, context FROM string WHERE key = ?",
                           (row[0],)).fetchone()
    return {"id": scene, "name_key": row[0], "settings": row[1],
            "name": name_row[0] if name_row else scene,
            "context": name_row[1] if name_row else "Scene name",
            "entities": con.execute("SELECT id, name, parent, components FROM entity "
                                    "WHERE scene = ? ORDER BY id", (scene,)).fetchall(),
            "stamps": con.execute("SELECT id, stamp FROM terrain_stamp WHERE scene = ? "
                                  "ORDER BY id", (scene,)).fetchall()}


def _restore(con: sqlite3.Connection, snap: dict[str, Any], keep_stamp_ids: bool = True) -> None:
    con.execute("INSERT OR REPLACE INTO string (key, english, context) VALUES (?, ?, ?)",
                (snap["name_key"], snap["name"], snap["context"]))
    con.execute("INSERT INTO scene (id, name_key, settings) VALUES (?, ?, ?)",
                (snap["id"], snap["name_key"], snap["settings"]))
    # Parents before children (a parent's id may sort after its child's).
    pending = list(snap["entities"])
    placed: set[str] = set()
    while pending:
        ready = [e for e in pending if e[2] is None or e[2] in placed]
        if not ready:  # (a broken parent link: put the rest in anyway)
            ready = pending
        for e in ready:
            con.execute("INSERT INTO entity (id, scene, name, parent, components) "
                        "VALUES (?, ?, ?, ?, ?)", (e[0], snap["id"], e[1], e[2], e[3]))
            placed.add(e[0])
        pending = [e for e in pending if e[0] not in placed]
    for stamp_id, stamp in snap["stamps"]:
        if keep_stamp_ids:
            con.execute("INSERT INTO terrain_stamp (id, scene, stamp) VALUES (?, ?, ?)",
                        (stamp_id, snap["id"], stamp))
        else:
            con.execute("INSERT INTO terrain_stamp (scene, stamp) VALUES (?, ?)",
                        (snap["id"], stamp))


def _delete(con: sqlite3.Connection, scene: str, name_key: str) -> None:
    con.execute("DELETE FROM scene WHERE id = ?", (scene,))  # entities and stamps go with it
    con.execute("DELETE FROM string WHERE key = ?", (name_key,))


def _gone(project: Project, scene: str) -> None:
    """A scene was deleted (or its making undone): if the editor had it open, it opens the
    starting scene instead."""
    if project.scene == scene:
        project.open_scene(start_scene(project))


def _fresh_ids(project: Project, entities: list) -> dict[str, str]:
    """New entity ids for entities copied into another scene (ids are the project's, not the
    scene's), in the same order."""
    first = int(project.next_entity_id()[1:])
    return {e[0]: f"e{first + i:06d}" for i, e in enumerate(entities)}


class AddSceneCommand(QUndoCommand):
    """A new scene: the default one (a camera, sun, ground, player and sky), or a copy of
    another scene (its entities with new ids, settings and terrain stamps)."""

    def __init__(self, project: Project, scene_name: str, copy_of: str | None = None):
        super().__init__(f"Duplicate scene {scene_name}" if copy_of else
                         f"New scene {scene_name}")
        self.project = project
        self.scene = new_id(project, scene_name)
        if copy_of is None:
            entities = [(e.id, e.name, e.parent, json.dumps(e.components))
                        for e in default_entities()]
            settings, stamps = json.dumps(DEFAULT_SETTINGS), []
        else:
            with project.connect() as con:
                source = _snapshot(con, copy_of)
            entities, settings, stamps = source["entities"], source["settings"], source["stamps"]
        ids = _fresh_ids(project, entities)
        self.snap = {"id": self.scene, "name_key": f"{self.scene}.name", "name": scene_name,
                     "context": "Scene name", "settings": settings, "stamps": stamps,
                     "entities": [(ids[e[0]], e[1], ids.get(e[2]) if e[2] else None, e[3])
                                  for e in entities]}

    def redo(self) -> None:
        with self.project.connect() as con:
            _restore(con, self.snap, keep_stamp_ids=False)
        self.project.scenes_changed.emit(self.scene)

    def undo(self) -> None:
        with self.project.connect() as con:
            _delete(con, self.scene, self.snap["name_key"])
        _gone(self.project, self.scene)
        self.project.scenes_changed.emit(self.scene)


class RenameSceneCommand(QUndoCommand):
    def __init__(self, project: Project, scene: str, new_name: str):
        super().__init__(f"Rename scene to {new_name}")
        self.project, self.scene, self.new = project, scene, new_name
        self.old = name(project, scene)
        with project.connect() as con:
            self.key = con.execute("SELECT name_key FROM scene WHERE id = ?",
                                   (scene,)).fetchone()[0]

    def _set(self, text: str) -> None:
        self.project.set_string(self.key, text, "Scene name")
        self.project.scenes_changed.emit(self.scene)

    def redo(self) -> None:
        self._set(self.new)

    def undo(self) -> None:
        self._set(self.old)


class DeleteSceneCommand(QUndoCommand):
    """Delete a scene with everything in it. Not the starting scene, nor the last one."""

    def __init__(self, project: Project, scene: str):
        super().__init__(f"Delete scene {name(project, scene)}")
        self.project, self.scene = project, scene
        if scene == start_scene(project):
            raise SceneError("The game starts in this scene: make another the starting scene "
                             "first.")
        if len(load_all(project)) <= 1:
            raise SceneError("A game needs a scene.")
        with project.connect() as con:
            self.snap = _snapshot(con, scene)

    def redo(self) -> None:
        with self.project.connect() as con:
            _delete(con, self.scene, self.snap["name_key"])
        _gone(self.project, self.scene)
        self.project.scenes_changed.emit(self.scene)

    def undo(self) -> None:
        with self.project.connect() as con:
            _restore(con, self.snap)
        self.project.scenes_changed.emit(self.scene)


class SetStartSceneCommand(QUndoCommand):
    """The scene the game starts in."""

    def __init__(self, project: Project, scene: str):
        super().__init__(f"Start the game in {name(project, scene)}")
        self.project, self.new = project, scene
        self.old = project.config.get(START_KEY)

    def _set(self, value: str | None) -> None:
        self.project.set_config(START_KEY, value)
        self.project.scenes_changed.emit(value or MAIN_SCENE)

    def redo(self) -> None:
        self._set(self.new)

    def undo(self) -> None:
        self._set(self.old)
