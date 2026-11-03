# SPDX-License-Identifier: Apache-2.0
"""The Python reference implementations against the shared tables in schema/tests/ (the engine
is checked against the same tables in engine/tests/test_reference_tables.cpp)."""
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from reference import TABLES, table  # noqa: E402
from reference.hashing import fnv1a64  # noqa: E402


def case_bytes(case: dict) -> bytes:
    return case["text"].encode("utf-8") if "text" in case else bytes.fromhex(case["hex"])


def test_fnv1a64_matches_its_table():
    cases = table("fnv1a64")
    assert len(cases) >= 3
    for case in cases:
        assert f"{fnv1a64(case_bytes(case)):016x}" == case["hash"], case


def test_every_table_is_well_formed():
    tables = sorted(TABLES.glob("*.json"))
    assert tables
    for path in tables:
        data = json.loads(path.read_text("utf-8"))
        assert data["_about"] and data["cases"], path.name


# ---- deterministic maths (midgard/det.h) ------------------------------------------------------
def _bits(x: float) -> str:
    import struct
    return f"{struct.unpack('<Q', struct.pack('<d', x))[0]:016x}"


def test_det_hash_matches_its_table():
    from reference import det
    for c in table("det_hash"):
        assert f"{det.hash3(int(c['seed']), c['x'], c['y'], c['z']):016x}" == c["hash"], c


def test_det_rng_matches_its_table():
    from reference import det
    for c in table("det_rng"):
        if "tag" in c:
            assert f"{det.tag(c['tag']):016x}" == c["value"]
            continue
        seed = int(c["seed"])
        r = det.Rng(seed)
        assert [f"{r.next():016x}" for _ in range(5)] == c["next"]
        s = det.Rng(seed).split(det.tag("weather"))
        assert [f"{s.next():016x}" for _ in range(3)] == c["split_weather"]
        b = det.Rng(seed)
        assert [str(b.below(n)) for n in (1, 2, 3, 10, 1000, 2**63 + 5)] == \
            c["below_1_2_3_10_1000_2p63p5"]
        u = det.Rng(seed)
        assert [_bits(u.uniform()) for _ in range(3)] == c["uniform_bits"]


def test_det_trig_matches_its_table_bit_for_bit():
    import math

    from reference import det
    for c in table("det_trig"):
        x = float.fromhex(c["x"])
        assert _bits(det.sin(x)) == c["sin"], c
        assert _bits(det.cos(x)) == c["cos"], c
        assert _bits(det.sqrt(abs(x))) == c["sqrt"], c
        # And it is sin and cos: within a few units in the last place of the system's.
        assert abs(det.sin(x) - math.sin(x)) <= 4e-16 * max(1.0, abs(x)) ** 0.5
        assert abs(det.cos(x) - math.cos(x)) <= 4e-16 * max(1.0, abs(x)) ** 0.5


def test_det_noise_fields_match_their_table():
    import struct

    from reference import det
    from reference.hashing import fnv1a64
    for c in table("det_noise"):
        seed, (ox, oy, oz), step, n = int(c["seed"]), c["origin"], c["step"], c["n"]
        values = []
        for j in range(n):
            for i in range(n):
                x, y = ox + i * step, oy + j * step
                if c["kind"] == "gradient":
                    values.append(det.gradient_noise(seed, x, y, oz))
                elif c["kind"] == "value":
                    values.append(det.value_noise(seed, x, y, oz))
                else:
                    values.append(det.fbm(seed, x, y, oz, c["octaves"]))
        assert values[:4] == c["first"], c["kind"]
        data = b"".join(struct.pack("<i", v) for v in values)
        assert f"{fnv1a64(data):016x}" == c["hash"], c["kind"]


