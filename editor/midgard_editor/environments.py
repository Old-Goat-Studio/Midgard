# SPDX-License-Identifier: Apache-2.0
"""The project's environments (roadmap §6; milestone 5.8): how a world looks through its day.

An environment is a dict with "id", "name", "curves" ({"<curve>": [[phase, value], ...]}, values
numbers or [r, g, b]) and "settings" (sun_size, glow_tightness, fog_height_falloff,
fog_sun_scatter), as the environment table (schema/migrations/0010_environment.sql) holds them.
Curves that aren't set come from the engine's analytic sky, so an empty environment is a full
day already. The scene's Sky entity links to one; the engine (midgard/environment.h) works out
the look at each moment.
"""
import copy
import json
import math
import re
from typing import Any

from PySide6.QtGui import QUndoCommand

from .project import Project


def load_all(project: Project) -> list[dict[str, Any]]:
    """Every environment, by id (none in a project from before migration 10)."""
    with project.connect() as con:
        if not con.execute("SELECT 1 FROM sqlite_master WHERE name = 'environment'").fetchone():
            return []
        rows = con.execute("SELECT id, name, curves, settings FROM environment ORDER BY id")
        return [{"id": i, "name": n, "curves": json.loads(c), "settings": json.loads(s)}
                for i, n, c, s in rows]


def load(project: Project, environment_id: str) -> dict[str, Any] | None:
    return next((e for e in load_all(project) if e["id"] == environment_id), None)


# ---- changing them ------------------------------------------------------------------------
GROUPS = {  # the curves, as the Environment editor lists them
    "Sky": ("zenith", "horizon", "horizon_away", "ground", "sky_brightness", "glow",
            "glow_brightness", "sun_disc", "sun_disc_brightness"),
    "Light": ("sunlight", "sunlight_intensity", "ambient", "ambient_ground"),
    "Fog": ("fog_density", "fog_color"),
}
LABELS = {
    "zenith": "Zenith", "horizon": "Horizon (sun's side)", "horizon_away": "Horizon (away)",
    "ground": "Below the horizon", "sky_brightness": "Sky brightness", "glow": "Sun's glow",
    "glow_brightness": "Glow brightness", "sun_disc": "Sun's disc",
    "sun_disc_brightness": "Disc brightness", "sunlight": "Sunlight colour",
    "sunlight_intensity": "Sunlight strength", "ambient": "Ambient light (sky)",
    "ambient_ground": "Ambient light (ground)", "fog_density": "Fog density",
    "fog_color": "Fog colour",
}
COLOURS = {"zenith", "horizon", "horizon_away", "ground", "glow", "sun_disc", "sunlight",
           "ambient", "ambient_ground", "fog_color"}
SETTINGS = {  # key: (label, default, low, high, step, decimals)
    "sun_size": ("Sun size (degrees)", 1.5, 0.1, 45.0, 0.1, 2),
    "glow_tightness": ("Glow tightness", 0.8, 0.0, 0.99, 0.01, 2),
    "fog_height_falloff": ("Fog thins upwards (per m)", 0.0, 0.0, 1.0, 0.001, 4),
    "fog_sun_scatter": ("Fog brighter towards the sun", 0.5, 0.0, 4.0, 0.05, 2),
}
# Inside worlds: no open sky overhead, but the far side of the world seen through haze, bluish by
# day and dark by night, the same at every height (roadmap §6).
INSIDE_ATMOSPHERE = {
    "curves": {
        "fog_density": [[0.0, 0.0012]],
        "fog_color": [[0.0, [0.55, 0.6, 0.66]], [0.25, [0.62, 0.7, 0.8]], [0.5, [0.55, 0.5, 0.5]],
                      [0.75, [0.05, 0.06, 0.09]]],
    },
    "settings": {"fog_height_falloff": 0.0, "fog_sun_scatter": 0.8},
}
SAMPLES = (0.0, 0.03, 0.08, 0.15, 0.25, 0.35, 0.42, 0.47, 0.5, 0.55, 0.65, 0.75, 0.9, 0.97)
NOON_ELEVATION = 50.0  # degrees: the sun's height at noon the editor assumes for its previews


