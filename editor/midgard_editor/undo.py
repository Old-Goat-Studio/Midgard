# SPDX-License-Identifier: Apache-2.0
"""Undoable edits. Every change the editor makes to a project is a QUndoCommand pushed onto
project.undo_stack, so Edit ▸ Undo and the History panel cover everything."""
from typing import Any

from PySide6.QtGui import QUndoCommand

from .objects import ObjectDef
from .project import Project
from .scene_model import MAIN_SCENE, Entity, descendants


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


class SetSceneSettingsCommand(QUndoCommand):
    """Replace a scene's settings (its world shape and fallback colours)."""

    def __init__(self, project: Project, settings: dict[str, Any], label: str,
                 scene: str = MAIN_SCENE):
        super().__init__(f"Change {label}")
        self.project, self.scene, self.new = project, scene, settings
        self.old = project.scene_settings(scene)

    def redo(self) -> None:
        self.project.set_scene_settings(self.new, self.scene)

    def undo(self) -> None:
        self.project.set_scene_settings(self.old, self.scene)


class SetPhysicsLayersCommand(QUndoCommand):
    """Replace the project's collision layers and the pairs that don't collide."""

    def __init__(self, project: Project, names: list[str], ignored: set[tuple[int, int]]):
        super().__init__("Change physics layers")
        self.project, self.new = project, (list(names), set(ignored))
        self.old = (project.physics_layers(), project.physics_ignored())

    def redo(self) -> None:
        self.project.set_physics_layers(*self.new)

    def undo(self) -> None:
        self.project.set_physics_layers(*self.old)


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


class AddObjectCommand(QUndoCommand):
    """A new object in the project's library (an imported model, or a template)."""

    def __init__(self, project: Project, obj: ObjectDef, parent: QUndoCommand | None = None):
        super().__init__(f"Add object {obj.name}", parent)
        self.project, self.obj = project, obj

    def redo(self) -> None:
        self.project.put_object(self.obj)

    def undo(self) -> None:
        self.project.delete_object(self.obj.id)


class SetObjectCommand(QUndoCommand):
    """Replace an object's name, settings or parts (one Object editor edit)."""

    def __init__(self, project: Project, new: ObjectDef, label: str):
        super().__init__(f"Change object {new.name} {label}".strip())
        old = project.object(new.id)
        if old is None:
            raise KeyError(new.id)
        self.project, self.old, self.new = project, old, new

    def redo(self) -> None:
        self.project.put_object(self.new)

    def undo(self) -> None:
        self.project.put_object(self.old)


class DeleteObjectCommand(QUndoCommand):
    """Remove an object from the library (its files stay in objects/; its instances show
    nothing until it comes back)."""

    def __init__(self, project: Project, obj: ObjectDef):
        super().__init__(f"Delete object {obj.name}")
        self.project, self.obj = project, obj

    def redo(self) -> None:
        self.project.delete_object(self.obj.id)

    def undo(self) -> None:
        self.project.put_object(self.obj)
