# SPDX-License-Identifier: Apache-2.0
"""World generation graphs (midgard/worldgen.h's twin; milestone 5.11): a world type's ground
height at a point, from a graph of nodes. The same operations in the same order as the C++, so
schema/tests/worldgen_cases.json holds both to the same results, to the bit.

    python -m tools.reference.worldgen      writes schema/tests/worldgen_cases.json (once)
"""
import json
import math
import struct

from . import det, shapes
from .hashing import fnv1a64
from .terrain import TABLES, TWO_PI, face_size, make_shape

DEGREES = 57.29577951308232
FAR = 1e30
TWO_INPUTS = {"subtract", "divide"}
MANY_INPUTS = {"add", "multiply", "min", "max"}
FILTERS = {"smooth", "erode"}


def smooth01(t: float) -> float:
    t = min(max(t, 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


def noise_position(shape, face: int, u: float, v: float):
    if shape.kind == shapes.FLAT_CYLINDER:
        su, sv = face_size(shape)
        r = su / TWO_PI
        angle = TWO_PI * shapes.wrap01(u)
        return (r * det.sin(angle), -(r * det.cos(angle)), (v - 0.5) * sv)
    return shape.position(face, u, v, 0.0)


def offset(shape, place, east: float, north: float):
    """A place `east` and `north` metres along the ground (graph_offset)."""
    face, u, v = place
    su, sv = face_size(shape)
    if shape.kind in (shapes.BOUNDED_AREA, shapes.FLAT_CYLINDER, shapes.CURVED_CYLINDER):
        return face, u + east / su, v + north / sv
    d = shapes.cube_direction(face, u, v)
    axis = (0.0, 1.0, 0.0) if abs(d[1]) < 0.9 else (1.0, 0.0, 0.0)
    e = (axis[1] * d[2] - axis[2] * d[1], axis[2] * d[0] - axis[0] * d[2],
         axis[0] * d[1] - axis[1] * d[0])
    el = det.sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2])
    e = (e[0] / el, e[1] / el, e[2] / el)
    n = (d[1] * e[2] - d[2] * e[1], d[2] * e[0] - d[0] * e[2], d[0] * e[1] - d[1] * e[0])
    r = shape.size_u
    q = (d[0] * r + e[0] * east + n[0] * north, d[1] * r + e[1] * east + n[1] * north,
         d[2] * r + e[2] * east + n[2] * north)
    ql = det.sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2])
    f, uu, vv = shapes.cube_surface((q[0] / ql, q[1] / ql, q[2] / ql))
    return f, uu, vv


