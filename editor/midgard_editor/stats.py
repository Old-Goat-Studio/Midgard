# SPDX-License-Identifier: Apache-2.0
"""The player's stats, scores and counters (milestone 6.0): numbers the designer names.

A stat is a dict with "id", "name" and "stat" (as the stat table holds it, in its "position"
order; schema/migrations/0019_stat.sql):

    {"kind": "stat", "min": 0, "max": 100, "start": 100, "regen": 0, "delay": 0,
     "hud": true}                      (false: kept off the HUD)

    stat     between its least and most (health, stamina); refills by itself, "regen" a
             second, once it has gone "delay" seconds without dropping (negative: drains)
    score    a whole number, never below its least (points)
    counter  a whole number up to a target ("max"; 0: none) (keys, coins)

and two that only show on the HUD, with no value (scripts can't read or change them):

    position where the player is: latitude and longitude on a planet or in a hollow world,
             else metres east and north of the world's middle      {"kind": "position"}
    clock    the date and time on the game's calendar, the parts "show" names
             {"kind": "clock", "show": ["season", "day", "month", "year", "time"]}

A project starts with Health, Stamina and Score. Scripts use them as midgard.stats (by id or
name), the HUD shows them, and the engine reads them in midgard/stats.h.
"""
import copy
import json
import math
import re
from pathlib import Path
from typing import Any

from PySide6.QtGui import QUndoCommand

from .project import Project

KINDS = {"stat": "Stat", "score": "Score", "counter": "Counter", "position": "Position",
         "clock": "Date and time"}
# Kinds that only show on the HUD, with no value.
DISPLAY = {"position", "clock"}
# What a clock can show, in the order it shows them (midgard/stats.h's clock_parts).
CLOCK_PARTS = {"season": "Season", "weekday": "Weekday", "day": "Day", "month": "Month",
               "year": "Year", "time": "Time (hour and minute)"}
# What each kind starts as when it is made in the Stats panel.
NEW = {
    "stat": {"kind": "stat", "min": 0, "max": 100, "start": 100, "regen": 0, "delay": 0},
    "score": {"kind": "score", "min": 0, "start": 0},
    "counter": {"kind": "counter", "min": 0, "max": 0, "start": 0},
    "position": {"kind": "position"},
    "clock": {"kind": "clock", "show": ["season", "day", "month", "year", "time"]},
}


def clock_shows(stat: dict[str, Any]) -> list[str]:
    """The parts a clock shows, in their order (as the engine reads them)."""
    show = stat.get("show", NEW["clock"]["show"])
    return [p for p in CLOCK_PARTS if isinstance(show, list) and p in show]


def has_table(project: Project) -> bool:
    with project.connect() as con:
        return con.execute("SELECT 1 FROM sqlite_master WHERE name = 'stat'").fetchone() is not None


def load_all(project: Project) -> list[dict[str, Any]]:
    """Every stat, in the Stats panel's order (none before migration 19)."""
    if not has_table(project):
        return []
    with project.connect() as con:
        rows = con.execute("SELECT id, name, stat FROM stat ORDER BY position, name")
        return [{"id": i, "name": n, "stat": json.loads(s)} for i, n, s in rows]


def load(project: Project, stat_id: str) -> dict[str, Any] | None:
    return next((s for s in load_all(project) if s["id"] == stat_id), None)


def save_all(project: Project, all_stats: list[dict[str, Any]]) -> None:
    """The stats, exactly these, in this order."""
    with project.connect() as con:
        con.execute("DELETE FROM stat")
        con.executemany("INSERT INTO stat (id, name, position, stat) VALUES (?, ?, ?, ?)",
                        [(s["id"], s["name"], i, json.dumps(s["stat"]))
                         for i, s in enumerate(all_stats)])
    project.stats_changed.emit("")


def new_id(existing: list[dict[str, Any]], name: str) -> str:
    """An id for a new stat named `name`, unlike the others."""
    stem = re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_") or "stat"
    taken = {s["id"] for s in existing}
    sid, n = stem, 2
    while sid in taken:
        sid, n = f"{stem}_{n}", n + 1
    return sid


def number(stat: dict[str, Any], key: str) -> float:
    """A stat's number, with its kind's default when it has none (as the engine reads it)."""
    kind = stat.get("kind", "stat")
    defaults = {"min": 0.0, "max": 100.0 if kind == "stat" else 0.0, "regen": 0.0, "delay": 0.0}
    if key == "start":
        return float(stat.get("start", number(stat, "max") if kind == "stat"
                              else number(stat, "min")))
    return float(stat.get(key, defaults[key]))


