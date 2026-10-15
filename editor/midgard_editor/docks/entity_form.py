# SPDX-License-Identifier: Apache-2.0
"""Inspector form for one entity: name, transform, and the settings of its components."""
import copy

from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QColorDialog,
    QDoubleSpinBox,
    QFormLayout,
    QHBoxLayout,
    QLineEdit,
    QPushButton,
    QSizePolicy,
    QWidget,
)

from ..project import Project
from ..scene_model import Entity
from ..undo import SetEntityCommand


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


class EntityForm(QWidget):
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
        self.color = self.fov = self.intensity = None
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
        project.entities_changed.connect(self._changed)
        self.refresh()

    def _entity(self) -> Entity:
        entity = self.project.entity(self.entity_id)
        if entity is None:
            raise KeyError(self.entity_id)
        return entity

    def _changed(self, entity_id: str) -> None:
        if entity_id == self.entity_id and self.project.entity(self.entity_id) is not None:
            self.refresh()

    def refresh(self) -> None:
        entity = self._entity()
        self.name.setText(entity.name)
        t = entity.transform()
        self.position.set_value(t["position"])
        self.rotation.set_value(t["rotation"])
        self.scale.set_value(t["scale"])
        if self.color is not None:
            rgba = entity.components["material"].get("color", [1, 1, 1, 1])
            c = QColor.fromRgbF(*rgba[:3])
            self.color.setText(c.name())
            self.color.setStyleSheet(f"background-color: {c.name()}; color: "
                                     f"{'#000' if c.lightnessF() > 0.5 else '#fff'};")
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
