# SPDX-License-Identifier: Apache-2.0
"""How cold or hot it feels (engine/core/include/midgard/survival.h; milestone 5.15).

    feels like = the air's temperature,
                 made colder by the wind (Environment Canada's wind chill, for wind over 4.8 km/h,
                 in full at 10 degrees and below and fading out by 16; never warmer),
                 colder still for being wet (up to 4 degrees, and 0.3 more a metre a second of
                 wind: wet and windy is the worst),
                 and warmer by the heat of fires and stoves nearby.

A heat source warms by `heat` degrees at its middle, less further out, and none from `radius`:
heat * (1 - (d / radius)^2)^2.
"""


def wind_chill(air: float, wind: float) -> float:
    """The wind chill (degrees Celsius) for air at `air` degrees and wind at `wind` m/s."""
    v = wind * 3.6
    if v <= 4.8:
        return air
    p = v ** 0.16
    return 13.12 + 0.6215 * air - 11.37 * p + 0.3965 * air * p


def feels_like(air: float, wind: float, wetness: float, heat: float) -> float:
    t = air
    chill = min(wind_chill(air, wind), air)
    k = min(max((16.0 - air) / 6.0, 0.0), 1.0)
    t = t + (chill - t) * k
    wet = min(max(wetness, 0.0), 1.0)
    t = t - wet * (4.0 + 0.3 * max(wind, 0.0))
    return t + heat


def warmth(heat: float, radius: float, distance: float) -> float:
    if radius <= 0.0 or distance >= radius:
        return 0.0
    f = 1.0 - (distance / radius) ** 2
    return heat * f * f


def make_table() -> dict:
    """The shared table schema/tests/feels_like.json (made once, then fixed)."""
    cases = []
    for air in (-30.0, -10.0, 0.0, 5.0, 10.0, 13.0, 16.0, 25.0, 38.0):
        for wind in (0.0, 1.0, 1.4, 5.0, 12.0, 25.0):
            for wetness in (0.0, 0.5, 1.0):
                for heat in (0.0, 12.5):
                    cases.append({"air": air, "wind": wind, "wetness": wetness, "heat": heat,
                                  "feels_like": feels_like(air, wind, wetness, heat)})
    warmths = [{"heat": h, "radius": r, "distance": d, "warmth": warmth(h, r, d)}
               for h, r in ((20.0, 4.0), (8.0, 1.5), (30.0, 0.0))
               for d in (0.0, 0.5, 1.0, 2.0, 3.99, 4.0, 10.0)]
    return {"_about": "Feels-like temperature and heat sources' warmth "
                      "(tools/reference/survival.py; midgard/survival.h).",
            "cases": cases, "warmth": warmths}


if __name__ == "__main__":
    import json
    import sys
    json.dump(make_table(), sys.stdout, indent=1)
