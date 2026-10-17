# SPDX-License-Identifier: Apache-2.0
"""Save Group as Object (roadmap §5.5): a group built from primitives in the scene becomes an
object in the project's library, to be placed many times from Add ▸ Object.

The group's contents become the object's parts, each placed relative to the group's centre
(groups inside it are flattened: their parts keep where they are). The group itself can be
replaced by an instance of the new object, in the same place and with the same id, keeping its
own components (rigid body, physics material, scripts and so on). All of it is one step to undo.
"""
import copy
from typing import Any

from PySide6.QtWidgets import (
    QCheckBox,
    QDialog,
    QDialogButtonBox,
    QFormLayout,
    QLineEdit,
    QWidget,
)

from . import transforms
from .objects import ObjectDef, new_object_id
from .project import Project
from .scene_model import Entity
from .undo import AddObjectCommand, DeleteEntityCommand, SetEntityCommand

# What a part never carries: what makes a group a group, and its place (set from the group's).
GROUPING = ("group", "folder", "object")


def template_from_group(entities: list[Entity], group_id: str) -> list[dict[str, Any]]:
    """The parts of an object made from a group: everything inside it that has components of its
    own, placed relative to the group."""
    by_id = {e.id: e for e in entities}
    group_world = transforms.world_matrix(by_id, group_id)
    parts = []

    def visit(parent_id: str) -> None:
        for entity in sorted((e for e in entities if e.parent == parent_id), key=lambda e: e.id):
            own = {k: copy.deepcopy(v) for k, v in entity.components.items()
                   if k not in GROUPING and k != "transform"}
            if own:
                local = transforms.relative_to(group_world,
                                               transforms.world_matrix(by_id, entity.id))
                parts.append({"name": entity.name, "components": {"transform": local, **own}})
            visit(entity.id)  # (a group inside: its parts, flattened)

    visit(group_id)
    return parts


def save_group_as_object(project: Project, group_id: str, name: str | None = None,
                         replace: bool = True) -> str:
    """Makes an object of a group's contents (Undo takes it all back). With replace, the group
    becomes an instance of it. Returns the new object's id."""
    group = project.entity(group_id)
    if group is None or "group" not in group.components:
        raise ValueError(f"'{group_id}' is not a group")
    entities = project.entities()
    name = (name or group.name).strip() or group.name
    obj = ObjectDef(new_object_id({o.id for o in project.objects()}, name), name, None, {},
                    template_from_group(entities, group_id))
    stack = project.undo_stack
    stack.beginMacro(f"Save {group.name} as object {name}")
    try:
        stack.push(AddObjectCommand(project, obj))
        if replace:
            for child in [e for e in entities if e.parent == group_id]:
                stack.push(DeleteEntityCommand(project, child))
            components = {k: v for k, v in group.components.items() if k != "group"}
            components["object"] = {"id": obj.id}
            stack.push(SetEntityCommand(project, Entity(group.id, group.name, group.parent,
                                                        components), "to an object"))
    finally:
        stack.endMacro()
    return obj.id


class SaveGroupDialog(QDialog):
    def __init__(self, group: Entity, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWindowTitle("Save Group as Object")
        form = QFormLayout(self)
        self.name = QLineEdit(group.name)
        self.replace = QCheckBox("Replace the group with an instance of the object")
        self.replace.setChecked(True)
        self.replace.setToolTip("Keep it unticked to go on editing the group; save it again as "
                                "another object when it changes")
        form.addRow("Object name", self.name)
        form.addRow("", self.replace)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Save
                                   | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        form.addRow(buttons)


def ask_save_group_as_object(project: Project, group_id: str,
                             parent: QWidget | None = None) -> str | None:
    """Asks for the object's name, then saves the group. Returns the object's id."""
    group = project.entity(group_id)
    if group is None or "group" not in group.components:
        return None
    dialog = SaveGroupDialog(group, parent)
    if dialog.exec() != QDialog.DialogCode.Accepted:
        return None
    return save_group_as_object(project, group_id, dialog.name.text(),
                                dialog.replace.isChecked())
