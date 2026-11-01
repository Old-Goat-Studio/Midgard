# SPDX-License-Identifier: Apache-2.0
"""World types (milestone 5.11; migration 12, midgard/world_type.h): recipes for worlds, kept in
versions. A scene's terrain names a world type's version and a seed:

    "terrain": {"world_type": "wt_...", "version": 2, "seed": 7}

Every version a shipped game could use is kept unchanged (an export marks the versions its scenes
use as shipped), so a world still generates the same after its world type changes: saving over
a shipped version saves a new one instead. Unshipped versions are changed in place."""
import copy
import json
import re
from typing import Any

from PySide6.QtGui import QUndoCommand

from .project import MAIN_SCENE, Project


def load_all(project: Project) -> list[dict[str, Any]]:
    """Every world type: {"id", "name", "version"} (the latest), by name."""
    with project.connect() as con:
        rows = con.execute("SELECT id, name, version FROM world_type ORDER BY name, id").fetchall()
    return [{"id": r[0], "name": r[1], "version": r[2]} for r in rows]


def recipe(project: Project, world_type: str, version: int | None = None) -> dict[str, Any] | None:
    """A version's recipe (the latest if none is given), or None if there is no such version."""
    with project.connect() as con:
        if version is None:
            row = con.execute("SELECT version FROM world_type WHERE id = ?",
                              (world_type,)).fetchone()
            if row is None:
                return None
            version = row[0]
        row = con.execute("SELECT recipe FROM world_type_version WHERE world_type = ? AND "
                          "version = ?", (world_type, version)).fetchone()
    return None if row is None else json.loads(row[0])


def is_shipped(project: Project, world_type: str, version: int) -> bool:
    with project.connect() as con:
        row = con.execute("SELECT shipped FROM world_type_version WHERE world_type = ? AND "
                          "version = ?", (world_type, version)).fetchone()
    return bool(row and row[0])


def new_id(project: Project, name: str) -> str:
    base = "wt_" + (re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_") or "world")
    taken = {t["id"] for t in load_all(project)}
    candidate, n = base, 2
    while candidate in taken:
        candidate, n = f"{base}_{n}", n + 1
    return candidate


def resolved_settings(project: Project, settings: dict[str, Any]) -> dict[str, Any]:
    """Scene settings with the terrain's world type looked up: the recipe's terrain settings and
    the world's seed, as the engine's viewport and checks take them (they have no database)."""
    terrain = settings.get("terrain")
    if not isinstance(terrain, dict) or "world_type" not in terrain:
        return settings
    out = copy.deepcopy(settings)
    found = recipe(project, terrain["world_type"], terrain.get("version", 1))
    if found is None:
        out.pop("terrain")  # (the cook names it)
        return out
    out["terrain"] = dict(found.get("terrain", {}))
    if found.get("graph"):
        out["terrain"]["graph"] = found["graph"]  # how the ground's height is made
    if "seed" in terrain:
        out["terrain"]["seed"] = terrain["seed"]
    return out


def used_versions(project: Project) -> set[tuple[str, int]]:
    """The world type versions the project's scenes are made with."""
    with project.connect() as con:
        rows = con.execute("SELECT settings FROM scene").fetchall()
    out = set()
    for (text,) in rows:
        terrain = json.loads(text).get("terrain")
        if isinstance(terrain, dict) and "world_type" in terrain:
            out.add((terrain["world_type"], int(terrain.get("version", 1))))
    return out


def mark_shipped(project: Project) -> None:
    """An export: the versions its scenes use are shipped, kept unchanged from now on."""
    with project.connect() as con:
        for world_type, version in used_versions(project):
            con.execute("UPDATE world_type_version SET shipped = 1 WHERE world_type = ? AND "
                        "version = ?", (world_type, version))


class SaveRecipeCommand(QUndoCommand):
    """Save a world type's recipe: over its latest version, or as a new one if that version has
    shipped. `self.version` is the version saved. Undo puts back what was there."""

    def __init__(self, project: Project, world_type: str, new_recipe: dict[str, Any],
                 name: str | None = None):
        super().__init__("Change world type")
        self.project, self.world_type = project, world_type
        self.recipe = copy.deepcopy(new_recipe)
        with project.connect() as con:
            row = con.execute("SELECT name, version FROM world_type WHERE id = ?",
                              (world_type,)).fetchone()
        self.created = row is None
        self.old_name = None if row is None else row[0]
        self.name = name if name is not None else (self.old_name or world_type)
        self.old_version = None if row is None else row[1]
        self.old_recipe = None if row is None else recipe(project, world_type, row[1])
        if row is None:
            self.version = 1
        elif is_shipped(project, world_type, row[1]):
            self.version = row[1] + 1
        else:
            self.version = row[1]

    def redo(self) -> None:
        with self.project.connect() as con:
            if self.created:
                con.execute("INSERT INTO world_type (id, name, version) VALUES (?, ?, ?)",
                            (self.world_type, self.name, self.version))
            else:
                con.execute("UPDATE world_type SET name = ?, version = ? WHERE id = ?",
                            (self.name, self.version, self.world_type))
            con.execute("INSERT INTO world_type_version (world_type, version, recipe) "
                        "VALUES (?, ?, ?) ON CONFLICT (world_type, version) DO UPDATE SET "
                        "recipe = excluded.recipe",
                        (self.world_type, self.version, json.dumps(self.recipe)))
        self.project.world_types_changed.emit(self.world_type)

    def undo(self) -> None:
        with self.project.connect() as con:
            if self.created:
                con.execute("DELETE FROM world_type WHERE id = ?", (self.world_type,))
            else:
                if self.version != self.old_version:  # a new version: gone again
                    con.execute("DELETE FROM world_type_version WHERE world_type = ? AND "
                                "version = ?", (self.world_type, self.version))
                else:
                    con.execute("UPDATE world_type_version SET recipe = ? WHERE world_type = ? "
                                "AND version = ?", (json.dumps(self.old_recipe),
                                                    self.world_type, self.version))
                con.execute("UPDATE world_type SET name = ?, version = ? WHERE id = ?",
                            (self.old_name, self.old_version, self.world_type))
        self.project.world_types_changed.emit(self.world_type)


def scene_world(project: Project, scene: str = MAIN_SCENE) -> dict[str, Any] | None:
    """The scene's world: {"world_type", "version", "seed"?}, or None (no terrain, or terrain
    settings from before world types)."""
    terrain = project.scene_settings(scene).get("terrain")
    if isinstance(terrain, dict) and "world_type" in terrain:
        return terrain
    return None
