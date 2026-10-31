# SPDX-License-Identifier: Apache-2.0
"""The project's calendars (roadmap §6; milestone 5.7), as the Calendar dock edits them.

A calendar is a dict: the calendar table's columns (schema/migrations/0009_calendar.sql) under
the same names, with "name" and "era" as English text, and lists "months" ([{"name", "days"}]),
"weekdays" (names), "seasons" ([{"name", "share", "daylight", "temp_min", "temp_max"}]) and
"moons" ([{"name", "period", "offset"}]). Names are kept in the string table under keys made from
the calendar's id ("<id>.month.0", ...), so they can be translated. The engine's own maths
(_midgard.Calendar) gives dates, daylight and the cook checks.
"""
import copy
import json
import re
from typing import Any

from PySide6.QtGui import QUndoCommand

from . import engine
from .project import Project

COLUMNS = ("hours_per_day", "minutes_per_hour", "leap_every", "leap_month", "start_year",
           "start_month", "start_day", "start_minute", "real_minutes_per_day", "daylight",
           "solar_noon", "twilight", "day_hours_min", "day_hours_max", "year_days_min",
           "year_days_max", "tilt_min", "tilt_max")


def _names(con, keys: list[str]) -> dict[str, str]:
    if not keys:
        return {}
    marks = ",".join("?" * len(keys))
    return dict(con.execute(f"SELECT key, english FROM string WHERE key IN ({marks})", keys))


def load_all(project: Project) -> list[dict[str, Any]]:
    """Every calendar, the standard one first."""
    out = []
    with project.connect() as con:
        cols = ", ".join(COLUMNS)
        rows = con.execute(f"SELECT id, name_key, standard, COALESCE(era_key, ''), {cols} "
                           "FROM calendar ORDER BY standard DESC, id").fetchall()
        for row in rows:
            cid, name_key, standard, era_key = row[:4]
            cal: dict[str, Any] = {"id": cid, "standard": bool(standard),
                                   **dict(zip(COLUMNS, row[4:], strict=True))}
            months = con.execute("SELECT name_key, days FROM calendar_month WHERE calendar = ? "
                                 "ORDER BY idx", (cid,)).fetchall()
            weekdays = [k for (k,) in con.execute(
                "SELECT name_key FROM calendar_weekday WHERE calendar = ? ORDER BY idx", (cid,))]
            seasons = con.execute("SELECT name_key, share, daylight, temp_min, temp_max FROM "
                                  "calendar_season WHERE calendar = ? ORDER BY idx",
                                  (cid,)).fetchall()
            moons = con.execute("SELECT name_key, period, offset FROM calendar_moon "
                                "WHERE calendar = ? ORDER BY idx", (cid,)).fetchall()
            keys = [name_key, era_key] + [m[0] for m in months] + weekdays + \
                [s[0] for s in seasons] + [m[0] for m in moons]
            names = _names(con, [k for k in keys if k])
            cal["name"] = names.get(name_key, cid)
            cal["era"] = names.get(era_key, "")
            cal["months"] = [{"name": names.get(k, ""), "days": d} for k, d in months]
            cal["weekdays"] = [names.get(k, "") for k in weekdays]
            cal["seasons"] = [{"name": names.get(k, ""), "share": sh, "daylight": dl,
                               "temp_min": lo, "temp_max": hi} for k, sh, dl, lo, hi in seasons]
            cal["moons"] = [{"name": names.get(k, ""), "period": p, "offset": o}
                            for k, p, o in moons]
            out.append(cal)
    return out


def load(project: Project, calendar_id: str) -> dict[str, Any] | None:
    return next((c for c in load_all(project) if c["id"] == calendar_id), None)


def put(project: Project, cal: dict[str, Any]) -> None:
    """Writes a calendar, its rows and its names (replacing what it had). Use CalendarCommand
    from the UI."""
    cid = cal["id"]
    strings: list[tuple[str, str, str]] = [(f"{cid}.name", cal.get("name") or cid, "Calendar")]
    era_key = None
    if cal.get("era"):
        era_key = f"{cid}.era"
        strings.append((era_key, cal["era"], "How a year is shown; {n} is its number"))
    with project.connect() as con:
        # Its old names go first (a renamed or shortened list leaves none behind).
        con.execute("DELETE FROM calendar WHERE id = ?", (cid,))
        con.execute("DELETE FROM string WHERE key LIKE ? ESCAPE '\\'",
                    (cid.replace("_", "\\_").replace("%", "\\%") + ".%",))
        rows = [(f"{cid}.month.{i}", m.get("name", ""), "Month")
                for i, m in enumerate(cal["months"])]
        rows += [(f"{cid}.weekday.{i}", w, "Weekday") for i, w in enumerate(cal["weekdays"])]
        rows += [(f"{cid}.season.{i}", s.get("name", ""), "Season")
                 for i, s in enumerate(cal["seasons"])]
        rows += [(f"{cid}.moon.{i}", m.get("name", ""), "Moon")
                 for i, m in enumerate(cal["moons"])]
        con.executemany("INSERT OR REPLACE INTO string (key, english, context) VALUES (?, ?, ?)",
                        strings + rows)
        cols = ", ".join(COLUMNS)
        marks = ", ".join("?" * len(COLUMNS))
        con.execute(f"INSERT INTO calendar (id, name_key, standard, era_key, {cols}) "
                    f"VALUES (?, ?, ?, ?, {marks})",
                    (cid, f"{cid}.name", int(bool(cal.get("standard"))), era_key,
                     *[cal[c] for c in COLUMNS]))
        con.executemany("INSERT INTO calendar_month (calendar, idx, name_key, days) "
                        "VALUES (?, ?, ?, ?)",
                        [(cid, i, f"{cid}.month.{i}", m["days"])
                         for i, m in enumerate(cal["months"])])
        con.executemany("INSERT INTO calendar_weekday (calendar, idx, name_key) VALUES (?, ?, ?)",
                        [(cid, i, f"{cid}.weekday.{i}") for i in range(len(cal["weekdays"]))])
        con.executemany("INSERT INTO calendar_season (calendar, idx, name_key, share, daylight, "
                        "temp_min, temp_max) VALUES (?, ?, ?, ?, ?, ?, ?)",
                        [(cid, i, f"{cid}.season.{i}", s["share"], s["daylight"], s["temp_min"],
                          s["temp_max"]) for i, s in enumerate(cal["seasons"])])
        con.executemany("INSERT INTO calendar_moon (calendar, idx, name_key, period, offset) "
                        "VALUES (?, ?, ?, ?, ?)",
                        [(cid, i, f"{cid}.moon.{i}", m["period"], m["offset"])
                         for i, m in enumerate(cal["moons"])])
    project.calendars_changed.emit(cid)


