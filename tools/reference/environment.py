# SPDX-License-Identifier: Apache-2.0
"""Environments (engine/scene/include/midgard/environment.h): curves over the solar phase.

A curve is a list of keys [[phase, value], ...], phases in [0, 1) in increasing order (0 sunrise,
0.25 noon, 0.5 sunset, 0.75 midnight), each value a number or an [r, g, b] colour. Between two
keys the value eases from one to the next (smoothstep), and the day wraps round: after the last
key it eases back to the first across midnight. One key holds all day; no keys means the curve
isn't set (the engine's analytic sky stands in).
"""


def smoothstep(t: float) -> float:
    return t * t * (3.0 - 2.0 * t)


def evaluate(keys: list, phase: float):
    if not keys:
        return None
    phase = phase % 1.0
    n = len(keys)
    if n == 1:
        return keys[0][1]
    i = n - 1
    for k in range(n):
        if keys[k][0] <= phase:
            i = k
    start = keys[i][0]
    if phase < start:  # before the first key: the stretch from the last key, across midnight
        phase += 1.0
    nxt = (i + 1) % n
    end = keys[nxt][0] + (1.0 if nxt <= i else 0.0)
    t = smoothstep((phase - start) / (end - start))
    a, b = keys[i][1], keys[nxt][1]
    if isinstance(a, list):
        return [x + (y - x) * t for x, y in zip(a, b, strict=True)]
    return a + (b - a) * t


def make_table() -> dict:
    """The shared table schema/tests/environment_curves.json (made once, then fixed)."""
    curves = {
        "one": [[0.3, 2.0]],
        "two": [[0.0, 0.0], [0.5, 1.0]],
        "day": [[0.0, 0.2], [0.05, 0.9], [0.25, 1.0], [0.45, 0.9], [0.5, 0.2], [0.75, 0.0]],
        "late_start": [[0.2, 1.0], [0.7, 3.0]],
        "colour": [[0.0, [1.0, 0.5, 0.2]], [0.25, [0.3, 0.5, 0.9]], [0.6, [0.02, 0.03, 0.08]]],
    }
    phases = [i / 40 for i in range(40)] + [0.0001, 0.2499, 0.2501, 0.4999, 0.9999]
    cases = [{"curve": name, "phase": p, "value": evaluate(keys, p)}
             for name, keys in curves.items() for p in phases]
    return {"_about": "Curves over the solar phase (tools/reference/environment.py).",
            "curves": curves, "cases": cases}


def season_tint(settings: dict, season: float) -> dict:
    """The season's tints ({"sunlight", "ambient", "sky"}: [r, g, b]): white blended towards
    settings["winter"]'s as season goes to -1, or settings["summer"]'s as it goes to 1 (tints not
    given are white). The engine blends in single precision."""
    s = min(max(season, -1.0), 1.0)
    to = settings.get("winter" if s < 0.0 else "summer", {})
    a = abs(s)
    return {k: [1.0 + (c - 1.0) * a for c in to.get(k, [1.0, 1.0, 1.0])]
            for k in ("sunlight", "ambient", "sky")}
