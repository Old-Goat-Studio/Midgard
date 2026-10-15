# SPDX-License-Identifier: Apache-2.0
"""Inspector form for one entity: name, transform, and the settings of its components. Add
Component adds a script (a new file in scripts/, opened in the Script editor) or a trigger."""
import copy

from PySide6.QtCore import Signal
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QColorDialog,
    QComboBox,
    QDoubleSpinBox,
    QFormLayout,
    QHBoxLayout,
    QLineEdit,
    QMenu,
    QPushButton,
    QSizePolicy,
    QToolButton,
    QWidget,
)

from .. import scripting
from ..project import Project
from ..scene_model import Entity
from ..undo import SetEntityCommand


def style_color_button(button: QPushButton, rgb: list[float]) -> None:
    """Shows a colour on a button: its hex name on a background of that colour."""
    c = QColor.fromRgbF(*[float(v) for v in rgb[:3]])
    button.setText(c.name())
    button.setStyleSheet(f"background-color: {c.name()}; color: "
                         f"{'#000' if c.lightnessF() > 0.5 else '#fff'};")


def _spin(minimum: float, maximum: float, step: float, decimals: int = 3) -> QDoubleSpinBox:
    box = QDoubleSpinBox()
    box.setRange(minimum, maximum)
    box.setSingleStep(step)
    box.setDecimals(decimals)
    box.setKeyboardTracking(False)  # commit when editing finishes, not on every keystroke
    return box


class Vec3Editor(QWidget):
    def __init__(self, step: float, minimum: float = -100000.0, maximum: float = 100000.0):
        super().__init__()
        row = QHBoxLayout(self)
        row.setContentsMargins(0, 0, 0, 0)
        self.boxes = [_spin(minimum, maximum, step) for _ in range(3)]
        for axis, box in zip("XYZ", self.boxes, strict=True):
            box.setPrefix(f"{axis} ")
            # Share the row's width instead of asking for room for 100000.000 (which would
            # squeeze the form's labels).
            box.setMinimumWidth(64)
            box.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Fixed)
            row.addWidget(box)

    def value(self) -> list[float]:
        return [round(b.value(), 4) for b in self.boxes]

    def set_value(self, values: list[float]) -> None:
        for box, v in zip(self.boxes, values, strict=True):
            box.blockSignals(True)
            box.setValue(float(v))
            box.blockSignals(False)


def _row(*widgets: QWidget) -> QWidget:
    row = QWidget()
    layout = QHBoxLayout(row)
    layout.setContentsMargins(0, 0, 0, 0)
    for w in widgets:
        layout.addWidget(w)
    return row


def _remove_button(tooltip: str, slot) -> QToolButton:
    button = QToolButton()
    button.setText("✕")
    button.setToolTip(tooltip)
    button.clicked.connect(slot)
    return button