def test_det_fingerprint_matches_its_table():
    import struct

    from reference import det
    from reference.hashing import fnv1a64
    data = bytearray()
    for j in range(64):
        for i in range(64):
            data += struct.pack("<i", det.fbm(829174332, -12000.0 + i * 0.173,
                                              4500.0 + j * 0.173, 33.0, 6))
    for k in range(1000):
        x = -500.0 + k * 1.0371
        data += struct.pack("<3d", det.sin(x), det.cos(x), det.sqrt(abs(x)))
    r = det.Rng(829174332).split(det.tag("selftest"))
    for _ in range(100):
        data += struct.pack("<Q", r.next())
    assert f"{fnv1a64(bytes(data)):016x}" == table("det_fingerprint")[0]["fingerprint"]


def test_det_atan_tan_and_atan2_match_their_table_bit_for_bit():
    import math

    from reference import det
    for c in table("det_atan"):
        if "atan" in c:
            x = float.fromhex(c["atan"])
            got, system = det.atan(x), math.atan(x)
        elif "tan" in c:
            x = float.fromhex(c["tan"])
            got, system = det.tan(x), math.tan(x)
        else:
            y, x = (float.fromhex(v) for v in c["atan2"])
            got, system = det.atan2(y, x), math.atan2(y, x)
        assert _bits(got) == c["bits"], c
        assert abs(got - system) <= 1e-15 * max(1.0, abs(system)), c


# ---- calendars (midgard/calendar.h) -----------------------------------------------------------
def test_calendar_dates_match_their_table():
    from reference import calendar as cal
    data = json.loads((TABLES / "calendar_dates.json").read_text("utf-8"))
    assert len(data["cases"]) > 1000
    for c in data["cases"]:
        calendar = data["calendars"][c["calendar"]]
        t = c["time"]
        d = cal.date_at(calendar, t)
        for key in ("year", "month", "day", "hour", "minute", "second", "weekday", "day_of_year",
                    "season"):
            assert d[key] == c[key], (key, c)
        assert _bits(cal.daylight_on(calendar, d["year"], d["day_of_year"])) == c["daylight"], c
        assert _bits(cal.solar_phase_at(calendar, t)) == c["phase"], c
        sunrise, sunset = cal.sun_times(calendar, t)
        assert [_bits(sunrise), _bits(sunset)] == [c["sunrise"], c["sunset"]], c
        assert [_bits(cal.moon_phase(calendar, i, t)) for i in range(len(calendar["moons"]))] \
            == c["moons"], c
        assert cal.time_of(calendar, d["year"], d["month"], d["day"], d["seconds_into_day"]) == t


def test_a_20_hour_day_with_70_percent_daylight():
    """Milestone 5.7's exit numbers: sunrise at 03:00 and sunset at 17:00."""
    from reference import calendar as cal
    twenty = json.loads((TABLES / "calendar_dates.json").read_text("utf-8"))["calendars"]["twenty"]
    sunrise, sunset = cal.sun_times(twenty, 0)
    assert round(sunrise) == 3 * 3600 and round(sunset) == 17 * 3600
    assert cal.solar_phase_at(twenty, 3 * 3600) == 0.0  # sunrise
    assert cal.solar_phase_at(twenty, 10 * 3600) == 0.25  # solar noon
    assert cal.solar_phase_at(twenty, 17 * 3600) == 0.5  # sunset


def test_body_calendars_match_their_table():
    from reference import calendar as cal
    data = json.loads((TABLES / "calendar_bodies.json").read_text("utf-8"))
    for c in data["cases"]:
        body = cal.roll_body(data["templates"][c["template"]], int(c["seed"]))
        assert body["hours_per_day"] == c["hours_per_day"], c
        assert [m["days"] for m in body["months"]] == c["months"], c
        assert _bits(body["tilt"]) == c["tilt"], c


# ---- environments (midgard/environment.h) -------------------------------------------------------
def test_environment_curves_match_their_table():
    from reference import environment as env
    data = json.loads((TABLES / "environment_curves.json").read_text("utf-8"))
    assert len(data["cases"]) == 225
    for c in data["cases"]:
        assert env.evaluate(data["curves"][c["curve"]], c["phase"]) == c["value"], c
    assert env.evaluate([], 0.3) is None  # not set
    assert env.evaluate([[0.6, 4.0]], 0.1) == 4.0  # one key: all day


