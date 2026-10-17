# SPDX-License-Identifier: Apache-2.0
import base64
import json
import sqlite3
import struct
import subprocess
import zipfile
import zlib

import pytest

from midgard_editor import about, engine, meshes
from midgard_editor.cook import cook
from midgard_editor.objects import ObjectDef
from midgard_editor.scene_model import Entity

needs_import = pytest.mark.skipif(
    engine.module() is None or not hasattr(engine.module(), "import_model"),
    reason="the engine module (with import_model) isn't built here")


def png(rgba: tuple[int, int, int, int], size: int = 4) -> bytes:
    """A small PNG of one colour."""
    def chunk(kind: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))
    rows = b"".join(b"\0" + bytes(rgba) * size for _ in range(size))
    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows))
            + chunk(b"IEND", b""))


def write_crate(folder) -> None:
    """crate.gltf: two cubes, a red-textured one with a green-tinted one on top, its buffer
    embedded and its texture beside it in red.png."""
    cube = meshes.cube()
    positions = b"".join(struct.pack("<3f", *p) for p in cube.positions)
    normals = b"".join(struct.pack("<3f", *n) for n in cube.normals)
    uvs = b"".join(struct.pack("<2f", 0.5, 0.5) for _ in cube.positions)
    indices = struct.pack(f"<{len(cube.indices)}I", *cube.indices)
    data = positions + normals + uvs + indices
    n = len(cube.positions)
    views, offset = [], 0
    for chunk in (positions, normals, uvs, indices):
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(chunk)})
        offset += len(chunk)
    attributes = {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}
    doc = {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0, 1]}],
        "nodes": [{"mesh": 0, "name": "box"}, {"mesh": 1, "name": "lid", "translation": [0, 1, 0]}],
        "meshes": [{"primitives": [{"attributes": attributes, "indices": 3, "material": 0}]},
                   {"primitives": [{"attributes": attributes, "indices": 3, "material": 1}]}],
        "materials": [
            {"name": "red", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0},
                                                     "metallicFactor": 0.0}},
            {"name": "green", "pbrMetallicRoughness": {"baseColorFactor": [0.2, 0.6, 0.1, 1],
                                                       "metallicFactor": 0.0,
                                                       "roughnessFactor": 0.5}},
        ],
        "textures": [{"source": 0}],
        "images": [{"uri": "red.png"}],
        "buffers": [{"byteLength": len(data), "uri": "data:application/octet-stream;base64," +
                     base64.b64encode(data).decode("ascii")}],
        "bufferViews": views,
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": n, "type": "VEC3",
             "min": [-0.5, -0.5, -0.5], "max": [0.5, 0.5, 0.5]},
            {"bufferView": 1, "componentType": 5126, "count": n, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": n, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5125, "count": len(cube.indices),
             "type": "SCALAR"},
        ],
    }
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "crate.gltf").write_text(json.dumps(doc), "utf-8")
    (folder / "red.png").write_bytes(png((230, 20, 20, 255)))


def place(project, object_id: str, entity_id: str = "e000050", **components) -> None:
    project.put_entity(Entity(entity_id, "Crate", None,
                              {"object": {"id": object_id}, **components}))


def cooked_parts(out, object_id: str) -> list:
    con = sqlite3.connect(out / "HelloCube.db")
    try:
        return json.loads(con.execute("SELECT parts FROM object WHERE id = ?",
                                      (object_id,)).fetchone()[0])
    finally:
        con.close()


def test_the_object_library_round_trips(project):
    seen = []
    project.objects_changed.connect(seen.append)
    obj = ObjectDef("obj_bench", "Bench", None, {}, [
        {"name": "seat", "components": {"mesh": {"asset": "prim_cube"}}}])
    project.put_object(obj)
    assert project.objects() == [obj]
    assert project.object("obj_bench") == obj
    project.delete_object("obj_bench")
    assert project.objects() == []
    assert seen == ["obj_bench", ""]


def test_objects_made_in_the_editor_cook_their_meshes(project, tmp_path):
    project.put_object(ObjectDef("obj_bench", "Bench", None, {}, [
        {"name": "seat", "components": {"mesh": {"asset": "prim_wedge"}, "collider": {}}},
        {"name": "leg", "components": {"mesh": {"asset": "mesh_missing"}}}]))
    place(project, "obj_bench", transform={"position": [2, 0.5, 0]})
    place(project, "obj_nowhere", "e000051")
    result = cook(project, tmp_path / "pkg")
    ids = [c["id"] for c in result.manifest["content"]]
    assert "prim_wedge" in ids
    assert any("Bench" in w and "mesh_missing" in w for w in result.warnings)
    assert any("obj_nowhere" in w and "not in the project" in w for w in result.warnings)
    # The template goes into the package as it is.
    assert cooked_parts(result.out_dir, "obj_bench")[0]["name"] == "seat"


