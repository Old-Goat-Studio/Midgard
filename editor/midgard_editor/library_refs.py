# SPDX-License-Identifier: Apache-2.0
"""Keeping a game working when its Asset Library is organized: renaming and moving assets and
collections (library_tree) changes the paths of their folders, so every path the game uses to
one of them is changed to match, as part of the same change (all or nothing).

A game names library files by their paths: "lib:models/hero/hero.glb" (a model),
"tex:library/textures/metal/rusty/rusty_basecolor.png" (an image), "snd:library/effects/..." (a
sound), and "library/..." wherever a file is named by its path in the project. They are kept
in the project's database (in any table), its config and its data files (.json), which are
always changed; and they may be spelled out in its scripts and shaders, which are shown to you
first (`find_uses`) and changed only if you say so.

`rename_asset`, `rename_collection` and `move` here are library_tree's, for a project: they ask
`confirm` (shown what will change; it says whether to fix the scripts, or raises Cancelled),
change the game to match, and then tell the editor's panels (`refresh`)."""
import contextlib
import json
import re
import sqlite3
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

from . import asset_library as lib
from . import library_tree as lt

DATA_SUFFIXES = (".json", ".config")  # always changed
SCRIPT_SUFFIXES = (".py", ".pyi", ".glsl", ".vert", ".frag", ".comp")  # shown, changed if asked
SKIPPED = {"build", ".midgard", lib.FOLDER, ".git", "__pycache__", ".venv"}  # never looked in


class Cancelled(lt.OrganizeError):
    """You said no: nothing was changed."""


@dataclass
class ScriptLine:
    file: str   # from the project's folder
    line: int   # counted from 1
    text: str   # the line as it is now


@dataclass
class Uses:
    """What a change to the library changes in the game."""
    database: dict[str, int] = field(default_factory=dict)  # table -> rows
    files: list[str] = field(default_factory=list)          # data files (always changed)
    scripts: list[ScriptLine] = field(default_factory=list)  # lines naming moved folders

    @property
    def any(self) -> bool:
        return bool(self.database or self.files or self.scripts)

    def summary(self) -> str:
        """In plain words: "3 entities, 2 sound events and 1 file"."""
        parts = [f"{n} {_plural(table.replace('_', ' '), n)}"
                 for table, n in sorted(self.database.items())]
        if self.files:
            parts.append(f"{len(self.files)} file{'' if len(self.files) == 1 else 's'}")
        if not parts:
            return "nothing in the game"
        return parts[0] if len(parts) == 1 else ", ".join(parts[:-1]) + " and " + parts[-1]


def _plural(word: str, n: int) -> str:
    if n == 1:
        return word
    return word[:-1] + "ies" if word.endswith("y") else word + "s"


# ---- the paths -------------------------------------------------------------------------------
def pattern(moved: lt.Moved) -> re.Pattern | None:
    """Finds a path into a moved folder, after "lib:" or "library/" (longest first, so a moved
    folder is never mistaken for one whose name begins the same)."""
    olds = sorted({old for old, _ in moved}, key=len, reverse=True)
    if not olds:
        return None
    return re.compile(r"(?<![\w.-])(lib:|library/)(" + "|".join(map(re.escape, olds)) +
                      r")(?![\w.-])")


def rewrite(text: str, moved: lt.Moved) -> tuple[str, int]:
    """The text with every path into a moved folder changed to its new one, and how many."""
    found = pattern(moved)
    if found is None:
        return text, 0
    new = dict(moved)
    return found.subn(lambda m: m.group(1) + new[m.group(2)], text)


def _project_files(project) -> list[Path]:
    """The project's own data, script and shader files (not its build, library or caches)."""
    root = Path(project.root)
    out = []
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root)
        if rel.parts and rel.parts[0] in SKIPPED or any(p in SKIPPED for p in rel.parts[:-1]):
            continue
        if path.is_file() and path.suffix.lower() in DATA_SUFFIXES + SCRIPT_SUFFIXES and \
                not path.name.endswith(".tmp"):
            out.append(path)
    return out


def _read(path: Path) -> str | None:
    try:
        return path.read_bytes().decode("utf-8")
    except (OSError, UnicodeDecodeError):
        return None


def _tables(con: sqlite3.Connection) -> list[str]:
    return [t for (t,) in con.execute("SELECT name FROM sqlite_master WHERE type = 'table' "
                                      "AND name NOT LIKE 'sqlite_%'")]


def _changed_rows(con: sqlite3.Connection, table: str, moved: lt.Moved):
    """(rowid, {column: new value}) for each row of a table naming a moved folder."""
    columns = [r[1] for r in con.execute(f'PRAGMA table_info("{table}")')]
    if not columns:
        return
    names = ", ".join(f'"{c}"' for c in columns)
    for row in con.execute(f'SELECT rowid, {names} FROM "{table}"').fetchall():
        changes = {}
        for column, value in zip(columns, row[1:], strict=True):
            if isinstance(value, str) and ("lib:" in value or "library/" in value):
                new, count = rewrite(value, moved)
                if count:
                    changes[column] = new
        if changes:
            yield row[0], changes