# ---- light models per world shape (midgard/sunlight.h) ------------------------------------------
def test_light_models_match_their_table():
    from reference import light
    data = json.loads((TABLES / "light_models.json").read_text("utf-8"))
    assert len(data["cases"]) == 288
    placed = tuple(data["placed"])
    for c in data["cases"]:
        body = data["bodies"][c["body"]]
        if body is not None:
            body = {"shape": body["shape"], "origin": tuple(body["origin"])}
        phase, to_sun, height = light.local_sun(body, placed, c["clock"], tuple(c["point"]))
        # (System trigonometry: the last bits differ between platforms' maths libraries.)
        assert phase == pytest.approx(c["phase"], abs=1e-12), c
        assert list(to_sun) == pytest.approx(c["to_sun"], abs=1e-12), c
        assert height == pytest.approx(c["height"], abs=1e-12), c


def test_a_planets_noon_moves_with_longitude_and_its_poles_have_polar_day_and_night():
    import math

    from reference import light
    planet = {"shape": "sphere_outside", "origin": (0.0, -1000.0, 0.0)}
    placed = light.norm((0.3, 0.75, -0.6))
    clock = {"time": 0.5, "daylight": 0.5, "noon": 0.5, "mode": "uniform", "tilt": 0.0,
             "year": 0.0}
    assert light.local_sun(planet, placed, clock, (0.0, 0.0, 0.0))[0] == 0.25  # noon here
    assert abs(light.local_sun(planet, placed, clock, (0.0, -2000.0, 0.0))[0] - 0.75) < 1e-12
    # Latitude mode at the north pole: the sun never sets in summer, never rises in winter.
    north0 = light.mul(light.norm((placed[0], 0.0, placed[2])), -1.0)
    lat0 = math.pi / 2 - math.asin(placed[1])
    axis = light.add(light.mul(north0, math.cos(lat0)), light.mul((0.0, 1.0, 0.0), math.sin(lat0)))
    pole = light.add(planet["origin"], light.mul(axis, 1000.0))
    for hour in range(24):
        summer = dict(clock, time=hour / 24, mode="latitude", tilt=23.5, year=0.25)
        winter = dict(summer, year=0.75)
        assert light.local_sun(planet, placed, summer, pole)[2] > 0.3
        assert light.local_sun(planet, placed, winter, pole)[2] < -0.3


# ---- seasons' colours (milestone 5.8) -----------------------------------------------------------
def test_seasons_match_their_table():
    from reference import calendar as cal
    from reference import environment as env
    from reference import light
    data = json.loads((TABLES / "seasons.json").read_text("utf-8"))
    calendars = json.loads((TABLES / "calendar_dates.json").read_text("utf-8"))["calendars"]
    assert len(data["summer"]) == 21 and len(data["cases"]) == 108
    for c in data["summer"]:
        got = cal.summer_on(calendars[c["calendar"]], c["year"], c["day_of_year"])
        assert got == c["summer"], c
    placed = tuple(data["placed"])
    for c in data["cases"]:
        body = data["bodies"][c["body"]]
        if body is not None:
            body = {"shape": body["shape"], "origin": tuple(body["origin"])}
        season = light.local_season(body, placed, c["clock"], tuple(c["point"]))
        assert season == pytest.approx(c["season"], abs=1e-12), c  # (system trigonometry)
    for c in data["tints"]:
        assert env.season_tint(data["settings"], c["season"]) == c["tint"], c


