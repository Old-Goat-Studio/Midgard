# SPDX-License-Identifier: Apache-2.0
"""Tests for the _midgard Python module (run by ctest).

    python test_bindings.py <folder with _midgard> <schema folder> <version>
"""
import json
import sqlite3
import sys
import tempfile
from pathlib import Path

module_dir, schema_dir, expected_version = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
sys.path.insert(0, str(module_dir))
import _midgard  # noqa: E402


def make_package(root: Path, entities: dict[str, dict], content: list[dict]) -> Path:
    """A package with the real migrations, a main scene and the given entities."""
    root.mkdir(parents=True)
    migrations = sorted((schema_dir / "migrations").glob("*.sql"))
    with sqlite3.connect(root / "Game.db") as con:
        for m in migrations:
            con.executescript(m.read_text("utf-8"))
        con.execute(f"PRAGMA user_version = {len(migrations)}")
        con.execute("INSERT INTO string VALUES ('scn_main.name', 'Main', '')")
        con.execute("INSERT INTO scene (id, name_key, settings) VALUES ('scn_main', "
                    "'scn_main.name', '{}')")
        for entity_id, components in entities.items():
            con.execute("INSERT INTO entity (id, scene, name, parent, components) "
                        "VALUES (?, 'scn_main', ?, NULL, ?)",
                        (entity_id, entity_id, json.dumps(components)))
    con.close()
    (root / "manifest.json").write_text(json.dumps({
        "format": "midgard-package", "format_version": "1.0",
        "game": {"name": "Game", "version": "0.1.0"},
        "built_with": {"editor": "0.1.0+1", "engine_abi": 0},
        "target": {"os": "windows", "arch": "x86_64", "role": "game"},
        "entry": {"scene": "scn_main"}, "database": "Game.db",
        "schema_version": len(migrations), "content": content}), "utf-8")
    return root


def main() -> int:
    version = _midgard.version()
    assert version["engine"] == expected_version, version
    assert version["schema_min"] <= version["schema_max"]

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        missing = _midgard.validate(tmp / "nowhere")
        assert not missing["ok"] and missing["errors"], missing

        good = make_package(tmp / "good", {"e1": {"mesh": {"asset": "builtin:cube"}},
                                           "e2": {"camera": {}}}, [])
        report = _midgard.validate(good)
        assert report["ok"] and report["entities"] == 2 and not report["warnings"], report

        report = _midgard.validate(str(good))  # a str works as well as a Path
        assert report["ok"], report

        unknown = make_package(tmp / "unknown_mesh", {"e1": {"mesh": {"asset": "prim_x"}}}, [])
        report = _midgard.validate(unknown)
        assert report["ok"] and "prim_x" in report["warnings"][0], report

        no_pak = make_package(tmp / "no_pak", {}, [
            {"id": "prim_cube", "path": "meshes/prim_cube.glb", "sha256": "0" * 64}])
        report = _midgard.validate(no_pak)
        assert not report["ok"] and "game.pak" in report["errors"][0], report

        bad = make_package(tmp / "bad_entity", {"e1": {"transform": {"position": [1, 2]}}}, [])
        report = _midgard.validate(bad)
        assert not report["ok"] and "e1" in report["errors"][0], report
    check_viewport()
    check_bodies()
    print("_midgard: all checks passed")
    return 0


# The shapes of schema/tests/shape_points.json, as body settings.
TABLE_BODIES = {
    "bounded_area": {"shape": "bounded_area", "width": 2000, "length": 3000, "gravity": 9.81},
    "flat_cylinder": {"shape": "flat_cylinder", "circumference": 6283, "length": 5000,
                      "gravity": 9.81},
    "curved_cylinder": {"shape": "curved_cylinder", "radius": 1000, "length": 5000,
                        "gravity": 9.81},
    "sphere_outside": {"shape": "sphere_outside", "radius": 1000, "gravity": 9.81},
    "sphere_inside": {"shape": "sphere_inside", "radius": 1500, "gravity": 7.5},
}


