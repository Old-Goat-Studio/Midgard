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
    print("_midgard: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
