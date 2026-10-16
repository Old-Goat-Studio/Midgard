# SPDX-License-Identifier: Apache-2.0
"""Undoable edits. Every change the editor makes to a project is a QUndoCommand pushed onto
project.undo_stack, so Edit ▸ Undo and the History panel cover everything."""
from typing import Any

from PySide6.QtGui import QUndoCommand

from .project import Project
from .scene_model import Entity, descendants


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


class AddEntityCommand(QUndoCommand):
    def __init__(self, project: Project, entity: Entity):
        super().__init__(f"Add {entity.name}")
        self.project, self.entity = project, entity

    def redo(self) -> None:
        self.project.put_entity(self.entity)

    def undo(self) -> None:
        self.project.delete_entity(self.entity.id)


class DeleteEntityCommand(QUndoCommand):
    """Delete an entity and everything inside it (a folder's contents, an entity's children);
    Undo puts them all back."""

    def __init__(self, project: Project, entity: Entity):
        super().__init__(f"Delete {entity.name}")
        self.project, self.entity = project, entity
        self.inside = descendants(project.entities(), entity.id)

    def redo(self) -> None:
        self.project.delete_entity(self.entity.id)  # the database deletes the children too

    def undo(self) -> None:
        for entity in [self.entity, *self.inside]:
            self.project.put_entity(entity)


class SetEntityCommand(QUndoCommand):
    """Replace an entity's name and components (one Inspector edit)."""

    def __init__(self, project: Project, new: Entity, label: str):
        super().__init__(f"Change {new.name} {label}".strip())
        old = project.entity(new.id)
        if old is None:
            raise KeyError(new.id)
        self.project, self.old, self.new = project, old, new

    def redo(self) -> None:
        self.project.put_entity(self.new)

    def undo(self) -> None:
        self.project.put_entity(self.old)