def delete(project: Project, calendar_id: str) -> None:
    with project.connect() as con:
        con.execute("DELETE FROM calendar WHERE id = ?", (calendar_id,))
        con.execute("DELETE FROM string WHERE key LIKE ? ESCAPE '\\'",
                    (calendar_id.replace("_", "\\_").replace("%", "\\%") + ".%",))
    project.calendars_changed.emit("")


def new_id(project: Project, name: str) -> str:
    """An id for a new calendar named `name`: cal_<name>, made unique."""
    stem = "cal_" + (re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_") or "calendar")
    taken = {c["id"] for c in load_all(project)}
    cid, n = stem, 2
    while cid in taken:
        cid, n = f"{stem}_{n}", n + 1
    return cid


def engine_json(cal: dict[str, Any]) -> str:
    """The calendar as _midgard.Calendar (and midgard/calendar.h) reads it."""
    return json.dumps({"id": cal["id"], "standard": int(bool(cal.get("standard"))),
                       **{c: cal[c] for c in COLUMNS},
                       "months": [{"days": m["days"]} for m in cal["months"]],
                       "weekdays": [""] * len(cal["weekdays"]),
                       "seasons": [{k: s[k] for k in ("share", "daylight", "temp_min", "temp_max")}
                                   for s in cal["seasons"]],
                       "moons": [{"period": m["period"], "offset": m["offset"]}
                                 for m in cal["moons"]]})


def engine_calendar(cal: dict[str, Any]):
    """The engine's calendar for this one, or None without the engine module."""
    module = engine.module()
    return module.Calendar(engine_json(cal)) if module is not None else None


def problems(calendars: list[dict[str, Any]]) -> list[str]:
    """The cook checks' complaints about a project's calendars (none without the engine)."""
    module = engine.module()
    if module is None:
        return []
    return list(module.calendar_problems([engine_json(c) for c in calendars]))


class CalendarCommand(QUndoCommand):
    """Adds (old None), changes or deletes (new None) a calendar. Changes to the same setting of
    the same calendar in a row (a number being dragged) merge into one step."""
    ID = 5701

    def __init__(self, project: Project, old: dict | None, new: dict | None, label: str,
                 field: str = "", parent: QUndoCommand | None = None):
        cal = new or old or {}
        verb = "Add" if old is None else "Delete" if new is None else "Change"
        super().__init__(f"{verb} calendar {cal.get('name', '')} {label}".strip(), parent)
        self.project = project
        self.old, self.new = copy.deepcopy(old), copy.deepcopy(new)
        self.cid, self.field = cal.get("id", ""), field

    def id(self) -> int:
        return self.ID

    def mergeWith(self, other: QUndoCommand) -> bool:
        if (not isinstance(other, CalendarCommand) or not self.field or other.cid != self.cid
                or other.field != self.field or self.new is None or other.new is None):
            return False
        self.new = other.new
        return True

    def _set(self, cal: dict | None) -> None:
        if cal is None:
            delete(self.project, self.cid)
        else:
            put(self.project, cal)

    def redo(self) -> None:
        self._set(self.new)

    def undo(self) -> None:
        self._set(self.old)


def make_standard(project: Project, calendar_id: str) -> None:
    """Makes a calendar the standard one, as one undo step (the old standard first gives it up:
    there is only ever one)."""
    calendars = {c["id"]: c for c in load_all(project)}
    if calendar_id not in calendars or calendars[calendar_id]["standard"]:
        return
    stack = project.undo_stack
    stack.beginMacro(f"Make {calendars[calendar_id]['name']} the standard calendar")
    for cal in calendars.values():
        if cal["standard"]:
            stack.push(CalendarCommand(project, cal, {**cal, "standard": False}, "(standard)"))
    chosen = calendars[calendar_id]
    stack.push(CalendarCommand(project, chosen, {**chosen, "standard": True}, "(standard)"))
    stack.endMacro()