class Graph:
    """A graph from its JSON ({"nodes": {...}, "output": id}); evaluate() as the engine does."""

    def __init__(self, j: dict):
        self.nodes = j["nodes"]
        self.output = j["output"]

    def evaluate(self, shape, seed: int, place) -> float:
        return self._eval(self.output, shape, seed, place, {})

    def _input(self, item, shape, seed, place, memo):
        if isinstance(item, (int, float)):
            return float(item)
        return self._eval(item, shape, seed, place, memo)

    def _around(self, item, shape, seed, place, radius):
        total = 0.0
        for east, north in ((radius, 0.0), (-radius, 0.0), (0.0, radius), (0.0, -radius)):
            total = total + self._input(item, shape, seed, offset(shape, place, east, north), {})
        return total * 0.25

    def _eval(self, node_id, shape, seed, place, memo):
        if node_id in memo:
            return memo[node_id]
        n = self.nodes[node_id]
        op = n["op"]
        ins = n.get("in", [])

        def at(k):
            return self._input(ins[k], shape, seed, place, memo)

        face, u, v = place
        if op in ("noise", "ridged"):
            q = noise_position(shape, face, u, v)
            size = float(n["size"])
            salt = det.tag(n["salt"]) if "salt" in n else 0
            noise = float(det.fbm(seed ^ salt, q[0] / size, q[1] / size, q[2] / size,
                                  int(n.get("octaves", 6)))) / 65536.0
            r = noise if op == "noise" else 1.0 - abs(noise)
        elif op == "constant":
            r = float(n["value"])
        elif op == "input":
            su, sv = face_size(shape)
            name = n["name"]
            r = 0.0
            if name == "latitude":
                if shape.kind in (shapes.SPHERE_OUTSIDE, shapes.SPHERE_INSIDE):
                    d = shapes.cube_direction(face, u, v)
                    r = det.atan2(d[1], det.sqrt(d[0] * d[0] + d[2] * d[2])) * DEGREES
            elif name == "edge_distance":
                r = (min(min(u * su, (1.0 - u) * su), min(v * sv, (1.0 - v) * sv))
                     if shape.kind == shapes.BOUNDED_AREA else FAR)
            elif name == "end_distance":
                r = (min(v * sv, (1.0 - v) * sv)
                     if shape.kind in (shapes.FLAT_CYLINDER, shapes.CURVED_CYLINDER) else FAR)
        elif op in MANY_INPUTS:
            r = at(0)
            for k in range(1, len(ins)):
                x = at(k)
                if op == "add":
                    r = r + x
                elif op == "multiply":
                    r = r * x
                elif op == "min":
                    r = min(r, x)
                else:
                    r = max(r, x)
        elif op == "subtract":
            r = at(0) - at(1)
        elif op == "divide":
            b = at(1)
            r = 0.0 if b == 0.0 else at(0) / b
        elif op == "lerp":
            a, b, t = at(0), at(1), at(2)
            r = a + (b - a) * t
        elif op == "abs":
            r = abs(at(0))
        elif op == "clamp":
            r = min(max(at(0), float(n.get("min", -1e30))), float(n.get("max", 1e30)))
        elif op == "smoothstep":
            lo, hi = float(n["from"]), float(n["to"])
            r = smooth01((at(0) - lo) / (hi - lo))
        elif op == "curve":
            x = at(0)
            pts = [(float(p[0]), float(p[1])) for p in n["points"]]
            if x <= pts[0][0]:
                r = pts[0][1]
            elif x >= pts[-1][0]:
                r = pts[-1][1]
            else:
                k = 0
                while not x < pts[k + 1][0]:
                    k += 1
                (ax, ay), (bx, by) = pts[k], pts[k + 1]
                r = ay + (by - ay) * ((x - ax) / (bx - ax))
        elif op == "terrace":
            step, sharp = float(n["step"]), float(n.get("sharpness", 1.0))
            s = at(0) / step
            k = float(math.floor(s))
            f = s - k
            g = f * f * (3.0 - 2.0 * f)
            r = (k + (f + (g - f) * sharp)) * step
        elif op == "smooth":
            r = self._around(ins[0], shape, seed, place, float(n["radius"]))
        elif op == "erode":
            x = at(0)
            r = x - float(n.get("strength", 0.5)) * max(
                0.0, x - self._around(ins[0], shape, seed, place, float(n["radius"])))
        else:
            raise ValueError(f"no node {op!r}")
        memo[node_id] = r
        return r


