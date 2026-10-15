# SPDX-License-Identifier: Apache-2.0
"""Editor primitives and a glTF binary (.glb) writer for the cook step.

Every primitive fits a 1 m box centred on the origin (the plane lies flat, facing up), and its
triangles run counter-clockwise seen from outside. The cube matches the engine's
render::make_cube(). Phase 5's Object editor adds more.
"""
import json
import math
import struct
from dataclasses import dataclass, field

PRIMITIVES = {"prim_cube": "Cube", "prim_sphere": "Sphere", "prim_plane": "Plane"}  # id -> label


@dataclass
class Mesh:
    positions: list[tuple[float, float, float]] = field(default_factory=list)
    normals: list[tuple[float, float, float]] = field(default_factory=list)
    indices: list[int] = field(default_factory=list)


def cube() -> Mesh:
    # (normal, u, v) with u × v = normal, so the corners run counter-clockwise from outside.
    faces = [
        ((1, 0, 0), (0, 0, -1), (0, 1, 0)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),
        ((0, 1, 0), (1, 0, 0), (0, 0, -1)), ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
        ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (-1, 0, 0), (0, 1, 0)),
    ]
    mesh = Mesh()
    for n, u, v in faces:
        base = len(mesh.positions)
        for cu, cv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            mesh.positions.append(tuple(0.5 * (n[i] + cu * u[i] + cv * v[i]) for i in range(3)))
            mesh.normals.append(tuple(float(x) for x in n))
        mesh.indices += [base + i for i in (0, 1, 2, 0, 2, 3)]
    return mesh


def sphere(segments: int = 32, rings: int = 16) -> Mesh:
    """A UV sphere of diameter 1 with smooth normals."""
    mesh = Mesh()
    for i in range(rings + 1):
        theta = math.pi * i / rings  # from the top
        for j in range(segments + 1):
            phi = 2 * math.pi * j / segments
            n = (math.sin(theta) * math.cos(phi), math.cos(theta),
                 -math.sin(theta) * math.sin(phi))
            mesh.normals.append(n)
            mesh.positions.append(tuple(0.5 * c for c in n))
    row = segments + 1
    for i in range(rings):
        for j in range(segments):
            a, b = i * row + j, (i + 1) * row + j
            c, d = b + 1, a + 1
            if i != rings - 1:
                mesh.indices += [a, b, c]
            if i != 0:  # the poles' second triangles would have no area
                mesh.indices += [a, c, d]
    return mesh


def plane() -> Mesh:
    """A 1 × 1 square in the XZ plane, facing up."""
    return Mesh([(-0.5, 0.0, 0.5), (0.5, 0.0, 0.5), (0.5, 0.0, -0.5), (-0.5, 0.0, -0.5)],
                [(0.0, 1.0, 0.0)] * 4, [0, 1, 2, 0, 2, 3])


def primitive(asset_id: str) -> Mesh:
    makers = {"prim_cube": cube, "prim_sphere": sphere, "prim_plane": plane}
    if asset_id not in makers:
        raise KeyError(asset_id)
    return makers[asset_id]()


def to_glb(mesh: Mesh) -> bytes:
    """A glTF 2.0 binary with one mesh: POSITION and NORMAL (float VEC3) and uint32 indices."""
    positions = b"".join(struct.pack("<3f", *p) for p in mesh.positions)
    normals = b"".join(struct.pack("<3f", *n) for n in mesh.normals)
    indices = struct.pack(f"<{len(mesh.indices)}I", *mesh.indices)
    binary = positions + normals + indices
    count = len(mesh.positions)
    lo = [min(p[i] for p in mesh.positions) for i in range(3)]
    hi = [max(p[i] for p in mesh.positions) for i in range(3)]
    doc = {
        "asset": {"version": "2.0", "generator": "Midgard editor"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "buffers": [{"byteLength": len(binary)}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(positions), "target": 34962},
            {"buffer": 0, "byteOffset": len(positions), "byteLength": len(normals),
             "target": 34962},
            {"buffer": 0, "byteOffset": len(positions) + len(normals),
             "byteLength": len(indices), "target": 34963},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": count, "type": "VEC3",
             "min": lo, "max": hi},
            {"bufferView": 1, "componentType": 5126, "count": count, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5125, "count": len(mesh.indices),
             "type": "SCALAR"},
        ],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1},
                                    "indices": 2}]}],
    }
    text = json.dumps(doc, separators=(",", ":")).encode("utf-8")
    text += b" " * (-len(text) % 4)
    binary += b"\0" * (-len(binary) % 4)
    total = 12 + 8 + len(text) + 8 + len(binary)
    return (struct.pack("<4sII", b"glTF", 2, total)
            + struct.pack("<I4s", len(text), b"JSON") + text
            + struct.pack("<I4s", len(binary), b"BIN\0") + binary)
