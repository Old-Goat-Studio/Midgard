# SPDX-License-Identifier: Apache-2.0
"""The Python reference implementations against the shared tables in schema/tests/ (the engine
is checked against the same tables in engine/tests/test_reference_tables.cpp)."""
import json
import sys
from pathlib import Path

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
