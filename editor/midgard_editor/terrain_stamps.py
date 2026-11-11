# SPDX-License-Identifier: Apache-2.0
"""Terrain stamps (milestone 5.9): a scene's sculpting and painting, one brush stroke each,
applied in order on top of the generated ground (the terrain_stamp table, migration 11; the
engine's midgard/terrain.h has the maths). Every change is undoable, and the viewport and the
running game follow."""
import copy
import json
from typing import Any

from PySide6.QtGui import QUndoCommand

from .project import Project

TOOLS = {  # tool: (label, what strength means, default strength, its range)
    "raise": ("Raise", "metres", 2.0, (0.0, 1000.0)),
    "lower": ("Lower", "metres", 2.0, (0.0, 1000.0)),
    "smooth": ("Smooth", "0 to 1", 0.6, (0.0, 1.0)),
    "flatten": ("Flatten", "0 to 1", 0.8, (0.0, 1.0)),
    "paint": ("Paint", "0 to 1", 0.8, (0.0, 1.0)),
}
PAINTS = {"lush": "Lush grass", "dry": "Dry grass", "sand": "Sand", "rock": "Rock",
          "paint_1": "Paint layer 1", "paint_2": "Paint layer 2", "clear": "Clear paint"}
# (paint_1 and paint_2: the world type's own paint layers, 6.0b: named and textured on its
# Ground Textures tab; world_types.paint_layer_names says what it calls them.)


def load(project: Project, scene: str | None = None) -> list[tuple[int, dict[str, Any]]]:
    """The scene's stamps in order: (row id, stamp)."""
    scene = scene or project.scene
    with project.connect() as con:
        rows = con.execute("SELECT id, stamp FROM terrain_stamp WHERE scene = ? ORDER BY id",
                           (scene,)).fetchall()
    return [(int(i), json.loads(s)) for i, s in rows]


def stamps(project: Project, scene: str | None = None) -> list[dict[str, Any]]:
    scene = scene or project.scene
    return [s for _i, s in load(project, scene)]


def _insert(project: Project, scene: str, rows: list[tuple[int | None, dict]]) -> list[int]:
    ids = []
    with project.connect() as con:
        for row_id, stamp in rows:
            cur = con.execute("INSERT INTO terrain_stamp (id, scene, stamp) VALUES (?, ?, ?)",
                              (row_id, scene, json.dumps(stamp)))
            ids.append(int(cur.lastrowid))
    project.terrain_changed.emit(scene)
    return ids


def _delete(project: Project, scene: str, ids: list[int]) -> None:
    with project.connect() as con:
        con.executemany("DELETE FROM terrain_stamp WHERE id = ?", [(i,) for i in ids])
    project.terrain_changed.emit(scene)


def describe(stamp: dict[str, Any]) -> str:
    tool = stamp.get("tool", "raise")
    if tool == "paint":
        return f"paint {PAINTS.get(stamp.get('paint', 'lush'), '').lower()}"
    return TOOLS.get(tool, (tool,))[0].lower()


class StampCommand(QUndoCommand):
    """Adds one stroke (redo puts it back with the same id, so the order stays)."""

    def __init__(self, project: Project, stamp: dict[str, Any], scene: str | None = None):
        super().__init__(f"Terrain: {describe(stamp)}")
        self.project, self.scene = project, scene or project.scene
        self.stamp = copy.deepcopy(stamp)
        self.row_id: int | None = None

    def redo(self) -> None:
        self.row_id = _insert(self.project, self.scene, [(self.row_id, self.stamp)])[0]

    def undo(self) -> None:
        if self.row_id is not None:
            _delete(self.project, self.scene, [self.row_id])


class ClearStampsCommand(QUndoCommand):
    """Removes every stroke from the scene's terrain."""

    def __init__(self, project: Project, scene: str | None = None):
        super().__init__("Terrain: clear all sculpting and painting")
        self.project, self.scene = project, scene or project.scene
        self.rows = load(project, scene)

    def redo(self) -> None:
        _delete(self.project, self.scene, [i for i, _s in self.rows])

    def undo(self) -> None:
        _insert(self.project, self.scene, [(i, s) for i, s in self.rows])
