# SPDX-License-Identifier: Apache-2.0
"""The editor's side of the Midgard contract (schema/): database migrations and manifests."""
import json
import re
import sqlite3
from pathlib import Path

from jsonschema import Draft202012Validator

from . import SCHEMA_DIR

MIGRATION_NAME = re.compile(r"^(\d{4})_[a-z0-9_]+\.sql$")
SUPPORTED_FORMAT_MAJOR = 1


def migrations(schema_dir: Path = SCHEMA_DIR) -> list[tuple[int, Path]]:
    """(number, path) for every migration, in order. Numbers must run 1, 2, 3… with no gaps."""
    found = []
    for path in (schema_dir / "migrations").glob("*.sql"):
        m = MIGRATION_NAME.match(path.name)
        if not m:
            raise ValueError(f"Badly named migration: {path.name}")
        found.append((int(m.group(1)), path))
    found.sort()
    numbers = [n for n, _ in found]
    if numbers != list(range(1, len(numbers) + 1)):
        raise ValueError(f"Migrations must be numbered 1..N without gaps, found {numbers}")
    return found


def schema_version(con: sqlite3.Connection) -> int:
    return con.execute("PRAGMA user_version").fetchone()[0]


def migrate(db_path: Path, schema_dir: Path = SCHEMA_DIR) -> int:
    """Apply every migration newer than the database's user_version. Returns the new version.

    Each migration runs in its own transaction, so a failure leaves the database at the
    last migration that succeeded.
    """
    con = sqlite3.connect(db_path, isolation_level=None)
    try:
        con.execute("PRAGMA foreign_keys = ON")
        current = schema_version(con)
        known = migrations(schema_dir)
        if current > len(known):
            raise ValueError(
                f"{db_path.name} is at schema version {current}, newer than this editor "
                f"({len(known)})")
        for number, path in known:
            if number <= current:
                continue
            script = path.read_text(encoding="utf-8")
            try:
                con.executescript(
                    f"BEGIN;\n{script}\n;PRAGMA user_version = {number};\nCOMMIT;")
            except sqlite3.Error:
                if con.in_transaction:
                    con.execute("ROLLBACK")
                raise
            current = number
        return current
    finally:
        con.close()


def manifest_validator(schema_dir: Path = SCHEMA_DIR) -> Draft202012Validator:
    schema = json.loads((schema_dir / "manifest.schema.json").read_text(encoding="utf-8"))
    Draft202012Validator.check_schema(schema)
    return Draft202012Validator(schema)


def manifest_errors(manifest: dict, schema_dir: Path = SCHEMA_DIR) -> list[str]:
    """Problems with a manifest, as readable strings. An empty list means it is valid."""
    errors = [
        f"{'/'.join(str(p) for p in e.absolute_path) or '(root)'}: {e.message}"
        for e in manifest_validator(schema_dir).iter_errors(manifest)
    ]
    if not errors:
        major = int(manifest["format_version"].split(".")[0])
        if major != SUPPORTED_FORMAT_MAJOR:
            errors.append(
                f"format_version: major version {major} is not supported "
                f"(this editor writes {SUPPORTED_FORMAT_MAJOR}.x)")
    return errors


def live_link_errors(message: dict, schema_dir: Path = SCHEMA_DIR) -> list[str]:
    """Problems with a live-link message (schema/live_link.schema.json); empty if valid."""
    schema = json.loads((schema_dir / "live_link.schema.json").read_text(encoding="utf-8"))
    return [e.message for e in Draft202012Validator(schema).iter_errors(message)]