def check_bodies() -> None:
    """The editor's view of world shapes is the engine's, bit for bit (the shared table)."""
    import struct

    def bits(x: float) -> str:
        return f"{struct.unpack('<Q', struct.pack('<d', x))[0]:016x}"

    bodies = {name: _midgard.Body(json.dumps(s)) for name, s in TABLE_BODIES.items()}
    cases = json.loads((schema_dir / "tests" / "shape_points.json").read_text("utf-8"))["cases"]
    for c in cases:
        body = bodies[c["shape"]]
        face = c["surface"][0]
        u, v, h = (float.fromhex(x) for x in c["surface"][1:])
        p = body.position(face, u, v, h)
        assert [bits(x) for x in p] == c["position"], c
        f2, u2, v2, h2 = body.surface(*p)
        assert [f2, bits(u2), bits(v2), bits(h2)] == c["back"], c
        assert [bits(x) for x in body.up(*p)] == c["up"], c
        assert [bits(x) for x in body.gravity(*p)] == c["gravity"], c
    fingerprint = json.loads((schema_dir / "tests" / "shape_fingerprint.json")
                             .read_text("utf-8"))["cases"][0]["fingerprint"]
    assert f"{_midgard.shape_fingerprint():016x}" == fingerprint

    planet = _midgard.Body(json.dumps({"id": "p", "shape": "sphere_outside", "radius": 1000,
                                       "seed": 7, "origin": [10, 0, 0]}))
    assert (planet.id, planet.seed, planet.shape, planet.faces) == ("p", 7, "sphere_outside", 6)
    up, east, north = planet.frame(*planet.position(4, 0.5, 0.5))
    assert abs(up[2] - 1.0) < 1e-12
    for bad in ('{"shape": "sphere_outside", "radius": 50}', "not json"):
        try:
            _midgard.Body(bad)
        except ValueError as e:
            assert "radius 50" in str(e) or "JSON" in str(e), e
        else:
            raise AssertionError(f"{bad} was accepted")


def pixel(rgba: bytes, width: int, x: int, y: int) -> tuple[int, int, int]:
    i = (y * width + x) * 4
    return rgba[i], rgba[i + 1], rgba[i + 2]


def check_viewport() -> None:
    """The editor's viewport, drawn offscreen (skipped on machines without Vulkan)."""
    view = _midgard.Viewport()
    view.put_entity(json.dumps({"id": "e1", "name": "Cube", "parent": None, "components": {
        "transform": {"position": [0, 0, 0]}, "mesh": {"asset": "builtin:cube"},
        "material": {"color": [1, 0.2, 0.1, 1]}}}))
    view.put_entity(json.dumps({"id": "e2", "name": "Sun", "components": {
        "transform": {"rotation": [-50, 35, 0]}, "light": {"type": "directional"}}}))
    view.set_settings(json.dumps({"sky": [0, 0, 1]}))
    view.set_camera([0, 0, 3], [0, 0, 0], 50)
    assert view.entity_count == 2 and not view.attached
    assert not view.render()  # nothing to draw into yet
    try:
        view.set_mesh("bad", b"not a glb")
        raise AssertionError("a bad mesh must be refused")
    except RuntimeError:
        pass
    try:
        view.attach_offscreen(160, 90)
    except RuntimeError as e:
        print(f"_midgard: viewport skipped (no Vulkan here: {e})")
        return
    for attempt in range(2):  # again after detaching: meshes go back onto the GPU
        assert view.render()
        rgba = view.read_pixels()
        assert len(rgba) == 160 * 90 * 4
        r, g, b = pixel(rgba, 160, 80, 45)
        assert r > 100 and b < 100, ("the cube is in the middle", r, g, b)
        r, g, b = pixel(rgba, 160, 2, 2)
        # Full-strength blue is above the tone mapper's 0.76, so it rolls off a little towards
        # white (about 33, 33, 241).
        assert b > 200 and r < 60, ("the sky is in the corner", r, g, b)
        view.detach()
        assert not view.attached
        if attempt == 0:
            view.attach_offscreen(160, 90)
    view.remove_entity("e1")
    assert view.entity_count == 1
    print(f"_midgard: viewport drew on {view.device_name or 'the GPU'}")


if __name__ == "__main__":
    sys.exit(main())
