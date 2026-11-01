# SPDX-License-Identifier: Apache-2.0
"""Light models per world shape (engine/scene/include/midgard/sunlight.h; roadmap §6, "Day and
night on each world shape"; milestone 5.8): where the sun is, seen from a point, and that
point's own solar phase (0 sunrise, 0.25 noon, 0.5 sunset, 0.75 midnight).

The clock gives the time of day at the scene's start (the world's origin) as a fraction of the
day; the scene's sun, as placed, is where it stands there at solar noon.

  bounded area, flat-wrap cylinder   one sun crossing the sky: the same time everywhere
  sphere outside, "uniform"          a distant sun and a turning planet: each place's time is
                                     shifted by its longitude, and every place has the calendar's
                                     daylight share, with the sun as high at noon as at the start
  sphere outside, "latitude"         a tilted planet: the sun's declination follows the year, day
                                     length follows latitude (polar day and night), and the
                                     start's latitude is set by how high the sun was placed
  sphere inside, curved cylinder     a central sun (or sun-line) with shades going round: each
                                     place's time is shifted by its angle round the axis, and the
                                     light is stylised (low from the side at dawn and dusk,
                                     overhead at noon)

Each place's season (local_season) is the calendar's everywhere, except on a tilted planet in
"latitude" mode: there it follows the sun's declination, opposite in the two hemispheres, none
at the equator and full from 45 degrees of latitude.

Vectors are tuples; all maths in doubles, in the same order as the engine.
"""
import math

TAU = 2.0 * math.pi


