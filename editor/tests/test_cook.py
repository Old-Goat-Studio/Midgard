# SPDX-License-Identifier: Apache-2.0
import hashlib
import json
import math
import os
import sqlite3
import struct
import subprocess
import zipfile

import pytest

from midgard_editor import about, contract, meshes
from midgard_editor.cook import cook, host_target
from midgard_editor.scene_model import look_at_rotation


def parse_glb(data: bytes) -> tuple[dict, bytes]:
    magic, version, total = struct.unpack_from("<4sII", data, 0)
    assert (magic, version, total) == (b"glTF", 2, len(data))
    json_len, json_type = struct.unpack_from("<I4s", data, 12)
    assert json_type == b"JSON"
    doc = json.loads(data[20:20 + json_len])
    bin_len, bin_type = struct.unpack_from("<I4s", data, 20 + json_len)
    assert bin_type == b"BIN\0"
    start = 28 + json_len
    return doc, data[start:start + bin_len]


def test_cube_matches_the_engine():
    cube = meshes.cube()
    assert len(cube.positions) == 24 and len(cube.indices) == 36
    for i in range(0, 36, 3):
        a, b, c = (cube.positions[cube.indices[i + k]] for k in range(3))
        n = cube.normals[cube.indices[i]]
        ab = [b[j] - a[j] for j in range(3)]
        ac = [c[j] - a[j] for j in range(3)]
        cross = (ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2],
                 ab[0] * ac[1] - ab[1] * ac[0])
        assert sum(cross[j] * n[j] for j in range(3)) > 0  # counter-clockwise from outside


def test_glb_is_well_formed():
    doc, binary = parse_glb(meshes.to_glb(meshes.cube()))
    assert doc["asset"]["version"] == "2.0"
    assert doc["accessors"][0]["count"] == 24
    assert doc["accessors"][2]["count"] == 36
    assert doc["buffers"][0]["byteLength"] == len(binary)
    indices = struct.unpack_from("<36I", binary, doc["bufferViews"][2]["byteOffset"])
    assert max(indices) == 23


@pytest.mark.parametrize("eye, target", [((4, 3, 6), (0, 0.5, 0)), ((0, 0, 5), (0, 0, 0)),
                                         ((-3, 1, -2), (1, 1, 1))])
def test_look_at_points_minus_z_at_the_target(eye, target):
    pitch, yaw, _ = (math.radians(a) for a in look_at_rotation(eye, target))
    forward = (-math.sin(yaw) * math.cos(pitch), math.sin(pitch),
               -math.cos(yaw) * math.cos(pitch))  # R = Ry·Rx applied to (0, 0, -1)
    d = [target[i] - eye[i] for i in range(3)]
    length = math.sqrt(sum(x * x for x in d))
    assert all(abs(forward[i] - d[i] / length) < 1e-4 for i in range(3))


def test_cook_writes_a_valid_package(project, tmp_path):
    result = cook(project, tmp_path / "pkg")
    out = result.out_dir
    manifest = json.loads((out / "manifest.json").read_text("utf-8"))
    assert contract.manifest_errors(manifest) == []
    assert manifest["target"] == host_target()
    assert manifest["entry"]["scene"] == "scn_main"
    assert manifest["schema_version"] == len(contract.migrations())
    assert manifest["built_with"]["editor"].startswith("0.1.0+")
    assert [c["id"] for c in manifest["content"]] == ["prim_cube"]

    with zipfile.ZipFile(out / "game.pak") as pak:
        data = pak.read("meshes/prim_cube.glb")
    assert hashlib.sha256(data).hexdigest() == manifest["content"][0]["sha256"]
    parse_glb(data)

    con = sqlite3.connect(out / "HelloCube.db")
    names = [r[0] for r in con.execute("SELECT name FROM entity ORDER BY id")]
    con.close()
    assert names == ["Camera", "Sun", "Ground", "Cube"]
    assert json.loads((out / "HelloCube.config").read_text("utf-8")) == {
        "game_version": "0.1.0", "world_mode": "system_map", "max_players": 4}


def test_unknown_meshes_are_warned_about(project, tmp_path):
    from midgard_editor.scene_model import Entity

    project.put_entity(Entity("e000099", "Ghost", None, {"mesh": {"asset": "mesh_missing"}}))
    result = cook(project, tmp_path / "pkg")
    assert any("Ghost" in w for w in result.warnings)


def test_recooking_replaces_the_old_package(project, tmp_path):
    out = tmp_path / "pkg"
    cook(project, out)
    (out / "stale.txt").write_text("old", "utf-8")
    cook(project, out)
    assert not (out / "stale.txt").exists()


@pytest.mark.skipif(not about.runtime_candidates(), reason="engine not built here")
def test_the_engine_renders_the_cooked_package(project, tmp_path):
    out = cook(project, tmp_path / "pkg").out_dir
    shot = tmp_path / "shot.png"
    run = subprocess.run([str(about.runtime_candidates()[0]), "--package", str(out),
                          "--screenshot", str(shot), "--size", "320x180"],
                         capture_output=True, text=True, timeout=60)
    if run.returncode != 0 and "Vulkan" in (run.stderr + run.stdout):
        pytest.skip("no Vulkan driver available: " + run.stderr.strip())
    assert run.returncode == 0, run.stderr
    assert "4 entities" in run.stdout
    assert shot.stat().st_size > 1000


def test_cook_target_names(monkeypatch):
    monkeypatch.setattr(os, "name", "nt")
    assert host_target()["os"] == "windows"
