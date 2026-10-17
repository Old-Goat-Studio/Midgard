# SPDX-License-Identifier: Apache-2.0
"""Transforms as the engine has them (schema/components.md): position, rotation as Euler degrees
combined R = Ry · Rx · Rz, and scale, making the local matrix T · R · S relative to the parent.

The editor uses these to keep an entity where it is in the world when it moves to another parent
(dragged into a group), to centre a group on its parts, and to turn the gizmos' world-axis drags
into changes of an entity's own transform. Matrices are 4 x 4, row by row.
"""
import math
from collections.abc import Iterable

from .scene_model import Entity

Matrix = list[list[float]]


def identity() -> Matrix:
    return [[1.0 if r == c else 0.0 for c in range(4)] for r in range(4)]


def multiply(a: Matrix, b: Matrix) -> Matrix:
    return [[sum(a[r][k] * b[k][c] for k in range(4)) for c in range(4)] for r in range(4)]


def rotation(euler: Iterable[float]) -> list[list[float]]:
    """The 3 x 3 rotation for Euler degrees (pitch, yaw, roll): Ry · Rx · Rz."""
    x, y, z = (math.radians(a) for a in euler)
    cx, sx, cy, sy, cz, sz = math.cos(x), math.sin(x), math.cos(y), math.sin(y), math.cos(z), \
        math.sin(z)
    ry = [[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]
    rx = [[1, 0, 0], [0, cx, -sx], [0, sx, cx]]
    rz = [[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]]
    mul = lambda a, b: [[sum(a[r][k] * b[k][c] for k in range(3)) for c in range(3)]  # noqa: E731
                        for r in range(3)]
    return mul(mul(ry, rx), rz)


def axis_rotation(axis: Iterable[float], degrees: float) -> list[list[float]]:
    """The 3 x 3 rotation by `degrees` about a unit axis."""
    x, y, z = axis
    a = math.radians(degrees)
    c, s, t = math.cos(a), math.sin(a), 1 - math.cos(a)
    return [[t * x * x + c, t * x * y - s * z, t * x * z + s * y],
            [t * x * y + s * z, t * y * y + c, t * y * z - s * x],
            [t * x * z - s * y, t * y * z + s * x, t * z * z + c]]


def euler(r: list[list[float]]) -> list[float]:
    """Euler degrees (pitch, yaw, roll) for a 3 x 3 rotation (the inverse of rotation())."""
    sx = max(-1.0, min(1.0, -r[1][2]))
    pitch = math.asin(sx)
    if abs(sx) < 0.99999:
        yaw, roll = math.atan2(r[0][2], r[2][2]), math.atan2(r[1][0], r[1][1])
    else:  # looking straight up or down: yaw and roll turn about the same axis
        yaw, roll = math.atan2(-r[2][0], r[0][0]), 0.0
    return [math.degrees(pitch), math.degrees(yaw), math.degrees(roll)]


def compose(transform: dict) -> Matrix:
    """The local matrix T · R · S of a transform component (missing keys: their defaults)."""
    p = transform.get("position", [0.0, 0.0, 0.0])
    s = transform.get("scale", [1.0, 1.0, 1.0])
    r = rotation(transform.get("rotation", [0.0, 0.0, 0.0]))
    m = identity()
    for row in range(3):
        for col in range(3):
            m[row][col] = r[row][col] * s[col]
        m[row][3] = p[row]
    return m


def inverse(m: Matrix) -> Matrix:
    """The inverse of an affine matrix (rotation, scale and translation)."""
    a = [row[:3] for row in m[:3]]
    det = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
           - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
           + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    if abs(det) < 1e-12:
        raise ValueError("a transform with a scale of 0 can't be undone")
    inv = [[(a[(c + 1) % 3][(r + 1) % 3] * a[(c + 2) % 3][(r + 2) % 3]
             - a[(c + 1) % 3][(r + 2) % 3] * a[(c + 2) % 3][(r + 1) % 3]) / det
            for c in range(3)] for r in range(3)]
    out = identity()
    for r in range(3):
        out[r][:3] = inv[r]
        out[r][3] = -sum(inv[r][k] * m[k][3] for k in range(3))
    return out


def _clean(v: float) -> float:
    v = round(v, 4)
    return 0.0 if v == 0 else v  # no -0.0


def decompose(m: Matrix) -> dict:
    """A transform component for a matrix: position, rotation and (positive) scale."""
    scale = [math.sqrt(sum(m[r][c] ** 2 for r in range(3))) for c in range(3)]
    r = [[m[row][col] / (scale[col] or 1.0) for col in range(3)] for row in range(3)]
    return {"position": [_clean(m[i][3]) for i in range(3)],
            "rotation": [_clean(a) for a in euler(r)],
            "scale": [_clean(s) for s in scale]}


def world_matrix(entities: dict[str, Entity], entity_id: str | None) -> Matrix:
    """An entity's matrix in the world: its parents' matrices times its own (identity for
    None, the top of the scene)."""
    m, seen = identity(), set()
    while entity_id is not None and entity_id in entities and entity_id not in seen:
        seen.add(entity_id)
        entity = entities[entity_id]
        m = multiply(compose(entity.components.get("transform", {})), m)
        entity_id = entity.parent
    return m


def position_of(m: Matrix) -> list[float]:
    return [m[0][3], m[1][3], m[2][3]]


def relative_to(parent: Matrix, world: Matrix) -> dict:
    """The transform that puts something at `world` when its parent is at `parent`."""
    return decompose(multiply(inverse(parent), world))