def test_a_missing_model_file_is_a_warning(project, tmp_path):
    project.put_object(ObjectDef("obj_crate", "Crate", "crate/crate.gltf"))
    place(project, "obj_crate")
    result = cook(project, tmp_path / "pkg")
    assert any("Crate" in w and "can't be imported" in w for w in result.warnings)


@needs_import
def test_imported_objects_cook_into_parts_meshes_and_textures(project, tmp_path):
    write_crate(project.objects_dir / "crate")
    project.put_object(ObjectDef("obj_crate", "Crate", "crate/crate.gltf", {"scale": 2.0}))
    place(project, "obj_crate", transform={"position": [2, 0, 0]})
    result = cook(project, tmp_path / "pkg")
    assert not [w for w in result.warnings if "Crate" in w], result.warnings

    content = {c["id"]: c["path"] for c in result.manifest["content"]}
    assert content["obj:obj_crate/0"] == "models/obj_crate/0.glb"
    assert content["obj:obj_crate/1"] == "models/obj_crate/1.glb"
    assert content["obj:obj_crate/tex0"] == "models/obj_crate/tex0.ktx2"
    with zipfile.ZipFile(result.out_dir / "game.pak") as pak:
        assert pak.read("models/obj_crate/tex0.ktx2")[:12] == b"\xabKTX 20\xbb\r\n\x1a\n"
        assert pak.read("models/obj_crate/0.glb")[:4] == b"glTF"

    parts = cooked_parts(result.out_dir, "obj_crate")
    assert [p["name"] for p in parts] == ["red", "green", "collider"]  # a part per material
    red, green, box = (p["components"] for p in parts)
    assert red["mesh"] == {"asset": "obj:obj_crate/0"}
    assert red["material"]["textures"] == {"base_color": "obj:obj_crate/tex0"}
    # Factors come over in sRGB (glTF's are linear).
    assert green["material"]["color"][1] == pytest.approx(0.797, abs=0.002)
    assert green["material"]["roughness"] == 0.5
    # The default collider: one box round it all (scaled by 2: 2 m wide, 4 m tall).
    assert box["collider"]["size"] == pytest.approx([2, 4, 2])
    assert box["transform"]["position"] == pytest.approx([0, 1, 0])


@needs_import
@pytest.mark.parametrize("collider", ["convex", "none"])
def test_the_collider_setting_shapes_the_parts(project, tmp_path, collider):
    write_crate(project.objects_dir / "crate")
    project.put_object(ObjectDef("obj_crate", "Crate", "crate/crate.gltf",
                                 {"collider": collider}))
    place(project, "obj_crate")
    parts = cooked_parts(cook(project, tmp_path / "pkg").out_dir, "obj_crate")
    assert len(parts) == 2
    for p in parts:
        assert p["components"].get("collider") == ({"shape": "convex"} if collider == "convex"
                                                   else None)


@needs_import
@pytest.mark.skipif(not about.runtime_candidates(), reason="engine not built here")
def test_the_game_draws_an_imported_object_with_its_texture(project, tmp_path):
    write_crate(project.objects_dir / "crate")
    project.put_object(ObjectDef("obj_crate", "Crate", "crate/crate.gltf"))
    # Beside the player, where the third-person camera looks; it falls onto the ground.
    place(project, "obj_crate", transform={"position": [0, 1.5, -1.5], "scale": [1.5, 1.5, 1.5]},
          rigidbody={})
    out = cook(project, tmp_path / "pkg").out_dir
    shot = tmp_path / "shot.png"
    run = subprocess.run([str(about.runtime_candidates()[0]), "--package", str(out),
                          "--screenshot", str(shot), "--size", "320x180"],
                         capture_output=True, text=True, timeout=60)
    if run.returncode != 0 and "Vulkan" in (run.stderr + run.stdout):
        pytest.skip("no Vulkan driver available: " + run.stderr.strip())
    assert run.returncode == 0, run.stderr

    from PySide6.QtGui import QImage
    image = QImage(str(shot))
    red = 0
    for y in range(0, image.height(), 2):
        for x in range(0, image.width(), 2):
            c = image.pixelColor(x, y)
            red += c.red() > 2 * c.green() + 40 and c.red() > 2 * c.blue() + 40
    assert red > 50, f"only {red} red pixels: the texture wasn't drawn"
