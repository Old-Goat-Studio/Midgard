# SPDX-License-Identifier: Apache-2.0
"""Calendars (engine/core/include/midgard/calendar.h, engine/core/src/calendar.cpp), step by step.

A calendar is a dict with the keys of the calendar table (schema/migrations/0009_calendar.sql)
and lists "months" ([{"days"}]), "weekdays", "seasons" ([{"share", "daylight"}]) and "moons"
([{"period", "offset"}]). The clock counts game seconds from 0 = year 1, month 1, day 1, 00:00.
Floats are combined in the same order as the engine, and daylight uses det.cos, so results match
it bit for bit.
"""
import math

from . import det


def get(c: dict, key: str, default):
    return c.get(key, default)


def seconds_per_day(c: dict) -> int:
    return get(c, "minutes_per_hour", 60) * 60 * get(c, "hours_per_day", 24)


def is_leap(c: dict, year: int) -> bool:
    every = get(c, "leap_every", 0)
    return every > 0 and year % every == 0


def month_days(c: dict, year: int, month: int) -> int:
    """month is 1-based."""
    days = c["months"][month - 1]["days"]
    return days + (1 if is_leap(c, year) and month == get(c, "leap_month", 1) else 0)


def year_days(c: dict, year: int) -> int:
    return sum(m["days"] for m in c["months"]) + (1 if is_leap(c, year) else 0)


def _year_of(c: dict, day: int) -> tuple[int, int]:
    """(year, day of year) for a whole-day number."""
    base = sum(m["days"] for m in c["months"])
    every = get(c, "leap_every", 0)
    if every > 0:
        n, day = divmod(day, every * base + 1)  # a cycle of years ends with its leap year
        year = 1 + n * every
    else:
        n, day = divmod(day, base)
        year = 1 + n
    while day >= year_days(c, year):
        day -= year_days(c, year)
        year += 1
    return year, day


def season_on(c: dict, year: int, day_of_year: int) -> int:
    seasons = c.get("seasons", [])
    if not seasons:
        return -1
    f = (day_of_year + 0.5) / year_days(c, year)
    acc = 0.0
    for i, s in enumerate(seasons):
        acc += s["share"]
        if f < acc:
            return i
    return len(seasons) - 1


def daylight_on(c: dict, year: int, day_of_year: int) -> float:
    seasons = c.get("seasons", [])
    if not seasons:
        d = get(c, "daylight", 0.6)
    elif len(seasons) == 1:
        d = seasons[0]["daylight"]
    else:
        longest = max(range(len(seasons)), key=lambda i: (seasons[i]["daylight"], -i))
        hi = seasons[longest]["daylight"]
        lo = min(s["daylight"] for s in seasons)
        base = (hi + lo) / 2
        amplitude = (hi - lo) / 2
        start = 0.0
        for s in seasons[:longest]:
            start += s["share"]
        middle = start + seasons[longest]["share"] / 2
        f = (day_of_year + 0.5) / year_days(c, year)
        d = base + amplitude * det.cos(2 * math.pi * (f - middle))
    return min(max(d, 0.01), 0.99)


def solar_phase(seconds_into_day: float, day_length: float, day_fraction: float,
                noon: float = 0.5) -> float:
    """0 = sunrise, 0.5 = sunset, 1 = next sunrise (roadmap §6)."""
    d = min(max(day_fraction, 0.01), 0.99)
    t = (seconds_into_day / day_length - noon + d / 2) % 1.0   # 0 at sunrise
    if t < d:
        return 0.5 * t / d                                      # day: 0 -> 0.5
    return 0.5 + 0.5 * (t - d) / (1 - d)                        # night: 0.5 -> 1


def sun_elevation(phase: float, max_elevation_deg: float) -> float:
    return max_elevation_deg * det.sin(2 * math.pi * phase)


