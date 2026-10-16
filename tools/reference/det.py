# SPDX-License-Identifier: Apache-2.0
"""Deterministic maths (engine/core/include/midgard/det.h, engine/core/src/det.cpp), step by step.

Python's ints are unbounded, so 64-bit wrapping is written out with MASK; its >> on negative
numbers rounds down, as C++20's does; and its floats are IEEE doubles, so +, -, *, / and sqrt
give the same bits as the engine when done in the same order.
"""
import math

MASK = (1 << 64) - 1
GOLDEN = 0x9E3779B97F4A7C15
FRAC_BITS = 16
ONE = 1 << FRAC_BITS


def mix64(z: int) -> int:
    z = (z + GOLDEN) & MASK
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK
    return z ^ (z >> 31)


def hash3(seed: int, x: int, y: int = 0, z: int = 0) -> int:
    h = mix64(seed & MASK)
    h = mix64(h ^ (x & MASK))
    h = mix64(h ^ (y & MASK))
    return mix64(h ^ (z & MASK))


class Rng:
    def __init__(self, seed: int):
        self.state = seed & MASK

    def next(self) -> int:
        self.state = (self.state + GOLDEN) & MASK
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK
        return z ^ (z >> 31)

    def split(self, tag: int) -> "Rng":
        return Rng(mix64(self.state ^ mix64(tag & MASK)))

    def below(self, n: int) -> int:
        threshold = ((1 << 64) - n) % n
        r = self.next()
        while r < threshold:
            r = self.next()
        return r % n

    def uniform(self) -> float:
        return (self.next() >> 11) * 2.0 ** -53


def tag(name: str) -> int:
    h = 0xCBF29CE484222325
    for byte in name.encode("utf-8"):
        h = ((h ^ byte) * 0x100000001B3) & MASK
    return h


def lattice(x: float) -> tuple[int, int]:
    cell = math.floor(x)
    frac = int((x - cell) * 65536.0)
    return cell, min(frac, ONE - 1)


def fade(t: int) -> int:
    t2 = (t * t) >> FRAC_BITS
    t3 = (t2 * t) >> FRAC_BITS
    inner = ((t * (6 * t - 15 * ONE)) >> FRAC_BITS) + 10 * ONE
    return (t3 * inner) >> FRAC_BITS


def lerp(a: int, b: int, t: int) -> int:
    return a + (((b - a) * t) >> FRAC_BITS)


GRADIENTS = [(1, 1, 0), (-1, 1, 0), (1, -1, 0), (-1, -1, 0), (1, 0, 1), (-1, 0, 1),
             (1, 0, -1), (-1, 0, -1), (0, 1, 1), (0, -1, 1), (0, 1, -1), (0, -1, -1)]


def _trilinear(corners: list[int], u: int, v: int, w: int) -> int:
    x00, x10 = lerp(corners[0], corners[1], u), lerp(corners[2], corners[3], u)
    x01, x11 = lerp(corners[4], corners[5], u), lerp(corners[6], corners[7], u)
    return lerp(lerp(x00, x10, v), lerp(x01, x11, v), w)


def gradient_noise(seed: int, x: float, y: float, z: float) -> int:
    (cx, fx), (cy, fy), (cz, fz) = lattice(x), lattice(y), lattice(z)
    corners = []
    for i in range(8):
        ox, oy, oz = i & 1, (i >> 1) & 1, (i >> 2) & 1
        g = GRADIENTS[hash3(seed, cx + ox, cy + oy, cz + oz) % 12]
        corners.append(g[0] * (fx - ox * ONE) + g[1] * (fy - oy * ONE) + g[2] * (fz - oz * ONE))
    return _trilinear(corners, fade(fx), fade(fy), fade(fz))


def value_noise(seed: int, x: float, y: float, z: float) -> int:
    (cx, fx), (cy, fy), (cz, fz) = lattice(x), lattice(y), lattice(z)
    corners = [(hash3(seed, cx + (i & 1), cy + ((i >> 1) & 1), cz + ((i >> 2) & 1)) & 0x1FFFF)
               - ONE for i in range(8)]
    return _trilinear(corners, fade(fx), fade(fy), fade(fz))


def fbm(seed: int, x: float, y: float, z: float, octaves: int) -> int:
    total, frequency = 0, 1.0
    for o in range(octaves):
        total += gradient_noise(hash3(seed, o), x * frequency, y * frequency,
                                z * frequency) >> o
        frequency *= 2.0
    return total


TWO_OVER_PI = 0.63661977236758134308
PIO2_HI = 1.57079632673412561417e+00
PIO2_LO = 6.07710050650619224932e-11


def _sin_kernel(r: float) -> float:
    r2 = r * r
    p = 1.0 / 1307674368000.0
    p = p * r2 - 1.0 / 6227020800.0
    p = p * r2 + 1.0 / 39916800.0
    p = p * r2 - 1.0 / 362880.0
    p = p * r2 + 1.0 / 5040.0
    p = p * r2 - 1.0 / 120.0
    p = p * r2 + 1.0 / 6.0
    return r - r * r2 * p


def _cos_kernel(r: float) -> float:
    r2 = r * r
    p = 1.0 / 20922789888000.0
    p = p * r2 - 1.0 / 87178291200.0
    p = p * r2 + 1.0 / 479001600.0
    p = p * r2 - 1.0 / 3628800.0
    p = p * r2 + 1.0 / 40320.0
    p = p * r2 - 1.0 / 720.0
    p = p * r2 + 1.0 / 24.0
    p = p * r2 - 1.0 / 2.0
    return 1.0 + r2 * p


def _reduce(x: float) -> tuple[float, int]:
    k = float(round(x * TWO_OVER_PI))  # Python rounds half to even, as nearbyint does
    return (x - k * PIO2_HI) - k * PIO2_LO, int(k) & 3


def sin(x: float) -> float:
    r, q = _reduce(x)
    return (_sin_kernel(r), _cos_kernel(r), -_sin_kernel(r), -_cos_kernel(r))[q]


def cos(x: float) -> float:
    r, q = _reduce(x)
    return (_cos_kernel(r), -_sin_kernel(r), -_cos_kernel(r), _sin_kernel(r))[q]


def tan(x: float) -> float:
    return sin(x) / cos(x)


TAN_PI12 = 0.26794919243112270
SQRT3 = 1.7320508075688772
PI6 = 0.52359877559829887
PI2 = 1.5707963267948966
PI2_LO = 6.123233995736766e-17
PI = 3.141592653589793
PI_LO = 1.2246467991473532e-16


def _atan_kernel(x: float) -> float:
    x2 = x * x
    p = 1.0 / 29.0
    for k in range(27, 0, -2):
        p = 1.0 / k - x2 * p
    return x * p


def atan(x: float) -> float:
    negative = x < 0.0
    a = -x if negative else x
    inverted = a > 1.0
    if inverted:
        a = 1.0 / a
    shifted = a > TAN_PI12
    if shifted:
        a = (a * SQRT3 - 1.0) / (a + SQRT3)
    r = _atan_kernel(a)
    if shifted:
        r = PI6 + r
    if inverted:
        r = (PI2 - r) + PI2_LO
    return -r if negative else r


def atan2(y: float, x: float) -> float:
    if x > 0.0:
        return atan(y / x)
    if x < 0.0:
        a = atan(y / x)
        return (a + PI) + PI_LO if y >= 0.0 else (a - PI) - PI_LO
    if y > 0.0:
        return PI2
    if y < 0.0:
        return -PI2
    return 0.0


def sqrt(x: float) -> float:
    return math.sqrt(x)
