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
