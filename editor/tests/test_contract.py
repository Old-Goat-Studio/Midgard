# SPDX-License-Identifier: Apache-2.0
import copy
import json
import sqlite3

import pytest

from midgard_editor import SCHEMA_DIR, contract

EXAMPLE = json.loads((SCHEMA_DIR / "examples" / "manifest.example.json").read_text("utf-8"))


def test_migrations_are_numbered_without_gaps():
    numbers = [n for n, _ in contract.migrations()]
    assert numbers == list(range(1, len(numbers) + 1))
    assert numbers, "contract v0 needs at least one migration"


def test_migrate_creates_the_contract_tables(tmp_path):
    db = tmp_path / "HelloCube.db"
    assert contract.migrate(db) == len(contract.migrations())
    con = sqlite3.connect(db)
    tables = {r[0] for r in con.execute("SELECT name FROM sqlite_master WHERE type='table'")}
    assert {"meta", "string", "scene", "entity"} <= tables


def test_migrate_is_idempotent(tmp_path):
    db = tmp_path / "p.db"
    first = contract.migrate(db)
    assert contract.migrate(db) == first


def test_entity_components_must_be_json(tmp_path):
    db = tmp_path / "p.db"
    contract.migrate(db)
    con = sqlite3.connect(db)
    con.execute("INSERT INTO string VALUES ('scn_main.name', 'Main', '')")
    con.execute("INSERT INTO scene VALUES ('scn_main', 'scn_main.name')")
    with pytest.raises(sqlite3.IntegrityError):
        con.execute("INSERT INTO entity (id, scene, name, components) "
                    "VALUES ('e1', 'scn_main', 'Cube', 'not json')")


def test_newer_database_is_refused(tmp_path):
    db = tmp_path / "future.db"
    con = sqlite3.connect(db)
    con.execute("PRAGMA user_version = 999")
    con.close()
    with pytest.raises(ValueError, match="newer than this editor"):
        contract.migrate(db)


def test_failed_migration_leaves_previous_version(tmp_path):
    schema = tmp_path / "schema"
    (schema / "migrations").mkdir(parents=True)
    (schema / "migrations" / "0001_ok.sql").write_text("CREATE TABLE a (x);", "utf-8")
    (schema / "migrations" / "0002_bad.sql").write_text(
        "CREATE TABLE b (y); INSERT INTO missing VALUES (1);", "utf-8")
    db = tmp_path / "p.db"
    with pytest.raises(sqlite3.Error):
        contract.migrate(db, schema)
    con = sqlite3.connect(db)
    assert contract.schema_version(con) == 1
    tables = {r[0] for r in con.execute("SELECT name FROM sqlite_master WHERE type='table'")}
    assert tables == {"a"}


def test_example_manifest_is_valid():
    assert contract.manifest_errors(EXAMPLE) == []


@pytest.mark.parametrize("change, expected", [
    (lambda m: m.pop("game"), "'game' is a required property"),
    (lambda m: m["target"].update(os="macos"), "target/os"),
    (lambda m: m.update(format_version="2.0"), "major version 2 is not supported"),
    (lambda m: m.update(database="../escape.db"), "database"),
])
def test_bad_manifests_are_reported(change, expected):
    manifest = copy.deepcopy(EXAMPLE)
    change(manifest)
    errors = contract.manifest_errors(manifest)
    assert any(expected in e for e in errors), errors


def test_live_link_schema_is_valid_json_schema():
    from jsonschema import Draft202012Validator
    schema = json.loads((SCHEMA_DIR / "live_link.schema.json").read_text("utf-8"))
    Draft202012Validator.check_schema(schema)
    validator = Draft202012Validator(schema)
    assert validator.is_valid({"type": "log", "level": "info", "text": "hi"})
    assert not validator.is_valid({"type": "log", "level": "loud", "text": "hi"})