def date_at(c: dict, time: int) -> dict:
    time = max(time, 0)
    spd = seconds_per_day(c)
    day, seconds = divmod(time, spd)
    year, day_of_year = _year_of(c, day)
    month, rest = 1, day_of_year
    while rest >= month_days(c, year, month):
        rest -= month_days(c, year, month)
        month += 1
    hour_seconds = get(c, "minutes_per_hour", 60) * 60
    return {"year": year, "month": month, "day": rest + 1,
            "hour": seconds // hour_seconds, "minute": seconds % hour_seconds // 60,
            "second": seconds % 60, "weekday": day % len(c["weekdays"]),
            "day_of_year": day_of_year, "season": season_on(c, year, day_of_year),
            "day_number": day, "seconds_into_day": seconds}


def time_of(c: dict, year: int, month: int, day: int, seconds_into_day: int = 0) -> int:
    if year < 1 or not 1 <= month <= len(c["months"]) or \
            not 1 <= day <= month_days(c, year, month) or \
            not 0 <= seconds_into_day < seconds_per_day(c):
        raise ValueError("no such date")
    base = sum(m["days"] for m in c["months"])
    every = get(c, "leap_every", 0)
    days, y = 0, 1
    if every > 0:
        n = (year - 1) // every
        days, y = n * (every * base + 1), 1 + n * every
    else:
        days, y = (year - 1) * base, year
    for earlier in range(y, year):
        days += year_days(c, earlier)
    for m in range(1, month):
        days += month_days(c, year, m)
    return (days + day - 1) * seconds_per_day(c) + seconds_into_day


def solar_phase_at(c: dict, time: int) -> float:
    d = date_at(c, time)
    return solar_phase(d["seconds_into_day"], seconds_per_day(c),
                       daylight_on(c, d["year"], d["day_of_year"]), get(c, "solar_noon", 0.5))


def sun_times(c: dict, time: int) -> tuple[float, float]:
    """Sunrise and sunset in seconds into the day of `time`."""
    d = date_at(c, time)
    spd = seconds_per_day(c)
    share = daylight_on(c, d["year"], d["day_of_year"])
    sunrise = (get(c, "solar_noon", 0.5) - share / 2) % 1.0 * spd
    return sunrise, sunrise + share * spd


def moon_phase(c: dict, moon: int, time: int) -> float:
    d = date_at(c, time)
    m = c["moons"][moon]
    return ((d["day_number"] + d["seconds_into_day"] / seconds_per_day(c)) / m["period"]
            + m.get("offset", 0.0)) % 1.0


def roll_body(c: dict, seed: int) -> dict:
    """A body's calendar from a template and the body's seed: day length (whole hours), year
    length (the months scaled to it, each keeping at least a day) and axial tilt, each drawn from
    its own stream within the template's ranges."""
    rng = det.Rng(seed).split(det.tag("calendar"))
    out = {**c, "months": [dict(m) for m in c["months"]], "standard": 0}

    lo = math.ceil(get(c, "day_hours_min", 24.0))
    hi = math.floor(get(c, "day_hours_max", 24.0))
    if hi < lo:
        hours = max(1, int(get(c, "day_hours_min", 24.0) + 0.5))
    else:
        hours = lo + rng.split(det.tag("day")).below(hi - lo + 1)
    out["hours_per_day"] = hours
    out["day_hours_min"] = out["day_hours_max"] = float(hours)

    ymin, ymax = get(c, "year_days_min", 0), get(c, "year_days_max", 0)
    if ymax > 0:
        target = ymin + rng.split(det.tag("year")).below(ymax - ymin + 1)
        target = max(target, len(c["months"]))
        base = sum(m["days"] for m in c["months"])
        cumulative, previous = 0, 0
        for m in out["months"]:
            cumulative += m["days"]
            boundary = cumulative * target // base
            m["days"] = boundary - previous
            previous = boundary
        for m in out["months"]:  # every month keeps a day, taken from the longest
            if m["days"] < 1:
                longest = max(out["months"], key=lambda x: x["days"])
                longest["days"] -= 1 - m["days"]
                m["days"] = 1
        out["year_days_min"] = out["year_days_max"] = target

    tmin, tmax = get(c, "tilt_min", 0.0), get(c, "tilt_max", 0.0)
    tilt = tmin + (tmax - tmin) * rng.split(det.tag("tilt")).uniform()
    out["tilt"] = tilt
    out["tilt_min"] = out["tilt_max"] = tilt
    return out
