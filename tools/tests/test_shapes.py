# SPDX-License-Identifier: Apache-2.0
"""The Python reference for world shapes (tools/reference/shapes.py) against the shared table
(the engine is checked against the same table in engine/tests/test_shape.cpp), and the seams."""
import math
import random
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from reference import shapes as S  # noqa: E402
from reference import table  # noqa: E402

SHAPES = {
    "bounded_area": S.Shape(S.BOUNDED_AREA, 2000.0, 3000.0, 9.81),
    "flat_cylinder": S.Shape(S.FLAT_CYLINDER, 6283.0, 5000.0, 9.81),
    "curved_cylinder": S.Shape(S.CURVED_CYLINDER, 1000.0, 5000.0, 9.81),
    "sphere_outside": S.Shape(S.SPHERE_OUTSIDE, 1000.0, 0.0, 9.81),
    "sphere_inside": S.Shape(S.SPHERE_INSIDE, 1500.0, 0.0, 7.5),
}


def bits(x: float) -> str:
    return f"{struct.unpack('<Q', struct.pack('<d', x))[0]:016x}"


def test_shapes_match_their_table_bit_for_bit():
    cases = table("shape_points")
    assert {c["shape"] for c in cases} == set(SHAPES)
    for c in cases:
        shape = SHAPES[c["shape"]]
        f, u, v, h = c["surface"][0], *(float.fromhex(x) for x in c["surface"][1:])
        p = shape.position(f, u, v, h)
        assert [bits(x) for x in p] == c["position"], c
        bf, bu, bv, bh = shape.surface(p)
        assert [bf, bits(bu), bits(bv), bits(bh)] == c["back"], c
        assert [bits(x) for x in shape.up(p)] == c["up"], c
        assert [bits(x) for x in shape.gravity(p)] == c["gravity"], c


def test_cube_sphere_seams_are_exact():
    for name in ("sphere_outside", "sphere_inside"):
        shape, n, shared = SHAPES[name], 32, {}
        for f in range(6):
            for i in range(n + 1):
                for j in range(n + 1):
                    if i in (0, n) or j in (0, n):
                        key = tuple(bits(x) for x in shape.position(f, i / n, j / n, 3.0))
                        shared[key] = shared.get(key, 0) + 1
        counts = sorted(shared.values())
        assert counts[0] == 2 and counts.count(3) == 8 and counts.count(2) == 12 * (n - 1)


def test_round_trips():
    rng = random.Random(5)
    for shape in SHAPES.values():
        for _ in range(3000):
            f, u, v = rng.randrange(shape.faces), rng.random(), rng.random()
            h = rng.uniform(-100, 100)
            p = shape.position(f, u, v, h)
            q = shape.position(*shape.surface(p))
            assert math.dist(p, q) < 1e-8


def test_spin_gravity_fades_towards_the_axis():
    tube = SHAPES["curved_cylinder"]
    assert math.hypot(*tube.gravity(tube.position(0, 0.4, 0.5, 0.0))) == pytest.approx(9.81)
    assert math.hypot(*tube.gravity(tube.position(0, 0.4, 0.5, 750.0))) == pytest.approx(9.81 / 4)


def test_shape_fingerprint_matches_its_table():
    from reference.hashing import fnv1a64
    data = bytearray()
    for shape in SHAPES.values():
        for f in range(shape.faces):
            for j in range(17):
                for i in range(17):
                    p = shape.position(f, i / 16, j / 16, 2.5)
                    data += struct.pack("<9d", *p, *shape.up(p), *shape.gravity(p))
    assert f"{fnv1a64(bytes(data)):016x}" == table("shape_fingerprint")[0]["fingerprint"]