def sun_elevation(phase: float) -> float:
    """The sun's height (radians) at a solar phase, for the editor's previews."""
    return math.asin(math.sin(math.radians(NOON_ELEVATION)) * math.sin(2 * math.pi * phase))


def put(project: Project, env: dict[str, Any]) -> None:
    with project.connect() as con:
        con.execute("INSERT INTO environment (id, name, curves, settings) VALUES (?, ?, ?, ?) "
                    "ON CONFLICT(id) DO UPDATE SET name = excluded.name, "
                    "curves = excluded.curves, settings = excluded.settings",
                    (env["id"], env["name"], json.dumps(env["curves"]),
                     json.dumps(env["settings"])))
    project.environments_changed.emit(env["id"])


def delete(project: Project, environment_id: str) -> None:
    with project.connect() as con:
        con.execute("DELETE FROM environment WHERE id = ?", (environment_id,))
    project.environments_changed.emit("")


def new_id(project: Project, name: str) -> str:
    """An id for a new environment named `name`: env_<name>, made unique."""
    stem = "env_" + (re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_") or "environment")
    taken = {e["id"] for e in load_all(project)}
    eid, n = stem, 2
    while eid in taken:
        eid, n = f"{stem}_{n}", n + 1
    return eid


class EnvironmentCommand(QUndoCommand):
    """Adds (old None), changes or deletes (new None) an environment. Changes to the same curve
    or setting of the same environment in a row (a key being dragged) merge into one step."""
    ID = 5801

    def __init__(self, project: Project, old: dict | None, new: dict | None, label: str,
                 field: str = ""):
        env = new or old or {}
        verb = "Add" if old is None else "Delete" if new is None else "Change"
        super().__init__(f"{verb} environment {env.get('name', '')} {label}".strip())
        self.project = project
        self.old, self.new = copy.deepcopy(old), copy.deepcopy(new)
        self.eid, self.field = env.get("id", ""), field

    def id(self) -> int:
        return self.ID

    def mergeWith(self, other: QUndoCommand) -> bool:
        if (not isinstance(other, EnvironmentCommand) or not self.field or other.eid != self.eid
                or other.field != self.field or self.new is None or other.new is None):
            return False
        self.new = other.new
        return True

    def _set(self, env: dict | None) -> None:
        if env is None:
            delete(self.project, self.eid)
        else:
            put(self.project, env)

    def redo(self) -> None:
        self._set(self.new)

    def undo(self) -> None:
        self._set(self.old)


def evaluate_curve(keys: list, phase: float):
    """A curve's value at a solar phase, as the engine works it out (midgard/environment.h;
    tools/reference/environment.py): eased between keys, round the day. None if not set."""
    if not keys:
        return None
    phase %= 1.0
    n = len(keys)
    if n == 1:
        return keys[0][1]
    i = n - 1
    for k in range(n):
        if keys[k][0] <= phase:
            i = k
    start = keys[i][0]
    if phase < start:
        phase += 1.0
    nxt = (i + 1) % n
    end = keys[nxt][0] + (1.0 if nxt <= i else 0.0)
    t = (phase - start) / (end - start)
    t = t * t * (3.0 - 2.0 * t)
    a, b = keys[i][1], keys[nxt][1]
    if isinstance(a, list):
        return [x + (y - x) * t for x, y in zip(a, b, strict=True)]
    return a + (b - a) * t


def analytic_value(name: str, phase: float):
    """What a curve gives when it isn't set: the engine's analytic sky at the editor's preview
    sun height (None for the ambient and fog colours, which are the scene's own; or without
    the engine module)."""
    from . import engine
    module = engine.module()
    if module is None or not hasattr(module, "Environment"):
        return None
    value = module.Environment().evaluate(phase, sun_elevation(phase))[name]
    return list(value) if isinstance(value, tuple) else value


def analytic_keys(name: str) -> list:
    """Keys that follow the analytic sky through the day (the Environment editor's "Start from
    the analytic sky")."""
    keys = []
    for phase in SAMPLES:
        value = analytic_value(name, phase)
        if value is None:
            return []
        value = [round(c, 4) for c in value] if isinstance(value, list) else round(value, 4)
        keys.append([phase, value])
    return keys
