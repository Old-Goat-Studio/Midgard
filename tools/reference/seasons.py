# SPDX-License-Identifier: Apache-2.0
"""Seasons' colours (milestone 5.8): how far into summer a day is, at each place, and the
environment's season tints at that.

  calendar.summer_on   1 on the longest day, -1 on the shortest, on the calendar's daylight blend
  light.local_season   the calendar's everywhere, except on a tilted planet in "latitude" mode
  environment.season_tint
                       white blended towards the winter or summer tint

make_table() writes the shared table schema/tests/seasons.json (made once, then fixed).
"""
import json
from pathlib import Path

from . import calendar, environment, light

TABLES = Path(__file__).resolve().parents[2] / "schema" / "tests"


def make_table() -> dict:
    calendars = json.loads((TABLES / "calendar_dates.json").read_text("utf-8"))["calendars"]
    summer = []
    for name, c in calendars.items():
        days = calendar.year_days(c, 1)
        for day in sorted({0, 1, days // 4, days // 3, days // 2, (3 * days) // 4, days - 1}):
            summer.append({"calendar": name, "year": 1, "day_of_year": day,
                           "summer": calendar.summer_on(c, 1, day)})

    planet = {"shape": "sphere_outside", "origin": (0.0, -1000.0, 0.0)}
    bodies = {"flat": None, "planet": planet,
              "hollow": {"shape": "sphere_inside", "origin": (0.0, 1000.0, 0.0)}}
    placed = light.norm((0.3, 0.75, -0.6))
    points = [(0.0, 0.0, 0.0), (1000.0, -1000.0, 0.0), (0.0, -2000.0, 0.0),
              (0.0, -1000.0, 1000.0), (0.0, -1000.0, -1000.0), (300.0, -150.0, 400.0),
              (-600.0, -400.0, 690.0)]
    places = []
    for name, body in bodies.items():
        for mode, tilt in (("uniform", 23.5), ("latitude", 23.5), ("latitude", 0.0)):
            for year in (0.0, 0.25, 0.6, 0.75):
                clock = {"time": 0.5, "daylight": 0.5, "noon": 0.5, "mode": mode, "tilt": tilt,
                         "year": year, "season": -0.4}
                for p in points if body is planet else points[:1]:
                    places.append({"body": name, "clock": clock, "point": list(p),
                                   "season": light.local_season(body, placed, clock, p)})

    settings = {"winter": {"sunlight": [0.8, 0.9, 1.2], "sky": [0.85, 0.9, 1.1]},
                "summer": {"sunlight": [1.1, 1.0, 0.85], "ambient": [1.05, 1.0, 0.9]}}
    tints = [{"season": s, "tint": environment.season_tint(settings, s)}
             for s in (-1.5, -1.0, -0.75, -0.5, -0.1, 0.0, 0.1, 0.5, 0.75, 1.0, 1.5)]
    return {"_about": "Seasons' colours (tools/reference/seasons.py).",
            "summer": summer, "placed": list(placed),
            "bodies": {k: (None if v is None else {"shape": v["shape"],
                                                    "origin": list(v["origin"])})
                       for k, v in bodies.items()},
            "cases": places, "settings": settings, "tints": tints}


if __name__ == "__main__":
    out = TABLES / "seasons.json"
    out.write_text(json.dumps(make_table(), indent=1) + "\n", encoding="utf-8", newline="\n")
    print("wrote", out)