def test_summer_is_the_longest_day_and_winter_the_shortest():
    from reference import calendar as cal
    c = {"months": [{"days": 100}], "seasons": [
        {"share": 0.25, "daylight": 0.5}, {"share": 0.25, "daylight": 0.7},
        {"share": 0.25, "daylight": 0.5}, {"share": 0.25, "daylight": 0.3}]}
    summer = [cal.summer_on(c, 1, d) for d in range(100)]
    assert max(range(100), key=lambda d: summer[d]) == 37  # the middle of the second season
    assert min(range(100), key=lambda d: summer[d]) == 87
    assert cal.summer_on({"months": [{"days": 10}]}, 1, 3) == 0.0  # no seasons


# ---- terrain (midgard/terrain.h) ----------------------------------------------------------------
def test_terrain_matches_its_table():
    from reference import terrain
    data = json.loads((TABLES / "terrain_chunks.json").read_text("utf-8"))
    for name, layout in data["layout"].items():
        t = terrain.Terrain(data["bodies"][name])
        assert [t.roots_u, t.roots_v, t.max_level] == layout
    for c in data["cases"]:
        t = terrain.Terrain(data["bodies"][c["body"]], data["settings"][c["settings"]])
        if "hash" in c:
            assert f"{t.chunk_hash(tuple(c['key'])):016x}" == c["hash"], c
        else:
            assert t.height(0, c["u"], c["v"]) == c["height"], c
    assert f"{terrain.fingerprint():016x}" == data["fingerprint"]


def test_cube_sphere_terrain_matches_its_tables():
    """Milestone 5.10's tables: a sample of their chunk hashes (the C++ tests check them all)."""
    from reference import terrain
    data = json.loads((TABLES / "terrain_spheres.json").read_text("utf-8"))
    for name, layout in data["layout"].items():
        t = terrain.Terrain(data["bodies"][name])
        assert [t.roots_u, t.roots_v, t.max_level] == layout
    cases = data["cases"]
    for c in [c for c in cases if "hash" in c][::24] + [c for c in cases if "height" in c]:
        t = terrain.Terrain(data["bodies"][c["body"]])
        if "hash" in c:
            assert f"{t.chunk_hash(tuple(c['key'])):016x}" == c["hash"], c
        else:
            assert t.height(c["face"], c["u"], c["v"]) == c["height"], c
    stamped = json.loads((TABLES / "terrain_sphere_stamps.json").read_text("utf-8"))
    t = terrain.Terrain(stamped["body"], {}, stamped["stamps"])
    for c in stamped["cases"]:
        if "height" in c:
            assert t.height(c["face"], c["u"], c["v"]) == c["height"], c
    assert f"{terrain.sphere_fingerprint():016x}" == stamped["fingerprint"]


def test_a_cylinders_terrain_meets_itself_at_the_join():
    from reference import terrain
    for body in (terrain.BODIES["strip"], terrain.BODIES["habitat"]):
        t = terrain.Terrain(body)
        for v in (0.2, 0.5, 0.77):
            assert t.height(0, 0.0, v) == t.height(0, 1.0, v)


def test_autumn_is_a_quarter_year_from_summer():
    from reference import calendar as cal
    calendars = json.loads((TABLES / "calendar_dates.json").read_text("utf-8"))["calendars"]
    for c in calendars.values():
        for day in range(0, cal.year_days(c, 1), 7):
            s, a = cal.summer_on(c, 1, day), cal.autumn_on(c, 1, day)
            if len(c.get("seasons", [])) >= 2:
                assert abs(s * s + a * a - 1.0) < 1e-12
            else:
                assert a == 0.0


def test_terrain_stamps_match_their_table():
    from reference import terrain
    data = json.loads((TABLES / "terrain_stamps.json").read_text("utf-8"))
    for c in data["cases"]:
        t = terrain.Terrain(data["bodies"][c["body"]], {}, data["stamps"])
        if "hash" in c:
            assert f"{t.chunk_hash(tuple(c['key'])):016x}" == c["hash"], c
        else:
            assert t.height(0, c["u"], c["v"]) == c["height"], c


