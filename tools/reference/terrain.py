# SPDX-License-Identifier: Apache-2.0
"""Terrain (engine/core/include/midgard/terrain.h, engine/core/src/terrain.cpp), step by step.

The ground's height at a point (face, u, v) of a world, from noise at 3D positions on the
shape's surface (det.fbm), then shaped by the shape's edges: a bounded area sinks into a band of
open ocean all round, and a cylinder rises towards walls at its ends. A flat-wrap cylinder
samples its noise on the circle it would be if it were curved, so its edges meet.

Chunks: each face is cut into roots about ROOT_SIZE metres across, and each root into a
quadtree; a chunk at level L is a grid of CHUNK_QUADS x CHUNK_QUADS quads, its points numbered
across the whole face so every chunk that has a point computes it from the same (u, v).

The order of operations is the specification, so heights are bit-identical everywhere.
make_table() writes the shared table schema/tests/terrain_chunks.json (made once, then fixed).
"""
import json
import math
import struct
from pathlib import Path

from . import det, shapes
from .hashing import fnv1a64

CHUNK_QUADS = 32
ROOT_SIZE = 1024.0
MAX_LEVEL = 12
TWO_PI = 6.283185307179586
DEFAULTS = {"base": 8.0, "height_scale": 60.0, "feature_size": 500.0, "octaves": 6,
            "sea_level": 0.0, "ocean_band": 300.0, "ocean_depth": 30.0, "end_ramp": 150.0,
            "wall_height": 120.0, "wall": "cliff", "spacing": 2.0}
KINDS = {"bounded_area": shapes.BOUNDED_AREA, "flat_cylinder": shapes.FLAT_CYLINDER,
         "curved_cylinder": shapes.CURVED_CYLINDER, "sphere_outside": shapes.SPHERE_OUTSIDE,
         "sphere_inside": shapes.SPHERE_INSIDE}
TABLES = Path(__file__).resolve().parents[2] / "schema" / "tests"


def make_shape(body: dict) -> shapes.Shape:
    kind = KINDS[body["shape"]]
    if kind == shapes.BOUNDED_AREA:
        return shapes.Shape(kind, body["width"], body["length"], 9.81)
    if kind == shapes.FLAT_CYLINDER:
        return shapes.Shape(kind, body["circumference"], body["length"], 9.81)
    if kind == shapes.CURVED_CYLINDER:
        return shapes.Shape(kind, body["radius"], body["length"], 9.81)
    return shapes.Shape(kind, body["radius"], 0.0, 9.81)


def face_size(shape: shapes.Shape) -> tuple[float, float]:
    if shape.kind == shapes.CURVED_CYLINDER:
        return TWO_PI * shape.size_u, shape.size_v
    if shape.kind in (shapes.SPHERE_OUTSIDE, shapes.SPHERE_INSIDE):
        return TWO_PI / 4.0 * shape.size_u, TWO_PI / 4.0 * shape.size_u
    return shape.size_u, shape.size_v


def stamp_from_json(j: dict) -> dict:
    """A terrain stamp with the engine's defaults (midgard/terrain.h's TerrainStamp)."""
    return {"tool": j.get("tool", "raise"), "paint": j.get("paint", "lush"),
            "radius": float(j.get("radius", 10.0)), "strength": float(j.get("strength", 2.0)),
            "height": float(j.get("height", 0.0)), "face": int(j.get("face", 0)),
            "points": [(float(p[0]), float(p[1])) for p in j["points"]]}