def add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def mul(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def norm(a):
    length = math.sqrt(dot(a, a))
    return mul(a, 1.0 / length)


def flat(v, up):
    """v made perpendicular to up (not normalised)."""
    return sub(v, mul(up, dot(v, up)))


def any_perpendicular(up):
    return norm(cross(up, (1.0, 0.0, 0.0) if abs(up[0]) < 0.9 else (0.0, 0.0, 1.0)))


def phase_of_time(f, daylight, noon):
    """The solar phase at a time of day (a fraction of the day): calendar.solar_phase."""
    d = min(max(daylight, 0.01), 0.99)
    t = (f - noon + d / 2.0) % 1.0
    if t < d:
        return 0.5 * t / d
    return 0.5 + 0.5 * (t - d) / (1.0 - d)


def time_of_phase(phase, daylight, noon):
    """The time of day (a fraction of the day) at a solar phase: phase_of_time undone."""
    d = min(max(daylight, 0.01), 0.99)
    phase %= 1.0
    t = phase / 0.5 * d if phase < 0.5 else d + (phase - 0.5) / 0.5 * (1.0 - d)
    return (t + noon - d / 2.0) % 1.0


def path(up, noon_dir, phase):
    """The sun's path through the day: rising at phase 0, at noon_dir at 0.25, setting at 0.5."""
    if dot(noon_dir, up) < 0.01:
        noon_dir = up  # placed below the horizon: overhead
    rising = cross(up, noon_dir)
    rising = norm(rising) if dot(rising, rising) >= 1e-16 else any_perpendicular(up)
    a = TAU * phase
    return add(mul(rising, math.cos(a)), mul(noon_dir, math.sin(a)))


def up_at(body, p):
    if body is None or body["shape"] in ("bounded_area", "flat_cylinder"):
        return (0.0, 1.0, 0.0)
    q = sub(p, body["origin"])
    if body["shape"] == "curved_cylinder":
        return norm((-q[0], -q[1], 0.0))  # towards the axis (along Z)
    out = norm(q)
    return out if body["shape"] == "sphere_outside" else mul(out, -1.0)


def local_sun(body, placed, clock, p):
    """(phase, to_sun, height) at p. placed: the direction towards the sun as placed (unit).
    clock: {"time": fraction of the day at the start, "daylight", "noon", "mode", "tilt"
    (degrees), "year" (fraction of the year since the spring equinox)}."""
    f, d, n = clock["time"], clock["daylight"], clock["noon"]
    up0 = up_at(body, (0.0, 0.0, 0.0))
    up_p = up_at(body, p)
    kind = body["shape"] if body is not None else "bounded_area"
    if kind in ("bounded_area", "flat_cylinder"):
        phase = phase_of_time(f, d, n)
        to_sun = path(up0, placed, phase)
        return phase, to_sun, dot(to_sun, up_p)

    if kind == "curved_cylinder":
        axis = (0.0, 0.0, 1.0)
        out0 = mul(up0, -1.0)
    else:
        # The start's frame: north points away from the sun at noon.
        h = flat(placed, up0)
        north0 = mul(norm(h), -1.0) if dot(h, h) > 1e-16 else any_perpendicular(up0)
        elev0 = math.asin(min(max(dot(placed, up0), -1.0), 1.0))
        elev0 = min(max(elev0, math.radians(1.0)), math.pi / 2)
        if kind == "sphere_outside":
            lat0 = math.pi / 2 - elev0  # how high the sun stands at noon sets the latitude
            axis = add(mul(north0, math.cos(lat0)), mul(up0, math.sin(lat0)))
            out0 = up0
        else:  # sphere inside: the shades go round an axis through the start's north
            axis = north0
            out0 = mul(up0, -1.0)
    e_y = norm(flat(out0, axis))   # outwards under the start, at right angles to the axis
    e_x = cross(axis, e_y)         # east: the way the day moves round
    q = norm(sub(p, body["origin"]))
    if kind == "curved_cylinder":
        q = norm((q[0], q[1], 0.0))
    longitude = math.atan2(dot(q, e_x), dot(q, e_y))

    if kind == "sphere_outside" and clock["mode"] == "latitude":
        tilt = math.radians(clock["tilt"])
        declination = tilt * math.sin(TAU * clock["year"])
        hour = TAU * (f - n)
        to_sun = add(mul(add(mul(e_y, math.cos(hour)), mul(e_x, -math.sin(hour))),
                         math.cos(declination)), mul(axis, math.sin(declination)))
        latitude = math.asin(min(max(dot(q, axis), -1.0), 1.0))
        c = -math.tan(latitude) * math.tan(declination)
        h0 = math.pi if c <= -1.0 else (0.0 if c >= 1.0 else math.acos(c))
        local_hour = (hour + longitude + math.pi) % TAU - math.pi
        if h0 > 0.0 and abs(local_hour) <= h0:
            phase = 0.25 + 0.25 * local_hour / h0
        else:
            night = TAU - 2.0 * h0
            since = (local_hour - h0) % TAU
            phase = 0.5 + 0.5 * since / night if night > 0.0 else 0.5
        return phase, to_sun, dot(to_sun, up_p)

    phase = phase_of_time(f + longitude / TAU, d, n)
    if kind == "sphere_outside":  # uniform: the start's noon height, everywhere
        north = flat(axis, up_p)
        north = norm(north) if dot(north, north) > 1e-16 else any_perpendicular(up_p)
        noon_dir = sub(mul(up_p, math.sin(elev0)), mul(north, math.cos(elev0)))
        to_sun = path(up_p, noon_dir, phase)
    else:
        # Inside: straight down from the light rod or sphere; the shades give each place's day
        # (the height of a stylised sun: low from the east at dawn, overhead at noon).
        east = cross(axis, q)
        east = norm(east) if dot(east, east) > 1e-16 else any_perpendicular(up_p)
        a = TAU * phase
        stylised = add(mul(east, math.cos(a)), mul(up_p, math.sin(a)))
        return phase, up_p, dot(stylised, up_p)
    return phase, to_sun, dot(to_sun, up_p)


def local_season(body, placed, clock, p) -> float:
    """How far into summer it is at p: 1 summer, -1 winter. clock["season"] is the calendar's
    (calendar.summer_on) at the start."""
    season = clock.get("season", 0.0)
    if (body is None or body["shape"] != "sphere_outside" or clock["mode"] != "latitude"
            or clock["tilt"] <= 0.0):
        return season
    up0 = up_at(body, (0.0, 0.0, 0.0))
    h = flat(placed, up0)
    north0 = mul(norm(h), -1.0) if dot(h, h) > 1e-16 else any_perpendicular(up0)
    elev0 = math.asin(min(max(dot(placed, up0), -1.0), 1.0))
    elev0 = min(max(elev0, math.radians(1.0)), math.pi / 2)
    lat0 = math.pi / 2 - elev0
    axis = add(mul(north0, math.cos(lat0)), mul(up0, math.sin(lat0)))
    q = norm(sub(p, body["origin"]))
    latitude = math.asin(min(max(dot(q, axis), -1.0), 1.0))
    return math.sin(TAU * clock["year"]) * min(max(latitude / (math.pi / 4), -1.0), 1.0)


def make_table() -> dict:
    """The shared table schema/tests/light_models.json (made once, then fixed)."""
    bodies = {
        "flat": None,
        "area": {"shape": "bounded_area", "origin": (0.0, 0.0, 0.0)},
        "planet": {"shape": "sphere_outside", "origin": (0.0, -1000.0, 0.0)},
        "hollow": {"shape": "sphere_inside", "origin": (0.0, 1000.0, 0.0)},
        "habitat": {"shape": "curved_cylinder", "origin": (0.0, 1000.0, 0.0)},
    }
    placed = norm((0.3, 0.75, -0.6))
    clocks = [
        {"time": t, "daylight": dl, "noon": 0.5, "mode": mode, "tilt": tilt, "year": year}
        for t in (0.0, 0.2, 0.37, 0.5, 0.64, 0.81)
        for dl, mode, tilt, year in ((0.5, "uniform", 0.0, 0.0), (0.7, "uniform", 0.0, 0.0),
                                     (0.6, "latitude", 23.5, 0.25), (0.6, "latitude", 23.5, 0.75))
    ]
    points = {
        "flat": [(0.0, 0.0, 0.0), (500.0, 3.0, -200.0)],
        "area": [(0.0, 0.0, 0.0), (500.0, 3.0, -200.0)],
        "planet": [(0.0, 0.0, 0.0), (1000.0, -1000.0, 0.0), (0.0, -2000.0, 0.0),
                   (0.0, -1000.0, 1000.0), (0.0, -1000.0, -1000.0), (300.0, -150.0, 400.0)],
        "hollow": [(0.0, 0.0, 0.0), (1000.0, 1000.0, 0.0), (0.0, 1000.0, 1000.0),
                   (-300.0, 200.0, 400.0)],
        "habitat": [(0.0, 0.0, 0.0), (1000.0, 1000.0, 50.0), (0.0, 2000.0, -20.0),
                    (-707.0, 293.0, 0.0)],
    }
    cases = []
    for name, body in bodies.items():
        for clock in clocks:
            if clock["mode"] == "latitude" and (body is None or
                                                body["shape"] != "sphere_outside"):
                continue
            for p in points[name]:
                phase, to_sun, height = local_sun(body, placed, clock, p)
                cases.append({"body": name, "clock": clock, "point": list(p), "phase": phase,
                              "to_sun": list(to_sun), "height": height})
    return {"_about": "Light models per world shape (tools/reference/light.py).",
            "placed": list(placed),
            "bodies": {k: (None if v is None else {"shape": v["shape"],
                                                    "origin": list(v["origin"])})
                       for k, v in bodies.items()},
            "cases": cases}