# ---- world generation graphs (midgard/worldgen.h) -----------------------------------------------
def test_world_graphs_match_their_table():
    from reference import worldgen
    data = json.loads((TABLES / "worldgen_cases.json").read_text("utf-8"))
    for c in data["cases"]:
        graph = worldgen.Graph(data["graphs"][c["graph"]])
        shape = worldgen.make_shape(data["bodies"][c["body"]])
        assert graph.evaluate(shape, data["seed"], tuple(c["place"])) == c["height"], c
    assert f"{worldgen.fingerprint():016x}" == data["fingerprint"]


def test_terrain_from_a_world_graph_matches_the_table():
    """Chunk hashes of terrain made from graphs; the 5.9 ground as a graph is unchanged."""
    from reference import terrain, worldgen
    data = json.loads((TABLES / "worldgen_cases.json").read_text("utf-8"))
    by_body = {}
    for c in data["chunks"]:
        body = {**data["bodies"][c["body"]], "seed": data["seed"]}
        settings = {"graph": data["graphs"][c["graph"]]} if c["graph"] else {}
        assert f"{terrain.Terrain(body, settings).chunk_hash(tuple(c['key'])):016x}" == c["hash"]
        by_body.setdefault(c["body"], {})[c["graph"]] = c["hash"]
    for hashes in by_body.values():
        assert hashes["classic"] == hashes[None]
    assert worldgen  # (the twin the table came from)


def test_world_graph_nodes_by_hand():
    from reference import shapes, worldgen
    area = shapes.Shape(shapes.BOUNDED_AREA, 3000.0, 2000.0, 9.81)

    def one(node):
        return worldgen.Graph({"output": "x", "nodes": {"x": node}}).evaluate(area, 7,
                                                                             (0, 0.5, 0.5))
    assert one({"op": "add", "in": [1, 2, 3.5]}) == 6.5
    assert one({"op": "divide", "in": [10, 0]}) == 0.0
    assert one({"op": "lerp", "in": [2, 10, 0.25]}) == 4.0
    assert one({"op": "smoothstep", "in": [2.5], "from": 0, "to": 10}) == 0.15625
    assert one({"op": "curve", "in": [20], "points": [[0, 0], [10, 20], [30, 25]]}) == 22.5
    assert one({"op": "input", "name": "edge_distance"}) == 1000.0
    assert one({"op": "smooth", "in": [7], "radius": 30}) == 7.0


# ---- feels-like temperature (midgard/survival.h; milestone 5.15) -----------------------------
def test_feels_like_matches_its_table():
    from reference import survival
    data = json.loads((TABLES / "feels_like.json").read_text("utf-8"))
    assert len(data["cases"]) == 324
    for c in data["cases"]:
        got = survival.feels_like(c["air"], c["wind"], c["wetness"], c["heat"])
        assert got == pytest.approx(c["feels_like"], abs=1e-12), c
    for w in data["warmth"]:
        assert survival.warmth(w["heat"], w["radius"], w["distance"]) == pytest.approx(
            w["warmth"], abs=1e-12), w


def test_feels_like_by_hand():
    from reference.survival import feels_like, warmth, wind_chill
    assert feels_like(20.0, 0.0, 0.0, 0.0) == 20.0  # still, dry, mild: as it is
    assert feels_like(20.0, 10.0, 0.0, 0.0) == 20.0  # wind doesn't chill warm air
    assert wind_chill(-10.0, 20.0 / 3.6) == pytest.approx(-17.9, abs=0.05)  # (the charts)
    assert feels_like(-10.0, 1.0, 0.0, 0.0) == -10.0  # (3.6 km/h: too little to chill)
    assert feels_like(5.0, 0.0, 1.0, 0.0) == 1.0  # soaked: four degrees colder
    assert feels_like(5.0, 0.0, 0.0, 20.0) == 25.0  # right by the fire
    assert warmth(20.0, 4.0, 0.0) == 20.0 and warmth(20.0, 4.0, 4.0) == 0.0
    assert warmth(20.0, 4.0, 2.0) == pytest.approx(11.25)