def find_uses(project, moved: lt.Moved) -> Uses:
    """What changing these folders' paths changes in the game (nothing is changed)."""
    uses = Uses()
    if not moved:
        return uses
    with project.connect() as con:
        for table in _tables(con):
            rows = sum(1 for _ in _changed_rows(con, table, moved))
            if rows:
                uses.database[table] = rows
    found = pattern(moved)
    root = Path(project.root)
    for path in _project_files(project):
        text = _read(path)
        if text is None or not found.search(text):
            continue
        rel = path.relative_to(root).as_posix()
        if path.suffix.lower() in DATA_SUFFIXES:
            uses.files.append(rel)
        else:
            uses.scripts += [ScriptLine(rel, i, line.strip())
                             for i, line in enumerate(text.splitlines(), 1)
                             if found.search(line)]
    return uses


def update(project, moved: lt.Moved, fix_scripts: bool = False) -> Uses:
    """Changes every path into a moved folder in the game: its database (one transaction), its
    data files, and its scripts and shaders if `fix_scripts`. All or nothing: if any part fails,
    what was changed is put back and OrganizeError says why."""
    uses = find_uses(project, moved)
    if not uses.any:
        return uses
    root = Path(project.root)
    files = uses.files + (sorted({s.file for s in uses.scripts}) if fix_scripts else [])
    written: list[tuple[Path, bytes]] = []
    try:
        for rel in files:
            path = root / rel
            before = path.read_bytes()
            new, _ = rewrite(before.decode("utf-8"), moved)
            tmp = path.with_name(path.name + ".tmp")
            tmp.write_bytes(new.encode("utf-8"))
            written.append((path, before))
            tmp.replace(path)
        with project.connect() as con:  # (one transaction: all rows or none)
            for table in _tables(con):
                for rowid, changes in list(_changed_rows(con, table, moved)):
                    sets = ", ".join(f'"{c}" = ?' for c in changes)
                    con.execute(f'UPDATE "{table}" SET {sets} WHERE rowid = ?',
                                (*changes.values(), rowid))
    except (OSError, sqlite3.Error, UnicodeDecodeError) as e:
        for path, before in reversed(written):
            with contextlib.suppress(OSError):
                path.write_bytes(before)
        raise lt.OrganizeError(f"The game couldn't be changed to match, so nothing was "
                               f"changed: {e}") from e
    return uses


def refresh(project) -> None:
    """Tells the editor's panels the game's paths changed, and clears Undo (what it would put
    back names the old paths)."""
    project.undo_stack.clear()
    try:  # (its config file may have changed: what the editor holds must too)
        doc = json.loads(project.config_path.read_text("utf-8"))
    except (OSError, ValueError):
        doc = None
    if isinstance(doc, dict):
        project.config.clear()
        project.config.update(doc)
    for signal in (project.entities_changed, project.objects_changed,
                   project.environments_changed, project.sound_events_changed,
                   project.weather_changed, project.stats_changed,
                   project.world_types_changed,
                   project.calendars_changed, project.scenes_changed):
        signal.emit("")
    project.scene_settings_changed.emit(project.scene)
    project.terrain_changed.emit(project.scene)
    project.config_changed.emit("")


# ---- organizing a project's library ----------------------------------------------------------
Confirm = Callable[[Uses], bool]  # shown what will change: whether to fix scripts (or Cancelled)


def _then(project, confirm: Confirm | None):
    def then(moved: lt.Moved) -> None:
        uses = find_uses(project, moved)
        fix_scripts = confirm(uses) if confirm is not None and uses.any else False
        update(project, moved, fix_scripts)
    return then


def rename_asset(project, path: str, name: str, confirm: Confirm | None = None) -> lt.Moved:
    moved = lt.rename_asset(lib.library_dir(project), path, name, lib.update_fields,
                            then=_then(project, confirm))
    if moved:
        refresh(project)
    return moved


def rename_collection(project, path: str, name: str,
                      confirm: Confirm | None = None) -> lt.Moved:
    moved = lt.rename_collection(lib.library_dir(project), path, name,
                                 then=_then(project, confirm))
    if moved:
        refresh(project)
    return moved


def move(project, paths: list[str], to: str, index: int | None = None,
         confirm: Confirm | None = None) -> lt.Moved:
    moved = lt.move(lib.library_dir(project), paths, to, index, lib.update_fields,
                    then=_then(project, confirm))
    if moved:
        refresh(project)
    return moved
