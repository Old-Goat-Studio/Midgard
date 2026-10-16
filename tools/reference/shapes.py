# SPDX-License-Identifier: Apache-2.0
"""World shapes (engine/core/include/midgard/shape.h, engine/core/src/shape.cpp), step by step.

A surface point is (face, u, v, height): u and v run from 0 to 1 across a face, and height is in
metres along "up". Flat shapes and cylinders have one face; spheres have six, the faces of a
cube projected onto the sphere ("cube-sphere"), with a tangent warp that evens out their area.

    bounded area        x = (u - 1/2) W, z = (v - 1/2) L, y = height; up +Y
    flat-wrap cylinder  the same, with u wrapping (x every C metres)
    curved cylinder     the inside of a tube along Z: u around (angle 2 pi u, 0 at the bottom),
                        v along; height towards the axis; spin gravity g r / R outwards
    sphere outside      direction from the cube face, times R + height; gravity towards the centre
    sphere inside       times R - height; up and height point to the centre, gravity outwards

The order of operations is the specification, so positions are bit-identical everywhere.
"""
import math

from . import det

BOUNDED_AREA, FLAT_CYLINDER, CURVED_CYLINDER, SPHERE_OUTSIDE, SPHERE_INSIDE = range(5)
KIND_NAMES = ["bounded_area", "flat_cylinder", "curved_cylinder", "sphere_outside",
              "sphere_inside"]

# Cube faces as (axis, sign) for the normal n, the u direction t and the v direction b, with
# t x b = n. A cube point is c[n] = n_sign, c[t] = t_sign * warp(2u - 1), c[b] = b_sign * ...
FACES = [
    ((0, 1.0), (2, -1.0), (1, 1.0)),   # +X
    ((0, -1.0), (2, 1.0), (1, 1.0)),   # -X
    ((1, 1.0), (0, 1.0), (2, -1.0)),   # +Y
    ((1, -1.0), (0, 1.0), (2, 1.0)),   # -Y
    ((2, 1.0), (0, 1.0), (1, 1.0)),    # +Z
    ((2, -1.0), (0, -1.0), (1, 1.0)),  # -Z
]
QUARTER_PI = 0.7853981633974483
FOUR_OVER_PI = 1.2732395447351628
TWO_PI = 6.283185307179586


def _zero(x: float) -> float:
    return 0.0 if x == 0.0 else x  # -0.0 becomes 0.0, so equal points have equal bits


def warp(a: float) -> float:
    """[-1, 1] to the cube face's coordinate. The edges are exactly +-1 (tan(pi/4) rounds)."""
    if a == 1.0:
        return 1.0
    if a == -1.0:
        return -1.0
    return det.tan(QUARTER_PI * a)


def unwarp(w: float) -> float:
    if w >= 1.0:
        return 1.0
    if w <= -1.0:
        return -1.0
    return det.atan(w) * FOUR_OVER_PI


def wrap01(u: float) -> float:
    return u - math.floor(u)


class Shape:
    def __init__(self, kind: int, size_u: float, size_v: float, gravity: float):
        """Bounded area / flat cylinder: size_u, size_v are the width (or circumference) and
        length. Curved cylinder: radius and length. Spheres: radius (size_v unused)."""
        self.kind, self.size_u, self.size_v, self.g = kind, size_u, size_v, gravity

    @property
    def faces(self) -> int:
        return 6 if self.kind in (SPHERE_OUTSIDE, SPHERE_INSIDE) else 1

    def position(self, face: int, u: float, v: float, h: float) -> tuple[float, float, float]:
        k = self.kind
        if k in (BOUNDED_AREA, FLAT_CYLINDER):
            if k == FLAT_CYLINDER:
                u = wrap01(u)
            return (_zero((u - 0.5) * self.size_u), _zero(h), _zero((v - 0.5) * self.size_v))
        if k == CURVED_CYLINDER:
            u = wrap01(u)
            angle = TWO_PI * u
            r = self.size_u - h
            return (_zero(r * det.sin(angle)), _zero(-(r * det.cos(angle))),
                    _zero((v - 0.5) * self.size_v))
        d = cube_direction(face, u, v)
        r = self.size_u + h if k == SPHERE_OUTSIDE else self.size_u - h
        return (_zero(d[0] * r), _zero(d[1] * r), _zero(d[2] * r))

    def surface(self, p: tuple[float, float, float]) -> tuple[int, float, float, float]:
        x, y, z = p
        k = self.kind
        if k in (BOUNDED_AREA, FLAT_CYLINDER):
            u = x / self.size_u + 0.5
            if k == FLAT_CYLINDER:
                u = wrap01(u)
            return 0, u, z / self.size_v + 0.5, y
        if k == CURVED_CYLINDER:
            u = det.atan2(x, -y) / TWO_PI
            if u < 0.0:
                u = u + 1.0
            return 0, u, z / self.size_v + 0.5, self.size_u - det.sqrt(x * x + y * y)
        r = det.sqrt(x * x + y * y + z * z)
        face, u, v = cube_surface((x / r, y / r, z / r))
        h = r - self.size_u if k == SPHERE_OUTSIDE else self.size_u - r
        return face, u, v, h

    def up(self, p: tuple[float, float, float]) -> tuple[float, float, float]:
        x, y, z = p
        k = self.kind
        if k in (BOUNDED_AREA, FLAT_CYLINDER):
            return (0.0, 1.0, 0.0)
        if k == CURVED_CYLINDER:
            r = det.sqrt(x * x + y * y)
            return (0.0, 1.0, 0.0) if r == 0.0 else (_zero(-x / r), _zero(-y / r), 0.0)
        r = det.sqrt(x * x + y * y + z * z)
        s = 1.0 if k == SPHERE_OUTSIDE else -1.0
        return (_zero(s * x / r), _zero(s * y / r), _zero(s * z / r))

    def gravity(self, p: tuple[float, float, float]) -> tuple[float, float, float]:
        x, y, z = p
        k = self.kind
        if k == CURVED_CYLINDER:  # spin: g r / R, away from the axis
            scale = self.g / self.size_u
            return (_zero(x * scale), _zero(y * scale), 0.0)
        ux, uy, uz = self.up(p)
        return (_zero(-ux * self.g), _zero(-uy * self.g), _zero(-uz * self.g))


def cube_direction(face: int, u: float, v: float) -> tuple[float, float, float]:
    (na, ns), (ta, ts), (ba, bs) = FACES[face]
    c = [0.0, 0.0, 0.0]
    c[na] = ns
    c[ta] = ts * warp(2.0 * u - 1.0)
    c[ba] = bs * warp(2.0 * v - 1.0)
    length = det.sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2])
    return (c[0] / length, c[1] / length, c[2] / length)


def cube_surface(d: tuple[float, float, float]) -> tuple[int, float, float]:
    """The face a direction falls on (ties go to the lower-numbered face) and its u, v."""
    best, face = -1.0, 0
    for f, ((na, ns), _t, _b) in enumerate(FACES):
        value = d[na] * ns
        if value > best:
            best, face = value, f
    (na, ns), (ta, ts), (ba, bs) = FACES[face]
    scale = d[na] * ns
    wu = d[ta] * ts / scale
    wv = d[ba] * bs / scale
    return face, (unwarp(wu) + 1.0) * 0.5, (unwarp(wv) + 1.0) * 0.5