def smooth01(t: float) -> float:
    t = min(max(t, 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


class Terrain:
    def __init__(self, body: dict, settings: dict | None = None,
                 stamps: list[dict] | None = None):
        self.shape = make_shape(body)
        self.settings = {**DEFAULTS, **(settings or {})}
        self.stamps = [stamp_from_json(s) for s in (stamps or [])]
        self.seed = self.settings.get("seed", body.get("seed", 0))
        su, sv = face_size(self.shape)
        if self.shape.kind in (shapes.SPHERE_OUTSIDE, shapes.SPHERE_INSIDE):
            # A power of two a side, so face-edge grid points are bit-identical from both faces.
            roots = 1
            while roots < (1 << 16) and 2.0 * roots <= su / ROOT_SIZE * 1.4142135623730951:
                roots *= 2
            self.roots_u = self.roots_v = roots
        else:
            self.roots_u = max(1, int(math.floor(su / ROOT_SIZE + 0.5)))
            self.roots_v = max(1, int(math.floor(sv / ROOT_SIZE + 0.5)))
        root = max(su / self.roots_u, sv / self.roots_v)
        spacing = max(self.settings["spacing"], 0.25)
        self.max_level = 0
        while (self.max_level < MAX_LEVEL
               and root / (float(1 << self.max_level) * CHUNK_QUADS) > spacing):
            self.max_level += 1

    def wraps(self) -> bool:
        return self.shape.kind in (shapes.FLAT_CYLINDER, shapes.CURVED_CYLINDER)

    def noise_position(self, face: int, u: float, v: float) -> tuple[float, float, float]:
        if self.shape.kind == shapes.FLAT_CYLINDER:
            su, sv = face_size(self.shape)
            r = su / TWO_PI
            angle = TWO_PI * shapes.wrap01(u)
            return (r * det.sin(angle), -(r * det.cos(angle)), (v - 0.5) * sv)
        return self.shape.position(face, u, v, 0.0)

    def stamp_weight(self, s: dict, face: int, u: float, v: float) -> float:
        """How much a stamp applies at (u, v): 1 on its line, falling to 0 at its radius."""
        if s["face"] != face or not s["points"] or not s["radius"] > 0.0:
            return 0.0
        su, sv = face_size(self.shape)
        round_ = self.wraps()

        def metres(p):
            du = p[0] - u
            if round_:
                du = du - math.floor(du + 0.5)
            return du * su, (p[1] - v) * sv

        ax, ay = metres(s["points"][0])
        best = ax * ax + ay * ay
        for p in s["points"][1:]:
            bx, by = metres(p)
            ex, ey = bx - ax, by - ay
            len2 = ex * ex + ey * ey
            t = -(ax * ex + ay * ey) / len2 if len2 > 0.0 else 0.0
            t = min(max(t, 0.0), 1.0)
            cx, cy = ax + ex * t, ay + ey * t
            best = min(best, cx * cx + cy * cy)
            ax, ay = bx, by
        return smooth01(1.0 - det.sqrt(best) / s["radius"])

    def height(self, face: int, u: float, v: float) -> float:
        """With the stamps (sculpting), in order."""
        h = self.base_height(face, u, v)
        su, sv = face_size(self.shape)
        for s in self.stamps:
            if s["tool"] == "paint" or s["face"] != face:
                continue
            w = self.stamp_weight(s, face, u, v)
            if w <= 0.0:
                continue
            if s["tool"] == "raise":
                h = h + s["strength"] * w
            elif s["tool"] == "lower":
                h = h - s["strength"] * w
            elif s["tool"] == "flatten":
                h = h + (s["height"] - h) * (w * s["strength"])
            elif s["tool"] == "smooth":
                du, dv = 0.5 * s["radius"] / su, 0.5 * s["radius"] / sv
                average = (self.base_height(face, u + du, v) + self.base_height(face, u - du, v)
                           + self.base_height(face, u, v + dv)
                           + self.base_height(face, u, v - dv)) * 0.25
                h = h + (average - h) * (w * s["strength"])
        return h

    def base_height(self, face: int, u: float, v: float) -> float:
        """As generated."""
        t = self.settings
        q = self.noise_position(face, u, v)
        f = t["feature_size"]
        n = det.fbm(self.seed, q[0] / f, q[1] / f, q[2] / f, t["octaves"])
        h = t["base"] + t["height_scale"] * (float(n) / 65536.0)
        su, sv = face_size(self.shape)
        if self.shape.kind == shapes.BOUNDED_AREA:
            if t["ocean_band"] > 0.0:
                d = min(min(u * su, (1.0 - u) * su), min(v * sv, (1.0 - v) * sv))
                s = smooth01((d - t["ocean_band"]) / t["ocean_band"])
                h = -t["ocean_depth"] + (h + t["ocean_depth"]) * s
        elif self.wraps() and t["end_ramp"] > 0.0:
            e = min(v * sv, (1.0 - v) * sv)
            s = smooth01(1.0 - e / t["end_ramp"])
            h = h + (t["wall_height"] - h) * s
        return h

    def points(self, level: int) -> tuple[int, int]:
        return (self.roots_u << level) * CHUNK_QUADS, (self.roots_v << level) * CHUNK_QUADS

    def heights(self, key: tuple[int, int, int, int]) -> list[float]:
        """A chunk's (CHUNK_QUADS + 1)^2 heights, row by row along v."""
        face, level, x, y = key
        pu, pv = self.points(level)
        out = []
        for j in range(CHUNK_QUADS + 1):
            v = float(y * CHUNK_QUADS + j) / float(pv)
            for i in range(CHUNK_QUADS + 1):
                out.append(self.height(face, float(x * CHUNK_QUADS + i) / float(pu), v))
        return out

    def chunk_hash(self, key: tuple[int, int, int, int]) -> int:
        data = struct.pack("<iiii", *key)
        data += b"".join(struct.pack("<d", h) for h in self.heights(key))
        return fnv1a64(data)


# The terrains of the shared table and the self-test's fingerprint (terrain_fingerprint()).
BODIES = {
    "area": {"shape": "bounded_area", "width": 3000, "length": 2000, "seed": 7},
    "strip": {"shape": "flat_cylinder", "circumference": 6000, "length": 5000, "seed": 8},
    "habitat": {"shape": "curved_cylinder", "radius": 1000, "length": 5000, "seed": 9},
    "planet": {"shape": "sphere_outside", "radius": 1000, "seed": 10},
}
FINGERPRINT_KEYS = [(0, 0, 0, 0), (0, 2, 3, 1)]


def fingerprint() -> int:
    data = b""
    for body in BODIES.values():
        t = Terrain(body)
        for key in FINGERPRINT_KEYS:
            data += struct.pack("<Q", t.chunk_hash(key))
    return fnv1a64(data)


STAMPS = [  # sculpting for the table of stamps (terrain_stamps.json)
    {"tool": "raise", "radius": 40.0, "strength": 12.0,
     "points": [[0.40, 0.45], [0.43, 0.47], [0.47, 0.46]]},
    {"tool": "lower", "radius": 25.0, "strength": 6.0, "points": [[0.52, 0.50]]},
    {"tool": "flatten", "radius": 30.0, "strength": 0.9, "height": 15.0,
     "points": [[0.45, 0.55], [0.50, 0.56]]},
    {"tool": "smooth", "radius": 35.0, "strength": 0.7, "points": [[0.55, 0.48], [0.58, 0.44]]},
    {"tool": "paint", "paint": "rock", "radius": 20.0, "strength": 1.0, "points": [[0.4, 0.4]]},
    {"tool": "raise", "radius": 60.0, "strength": 20.0, "points": [[0.999, 0.3], [0.002, 0.31]]},
]


def make_stamp_table() -> dict:
    """The shared table schema/tests/terrain_stamps.json (made once, then fixed)."""
    cases = []
    for name in ("area", "strip", "habitat"):
        t = Terrain(BODIES[name], {}, STAMPS)
        for u, v in ((0.43, 0.47), (0.41, 0.455), (0.52, 0.50), (0.47, 0.555), (0.565, 0.46),
                     (0.0, 0.305), (0.9995, 0.3), (0.3, 0.3), (0.4, 0.4)):
            cases.append({"body": name, "u": u, "v": v, "height": t.height(0, u, v)})
        level = 2
        key = (0, level, int(0.43 * (t.roots_u << level)), int(0.47 * (t.roots_v << level)))
        cases.append({"body": name, "key": list(key), "hash": f"{t.chunk_hash(key):016x}"})
    return {"_about": "Terrain sculpting: heights and a chunk hash with stamps "
                      "(tools/reference/terrain.py).",
            "bodies": BODIES, "stamps": STAMPS, "cases": cases}


def make_table() -> dict:
    """The shared table schema/tests/terrain_chunks.json (made once, then fixed)."""
    settings = {"default": {},
                "rough": {"base": -5.0, "height_scale": 200.0, "feature_size": 120.0,
                          "octaves": 4, "ocean_band": 100.0, "end_ramp": 400.0, "seed": 1234}}
    cases = []
    for name, body in BODIES.items():
        for sname, s in settings.items():
            t = Terrain(body, s)
            keys = [(0, 0, 0, 0), (0, 2, 3, 1), (0, 1, (t.roots_u << 1) - 1, 0)]
            for key in keys:
                cases.append({"body": name, "settings": sname, "key": list(key),
                              "hash": f"{t.chunk_hash(key):016x}"})
            for u, v in ((0.0, 0.0), (0.5, 0.5), (0.999, 0.03), (0.25, 0.97), (1.0, 0.4),
                         (0.013, 0.6)):
                cases.append({"body": name, "settings": sname, "u": u, "v": v,
                              "height": t.height(0, u, v)})
    return {"_about": "Terrain heights and chunk hashes (tools/reference/terrain.py).",
            "bodies": BODIES, "settings": settings,
            "layout": {name: [Terrain(b).roots_u, Terrain(b).roots_v, Terrain(b).max_level]
                       for name, b in BODIES.items()},
            "fingerprint": f"{fingerprint():016x}", "cases": cases}


SPHERES = {  # the cube-sphere's terrains (terrain_spheres.json)
    "world": {"shape": "sphere_outside", "radius": 3000, "seed": 11},
    "hollow": {"shape": "sphere_inside", "radius": 2500, "seed": 12},
}


def make_sphere_table() -> dict:
    """The shared table schema/tests/terrain_spheres.json (made once, then fixed): chunk
    hashes on every face, at the face's corners, edges and middle, at three levels."""
    cases = []
    for name, body in SPHERES.items():
        t = Terrain(body)
        for face in range(6):
            for level in (0, 1, 3):
                last = (t.roots_u << level) - 1
                for x, y in ((0, 0), (last, last), (last // 2, 0), (0, last // 2)):
                    key = (face, level, x, y)
                    cases.append({"body": name, "key": list(key),
                                  "hash": f"{t.chunk_hash(key):016x}"})
            for u, v in ((0.0, 0.0), (1.0, 0.5), (0.5, 1.0), (0.37, 0.61)):
                cases.append({"body": name, "face": face, "u": u, "v": v,
                              "height": t.height(face, u, v)})
    return {"_about": "Cube-sphere terrain: heights and chunk hashes on every face "
                      "(tools/reference/terrain.py).",
            "bodies": SPHERES,
            "layout": {name: [Terrain(b).roots_u, Terrain(b).roots_v, Terrain(b).max_level]
                       for name, b in SPHERES.items()},
            "cases": cases}


if __name__ == "__main__":
    import sys
    which = sys.argv[1] if len(sys.argv) > 1 else "chunks"
    if which == "stamps":
        out, table = TABLES / "terrain_stamps.json", make_stamp_table()
    elif which == "spheres":
        out, table = TABLES / "terrain_spheres.json", make_sphere_table()
    else:
        out, table = TABLES / "terrain_chunks.json", make_table()
    out.write_text(json.dumps(table, indent=1) + "\n", encoding="utf-8", newline="\n")
    print("wrote", out)
