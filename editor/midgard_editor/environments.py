# SPDX-License-Identifier: Apache-2.0
"""The project's environments (roadmap §6; milestone 5.8): how a world looks through its day.

An environment is a dict with "id", "name", "curves" ({"<curve>": [[phase, value], ...]}, values
numbers or [r, g, b]) and "settings" (sun_size, glow_tightness, fog_height_falloff,
fog_sun_scatter), as the environment table (schema/migrations/0010_environment.sql) holds them.
Curves that aren't set come from the engine's analytic sky, so an empty environment is a full
day already. The scene's Sky entity links to one; the engine (midgard/environment.h) works out
the look at each moment.
"""
import json
from typing import Any

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