class EntityForm(QWidget):
    open_script = Signal(str)  # a module to open in the Script editor
    rebuild = Signal(str)  # the entity's set of components changed: build the form again

    ADDABLE = {"script": "Script", "trigger": "Trigger"}

    def __init__(self, project: Project, entity_id: str, parent: QWidget | None = None):
        super().__init__(parent)
        self.project, self.entity_id = project, entity_id
        form = QFormLayout(self)
        self.name = QLineEdit()
        self.position = Vec3Editor(0.1)
        self.rotation = Vec3Editor(5.0, -360.0, 360.0)
        self.scale = Vec3Editor(0.1, 0.001)
        form.addRow("Name", self.name)
        form.addRow("Position", self.position)
        form.addRow("Rotation", self.rotation)
        form.addRow("Scale", self.scale)
        self.name.editingFinished.connect(self._name_edited)
        for label, editor in (("position", self.position), ("rotation", self.rotation),
                              ("scale", self.scale)):
            for box in editor.boxes:
                box.valueChanged.connect(lambda _v, k=label: self._transform_edited(k))

        entity = self._entity()
        self.color = self.fov = self.intensity = self.mesh = None
        if "mesh" in entity.components:
            self.mesh = QComboBox()
            for asset, label in project.mesh_assets().items():
                self.mesh.addItem(label, asset)
            self.mesh.currentIndexChanged.connect(self._mesh_chosen)
            form.addRow("Mesh", self.mesh)
        if "material" in entity.components:
            self.color = QPushButton()
            self.color.clicked.connect(self._pick_color)
            form.addRow("Colour", self.color)
        if "camera" in entity.components:
            self.fov = _spin(10.0, 120.0, 1.0, 1)
            self.fov.setSuffix("°")
            self.fov.valueChanged.connect(lambda _v: self._component_value("camera", "fov",
                                                                           self.fov.value()))
            form.addRow("Field of view", self.fov)
        if "light" in entity.components:
            self.intensity = _spin(0.0, 100.0, 0.1, 2)
            self.intensity.valueChanged.connect(
                lambda _v: self._component_value("light", "intensity", self.intensity.value()))
            form.addRow("Intensity", self.intensity)
        self.script = self.trigger = None
        if "script" in entity.components:
            self.script = QComboBox()
            self.script.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Fixed)
            for module in scripting.modules(project):
                self.script.addItem(module, module)
            self.script.currentIndexChanged.connect(lambda _i: self.set_script(
                self.script.currentData()))
            edit = QPushButton("Edit")
            edit.setToolTip("Open the script in the Script editor")
            edit.clicked.connect(lambda: self.open_script.emit(
                self._entity().components["script"].get("module", "")))
            form.addRow("Script", _row(self.script, edit, _remove_button(
                "Remove the script (the file stays)", lambda: self.remove_component("script"))))
        if "trigger" in entity.components:
            self.trigger = Vec3Editor(0.1, 0.01)
            for box in self.trigger.boxes:
                box.valueChanged.connect(lambda _v: self._trigger_edited())
            form.addRow("Trigger size", _row(self.trigger, _remove_button(
                "Remove the trigger", lambda: self.remove_component("trigger"))))
        missing = [k for k in self.ADDABLE if k not in entity.components]
        self.add_component = QPushButton("Add Component")
        menu = QMenu(self.add_component)
        for key in missing:
            menu.addAction(self.ADDABLE[key], lambda k=key: self.add(k))
        self.add_component.setMenu(menu)
        self.add_component.setEnabled(bool(missing))
        form.addRow("", self.add_component)
        self._kinds = set(entity.components)
        project.entities_changed.connect(self._changed)
        self.refresh()

    def _entity(self) -> Entity:
        entity = self.project.entity(self.entity_id)
        if entity is None:
            raise KeyError(self.entity_id)
        return entity

    def _changed(self, entity_id: str) -> None:
        if entity_id != self.entity_id:
            return
        entity = self.project.entity(self.entity_id)
        if entity is None:
            return
        if set(entity.components) != self._kinds:
            self.rebuild.emit(self.entity_id)  # rows to add or remove (also after Undo)
        else:
            self.refresh()

    def refresh(self) -> None:
        entity = self._entity()
        self.name.setText(entity.name)
        t = entity.transform()
        self.position.set_value(t["position"])
        self.rotation.set_value(t["rotation"])
        self.scale.set_value(t["scale"])
        if self.mesh is not None:
            asset = entity.components["mesh"].get("asset", "")
            self.mesh.blockSignals(True)
            if self.mesh.findData(asset) < 0:  # e.g. a file that has gone from assets/
                self.mesh.addItem(f"{asset} (missing)", asset)
            self.mesh.setCurrentIndex(self.mesh.findData(asset))
            self.mesh.blockSignals(False)
        if self.color is not None:
            style_color_button(self.color,
                               entity.components["material"].get("color", [1, 1, 1, 1]))
        if self.script is not None:
            module = entity.components["script"].get("module", "")
            self.script.blockSignals(True)
            if self.script.findData(module) < 0:
                self.script.addItem(f"{module} (missing)", module)
            self.script.setCurrentIndex(self.script.findData(module))
            self.script.blockSignals(False)
        if self.trigger is not None:
            self.trigger.set_value(entity.components["trigger"].get("size", [1.0, 1.0, 1.0]))
        for box, component, key, default in ((self.fov, "camera", "fov", 55.0),
                                             (self.intensity, "light", "intensity", 1.0)):
            if box is not None:
                box.blockSignals(True)
                box.setValue(float(entity.components[component].get(key, default)))
                box.blockSignals(False)

    def _push(self, entity: Entity, label: str) -> None:
        self.project.undo_stack.push(SetEntityCommand(self.project, entity, label))

    def _name_edited(self) -> None:
        entity = self._entity()
        name = self.name.text().strip()
        if name and name != entity.name:
            entity.name = name
            self._push(entity, "name")

    def _transform_edited(self, key: str) -> None:
        entity = self._entity()
        editor = {"position": self.position, "rotation": self.rotation, "scale": self.scale}[key]
        transform = copy.deepcopy(entity.components.get("transform", {}))
        if transform.get(key) == editor.value():
            return
        transform[key] = editor.value()
        entity.components["transform"] = transform
        self._push(entity, key)

    def _component_value(self, component: str, key: str, value: float) -> None:
        entity = self._entity()
        if entity.components[component].get(key) == round(value, 4):
            return
        entity.components[component][key] = round(value, 4)
        self._push(entity, key.replace("_", " "))

    # ---- components ------------------------------------------------------------------------
    def add(self, kind: str) -> None:
        entity = self._entity()
        if kind == "script":
            module = scripting.new_module_name(self.project, entity.name)
            scripting.create_script(self.project, module, f"The {entity.name} script.")
            entity.components["script"] = {"module": module}
        elif kind == "trigger":
            entity.components["trigger"] = {"size": [1.0, 1.0, 1.0]}
        self._push(entity, self.ADDABLE[kind].lower())
        if kind == "script":
            self.open_script.emit(entity.components["script"]["module"])

    def remove_component(self, kind: str) -> None:
        entity = self._entity()
        if entity.components.pop(kind, None) is not None:
            self._push(entity, f"{kind} removed")

    def set_script(self, module: str) -> None:
        entity = self._entity()
        if module and entity.components["script"].get("module") != module:
            entity.components["script"] = {**entity.components["script"], "module": module}
            self._push(entity, "script")

    def _trigger_edited(self) -> None:
        entity = self._entity()
        size = self.trigger.value()
        if entity.components["trigger"].get("size") != size:
            entity.components["trigger"] = {**entity.components["trigger"], "size": size}
            self._push(entity, "trigger size")

    def _mesh_chosen(self, _index: int) -> None:
        self.set_mesh(self.mesh.currentData())

    def set_mesh(self, asset: str) -> None:
        entity = self._entity()
        if entity.components["mesh"].get("asset") == asset:
            return
        entity.components["mesh"] = {**entity.components["mesh"], "asset": asset}
        self._push(entity, "mesh")

    def _pick_color(self) -> None:
        entity = self._entity()
        rgba = entity.components["material"].get("color", [1, 1, 1, 1])
        chosen = QColorDialog.getColor(QColor.fromRgbF(*rgba[:3]), self, "Colour")
        if chosen.isValid():
            self.set_color([chosen.redF(), chosen.greenF(), chosen.blueF()])

    def set_color(self, rgb: list[float]) -> None:
        entity = self._entity()
        alpha = entity.components["material"].get("color", [1, 1, 1, 1])[3:4] or [1.0]
        entity.components["material"]["color"] = [round(c, 4) for c in rgb] + alpha
        self._push(entity, "colour")
