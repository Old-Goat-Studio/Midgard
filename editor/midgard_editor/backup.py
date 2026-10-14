# SPDX-License-Identifier: Apache-2.0
"""Automatic project backups: rotating copies of {project}.db and {project}.config in
.midgard/backups/. Taken when a project opens and every few minutes while it is being edited."""
import shutil
import sqlite3
from datetime import datetime
from pathlib import Path

from PySide6.QtCore import QObject, QTimer

from .project import Project


def backup_dir(project: Project) -> Path:
    return project.work_dir / "backups"


def list_backups(project: Project) -> list[Path]:
    """Backup databases, oldest first."""
    folder = backup_dir(project)
    return sorted(folder.glob(f"{project.name}-*.db")) if folder.is_dir() else []


def make_backup(project: Project, keep: int) -> Path:
    """Copy the database (with SQLite's online backup, so it is consistent) and the config,
    then delete the oldest copies beyond `keep`. Returns the new database copy."""
    folder = backup_dir(project)
    folder.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    db_copy = folder / f"{project.name}-{stamp}.db"
    with project.connect() as src:
        dst = sqlite3.connect(db_copy)
        try:
            src.backup(dst)
        finally:
            dst.close()
    shutil.copy2(project.config_path, db_copy.with_suffix(".config"))
    for old in list_backups(project)[:-max(keep, 1)]:
        old.unlink(missing_ok=True)
        old.with_suffix(".config").unlink(missing_ok=True)
    return db_copy


class BackupScheduler(QObject):
    """Backs up on open, then every `minutes` while there are unsaved-to-backup edits."""

    def __init__(self, project: Project, keep: int, minutes: int, parent: QObject | None = None):
        super().__init__(parent)
        self.project, self.keep = project, keep
        self._dirty = False
        project.undo_stack.indexChanged.connect(self._mark_dirty)
        self.timer = QTimer(self)
        self.timer.setInterval(max(minutes, 1) * 60_000)
        self.timer.timeout.connect(self.backup_if_needed)
        self.last: Path | None = make_backup(project, keep)
        self.timer.start()

    def _mark_dirty(self) -> None:
        self._dirty = True

    def backup_if_needed(self) -> Path | None:
        if self._dirty:
            self.last = make_backup(self.project, self.keep)
            self._dirty = False
            return self.last
        return None
