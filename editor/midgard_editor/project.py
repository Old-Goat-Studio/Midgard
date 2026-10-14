# SPDX-License-Identifier: Apache-2.0
"""A Midgard project: a folder holding {name}.config (JSON) and {name}.db (SQLite).

Edits are written straight away, so there is no separate Save step. Every edit goes through
the project's undo stack (see undo.py), and backups protect against mistakes (see backup.py).
"""
import json
import os
import re
import sqlite3
from collections.abc import Iterator
from contextlib import contextmanager
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

from PySide6.QtCore import QObject, Signal
from PySide6.QtGui import QUndoStack

from . import contract
from .version import VERSION

CONFIG_FORMAT = 1
WORLD_MODES = {"system_map": "System map", "bounded_surface_map": "Bounded surface map"}
NAME_PATTERN = re.compile(r"^[A-Za-z][A-Za-z0-9_-]{0,63}$")
GAME_TITLE_KEY = "game.title"


class ProjectError(Exception):
    """A project can't be created or opened. The message is shown to the user."""


class Project(QObject):
    """An open project. Signals let docks refresh when something changes."""

    config_changed = Signal(str)  # key
    string_changed = Signal(str)  # string key

    def __init__(self, root: Path, name: str, config: dict[str, Any]):
        super().__init__()
        self.root = root
        self.name = name
        self.config = config
        self.undo_stack = QUndoStack(self)

    # ---- locations ---------------------------------------------------------------------
    @property
    def config_path(self) -> Path:
        return self.root / f"{self.name}.config"

    @property
    def db_path(self) -> Path:
        return self.root / f"{self.name}.db"

    @property
    def work_dir(self) -> Path:
        """Editor-only data that is never cooked or committed (backups, caches)."""
        return self.root / ".midgard"

    # ---- create / open -----------------------------------------------------------------
    @classmethod
    def create(cls, parent_dir: Path, name: str, world_mode: str, game_title: str) -> "Project":
        if not NAME_PATTERN.match(name):
            raise ProjectError(
                "Project names start with a letter and use only letters, digits, '-' and '_' "
                "(up to 64 characters).")
        if world_mode not in WORLD_MODES:
            raise ProjectError(f"Unknown world mode: {world_mode}")
        root = Path(parent_dir) / name
        if root.exists() and any(root.iterdir()):
            raise ProjectError(f"{root} already exists and is not empty.")
        root.mkdir(parents=True, exist_ok=True)
        now = datetime.now(UTC).isoformat(timespec="seconds")
        config = {
            "format": CONFIG_FORMAT,
            "name": name,
            "game_version": "0.1.0",
            "world_mode": world_mode,    # fixed for the life of the project
            "max_players": 4,
            "created": now,
            "midgard_version": VERSION,
        }
        project = cls(root, name, config)
        project._write_config()
        contract.migrate(project.db_path)
        with project.connect() as con:
            con.executemany("INSERT INTO meta (key, value) VALUES (?, ?)",
                            [("project", name), ("created", now)])
            con.execute("INSERT INTO string (key, english, context) VALUES (?, ?, ?)",
                        (GAME_TITLE_KEY, game_title.strip() or name,
                         "The game's title, shown to players"))
        (project.root / ".gitignore").write_text(".midgard/\n", encoding="utf-8")
        return project

    @classmethod
    def open(cls, config_path: Path) -> "Project":
        config_path = Path(config_path)
        try:
            config = json.loads(config_path.read_text(encoding="utf-8"))
        except OSError as e:
            raise ProjectError(f"Can't read {config_path}: {e.strerror}") from e
        except ValueError as e:
            raise ProjectError(f"{config_path.name} is not valid JSON: {e}") from e
        if not isinstance(config, dict) or config.get("format") != CONFIG_FORMAT:
            raise ProjectError(f"{config_path.name} is not a Midgard project (format "
                               f"{config.get('format') if isinstance(config, dict) else '?'}).")
        project = cls(config_path.parent, config_path.stem, config)
        if not project.db_path.is_file():
            raise ProjectError(f"{project.db_path.name} is missing from {project.root}.")
        try:
            contract.migrate(project.db_path)  # upgrades projects made by older editors
        except (ValueError, sqlite3.Error) as e:
            raise ProjectError(str(e)) from e
        return project

    # ---- config ------------------------------------------------------------------------
    def _write_config(self) -> None:
        tmp = self.config_path.with_suffix(".config.tmp")
        tmp.write_text(json.dumps(self.config, indent=2) + "\n", encoding="utf-8")
        os.replace(tmp, self.config_path)

    def set_config(self, key: str, value: Any) -> None:
        """Write one config value. Use undo.SetConfigCommand from the UI so it can be undone."""
        self.config[key] = value
        self._write_config()
        self.config_changed.emit(key)

    # ---- database ----------------------------------------------------------------------
    @contextmanager
    def connect(self) -> Iterator[sqlite3.Connection]:
        """A connection that commits on success, rolls back on error, and always closes
        (an open handle would stop Windows from replacing or deleting the file)."""
        con = sqlite3.connect(self.db_path)
        try:
            con.execute("PRAGMA foreign_keys = ON")
            with con:
                yield con
        finally:
            con.close()

    def schema_version(self) -> int:
        with self.connect() as con:
            return contract.schema_version(con)

    def string(self, key: str) -> str | None:
        """The English text for a localisation key."""
        with self.connect() as con:
            row = con.execute("SELECT english FROM string WHERE key = ?", (key,)).fetchone()
        return row[0] if row else None

    def set_string(self, key: str, english: str | None, context: str = "") -> None:
        """Set (or, with None, delete) a localisation string. Undo via undo.SetStringCommand."""
        with self.connect() as con:
            if english is None:
                con.execute("DELETE FROM string WHERE key = ?", (key,))
            else:
                con.execute(
                    "INSERT INTO string (key, english, context) VALUES (?, ?, ?) "
                    "ON CONFLICT(key) DO UPDATE SET english = excluded.english",
                    (key, english, context))
        self.string_changed.emit(key)

    @property
    def game_title(self) -> str:
        return self.string(GAME_TITLE_KEY) or self.name
