# SPDX-License-Identifier: Apache-2.0
"""Undoable edits. Every change the editor makes to a project is a QUndoCommand pushed onto
project.undo_stack, so Edit ▸ Undo and the History panel cover everything."""
from typing import Any

from PySide6.QtGui import QUndoCommand

from .project import Project


class SetConfigCommand(QUndoCommand):
    """Change one value in {project}.config."""

    def __init__(self, project: Project, key: str, value: Any, label: str):
        super().__init__(f"Change {label}")
        self.project, self.key, self.new = project, key, value
        self.old = project.config.get(key)

    def redo(self) -> None:
        self.project.set_config(self.key, self.new)

    def undo(self) -> None:
        self.project.set_config(self.key, self.old)


class SetStringCommand(QUndoCommand):
    """Change the English text of a localisation key in {project}.db."""

    def __init__(self, project: Project, key: str, english: str, label: str, context: str = ""):
        super().__init__(f"Change {label}")
        self.project, self.key, self.new, self.context = project, key, english, context
        self.old = project.string(key)

    def redo(self) -> None:
        self.project.set_string(self.key, self.new, self.context)

    def undo(self) -> None:
        self.project.set_string(self.key, self.old, self.context)