# The graphs and shapes of the shared table.
BODIES = {
    "area": {"shape": "bounded_area", "width": 3000, "length": 2000},
    "strip": {"shape": "flat_cylinder", "circumference": 6000, "length": 5000},
    "habitat": {"shape": "curved_cylinder", "radius": 1000, "length": 5000},
    "planet": {"shape": "sphere_outside", "radius": 3000},
    "hollow": {"shape": "sphere_inside", "radius": 2500},
}
GRAPHS = {
    # The 5.9 ground's noise term as a graph: base + height_scale * noise.
    "classic": {"output": "height", "nodes": {
        "hills": {"op": "noise", "size": 500, "octaves": 6},
        "scaled": {"op": "multiply", "in": [60, "hills"]},
        "height": {"op": "add", "in": [8, "scaled"]}}},
    # Every kind of node at once.
    "everything": {"output": "out", "nodes": {
        "n": {"op": "noise", "size": 300, "octaves": 4, "salt": "base"},
        "r": {"op": "ridged", "size": 900, "octaves": 3, "salt": "ridges"},
        "lat": {"op": "input", "name": "latitude"},
        "edge": {"op": "input", "name": "edge_distance"},
        "end": {"op": "input", "name": "end_distance"},
        "near_edge": {"op": "smoothstep", "in": ["edge"], "from": 0, "to": 400},
        "near_end": {"op": "smoothstep", "in": ["end"], "from": 0, "to": 300},
        "cold": {"op": "curve", "in": ["abs_lat"],
                 "points": [[0, 0], [45, 10], [90, 40]]},
        "abs_lat": {"op": "abs", "in": ["lat"]},
        "land": {"op": "lerp", "in": ["n", "r", 0.4]},
        "scaled": {"op": "multiply", "in": [50, "land", "near_edge", "near_end"]},
        "stepped": {"op": "terrace", "in": ["scaled"], "step": 6, "sharpness": 0.7},
        "smoothed": {"op": "smooth", "in": ["stepped"], "radius": 12},
        "worn": {"op": "erode", "in": ["scaled"], "radius": 20, "strength": 0.6},
        "blend": {"op": "lerp", "in": ["smoothed", "worn", 0.5]},
        "sum": {"op": "add", "in": ["blend", "cold", 3]},
        "low": {"op": "min", "in": ["sum", 70]},
        "high": {"op": "max", "in": ["low", -25]},
        "clamped": {"op": "clamp", "in": ["high"], "min": -20, "max": 65},
        "diff": {"op": "subtract", "in": ["clamped", "n"]},
        "out": {"op": "divide", "in": ["diff", 1.5]}}},
}
PLACES = [(0, 0.5, 0.5), (0, 0.013, 0.97), (0, 0.731, 0.204), (0, 0.999, 0.5), (0, 0.25, 0.0005)]
SPHERE_PLACES = [(f, 0.37, 0.61) for f in range(6)] + [(2, 0.0, 0.0), (4, 1.0, 0.5)]
SEED = 1234
FINGERPRINT_GRAPH = "everything"


def body_shape(name: str):
    return make_shape(BODIES[name])


def places_for(name: str):
    return SPHERE_PLACES if BODIES[name]["shape"].startswith("sphere") else PLACES


def fingerprint() -> int:
    """midgard/worldgen.h's worldgen_fingerprint(): the "everything" graph on every body."""
    graph = Graph(GRAPHS[FINGERPRINT_GRAPH])
    data = b""
    for name in BODIES:
        shape = body_shape(name)
        for place in places_for(name):
            data += struct.pack("<d", graph.evaluate(shape, SEED, place))
    return fnv1a64(data)


def make_table() -> dict:
    """The shared table schema/tests/worldgen_cases.json (made once, then fixed)."""
    cases = []
    for gname, graph_json in GRAPHS.items():
        graph = Graph(graph_json)
        for name in BODIES:
            shape = body_shape(name)
            for place in places_for(name):
                cases.append({"graph": gname, "body": name, "place": list(place),
                              "height": graph.evaluate(shape, SEED, place)})
    # Terrain made from a graph (its chunk hashes): the 5.9 ground as a graph is the same as
    # the 5.9 ground itself.
    from .terrain import Terrain
    chunks = []
    for name in ("area", "habitat", "planet"):
        body = {**BODIES[name], "seed": SEED}
        for gname in GRAPHS:
            key = (0, 2, 3, 1)
            made = Terrain(body, {"graph": GRAPHS[gname]})
            chunks.append({"body": name, "graph": gname, "key": list(key),
                           "hash": f"{made.chunk_hash(key):016x}"})
        chunks.append({"body": name, "graph": None, "key": [0, 2, 3, 1],
                       "hash": f"{Terrain(body).chunk_hash((0, 2, 3, 1)):016x}"})
    return {"_about": "World generation graphs' heights (tools/reference/worldgen.py).",
            "seed": SEED, "bodies": BODIES, "graphs": GRAPHS, "cases": cases, "chunks": chunks,
            "fingerprint": f"{fingerprint():016x}"}


if __name__ == "__main__":
    out = TABLES / "worldgen_cases.json"
    out.write_text(json.dumps(make_table(), indent=1) + "\n", encoding="utf-8", newline="\n")
    print("wrote", out)