def problems_of(stat: Any) -> list[str]:
    """What's wrong with a stat's settings, in plain words."""
    if not isinstance(stat, dict):
        return ["isn't a set of settings"]
    found = []
    kind = stat.get("kind", "stat")
    if kind not in KINDS:
        return [f"has an unknown kind '{kind}'"]
    if "hud" in stat and not isinstance(stat["hud"], bool):
        found.append("'hud' isn't true or false")
    if kind in DISPLAY:
        show = stat.get("show", [])
        if kind == "clock" and not isinstance(show, list):
            found.append("'show' isn't a list of parts")
        elif kind == "clock":
            found += [f"shows '{p}', which a clock can't (season, weekday, day, month, year "
                      "or time)" for p in show if not isinstance(p, str) or p not in CLOCK_PARTS]
            if not show and "show" in stat:
                found.append("shows nothing (tick at least one part)")
        return found
    for key in ("min", "max", "start", "regen", "delay"):
        value = stat.get(key, 0)
        if isinstance(value, bool) or not isinstance(value, int | float) or \
                not math.isfinite(value):
            found.append(f"'{key}' isn't a number")
    if found:
        return found
    low, high, start = number(stat, "min"), number(stat, "max"), number(stat, "start")
    if kind == "stat" and high <= low:
        found.append(f"its most ({high:g}) isn't above its least ({low:g})")
    elif start < low or (kind == "stat" or (kind == "counter" and high > low)) and start > high:
        found.append(f"starts at {start:g}, outside {low:g} to {high:g}")
    if kind == "stat" and number(stat, "delay") < 0:
        found.append("waits less than no time to refill")
    return found


def problems(project: Project) -> list[str]:
    """The project's stats' problems, and names given twice (a script couldn't tell them
    apart)."""
    all_stats = load_all(project)
    found = [f"Stat '{s['name']}' {p}." for s in all_stats for p in problems_of(s["stat"])]
    seen: dict[str, str] = {}
    for s in all_stats:
        key = s["name"].strip().lower()
        if key in seen:
            found.append(f"Two stats are called '{s['name']}'; scripts can only tell them apart "
                         "by their ids.")
        seen[key] = s["id"]
    return found


# midgard.stats.get("health"), .set("Score", 1), .add(...), stats["keys"]: the stat named.
_SCRIPT_USE = re.compile(
    r"""stats\s*(?:\.\s*(?:get|set|add|max|min|set_max|reset|name|kind)\s*\(\s*|\[\s*)"""
    r"""(['"])([^'"\n]+)\1""")


def script_problems(project: Project, scripts: dict[str, Path]) -> list[str]:
    """Stats the scripts name (in quotes) that the project doesn't have: the script would
    stop there with a KeyError."""
    known = {s["id"] for s in load_all(project)} | {s["name"].lower()
                                                    for s in load_all(project)}
    found = []
    for module, file in sorted(scripts.items()):
        try:
            text = file.read_text("utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        for line_no, line in enumerate(text.splitlines(), 1):
            for m in _SCRIPT_USE.finditer(line):
                name = m.group(2)
                if name not in known and name.lower() not in known:
                    found.append(f"Script '{module}' (line {line_no}) uses stat '{name}', which "
                                 "the project doesn't have (see the Stats panel).")
    return found


def live_message(project: Project) -> list[dict[str, Any]]:
    """The stats for the running game (the live link's stats.put)."""
    return [{"id": s["id"], "name": s["name"], "stat": s["stat"]} for s in load_all(project)]


class SetStatsCommand(QUndoCommand):
    """The Stats panel's edits: every stat, before and after (they are few). Edits of one
    stat's field merge while typing."""

    ID = 0x5747

    def __init__(self, project: Project, old: list[dict], new: list[dict], label: str,
                 merge_key: str = ""):
        super().__init__(label)
        self.project = project
        self.old, self.new = copy.deepcopy(old), copy.deepcopy(new)
        self.merge_key = merge_key

    def id(self) -> int:
        return self.ID

    def mergeWith(self, other: QUndoCommand) -> bool:
        if not isinstance(other, SetStatsCommand) or not self.merge_key or \
                other.merge_key != self.merge_key:
            return False
        self.new = copy.deepcopy(other.new)
        return True

    def redo(self) -> None:
        save_all(self.project, self.new)

    def undo(self) -> None:
        save_all(self.project, self.old)
