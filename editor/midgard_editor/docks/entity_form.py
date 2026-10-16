# SPDX-License-Identifier: Apache-2.0
"""Inspector form for one entity: name, transform, and the settings of its components. Add
Component adds a script (a new file in scripts/, opened in the Script editor), a trigger, or
physics: a collider (its shape), a rigid body (how it moves, its mass and collision layer) and a
physics material (friction and bounciness). A folder has only its name, and the sky its sky and
ambient light colours."""
import copy
from typing import Any

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
from ..scene_model import Entity, sky_component
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

    ADDABLE = {"script": "Script", "trigger": "Trigger", "collider": "Collider",
               "rigidbody": "Rigid body", "physics_material": "Physics material"}
    SKY_COLOURS = {"color": "Sky colour", "ambient": "Ambient light"}
    COLLIDER_SHAPES = {"box": "Box", "sphere": "Sphere", "capsule": "Capsule",
                       "convex": "Convex (wraps its mesh)", "mesh": "Mesh (static only)"}
    # Which collider settings each shape uses.
    COLLIDER_ROWS = {"box": {"size", "offset"}, "sphere": {"radius", "offset"},
                     "capsule": {"radius", "height", "offset"}, "convex": {"offset"},
                     "mesh": {"offset"}, "heightfield": set()}
    MOTIONS = {"dynamic": "Dynamic (falls, gets pushed)", "kinematic": "Kinematic (moved by "
               "the game)", "static": "Static (never moves)"}

    def __init__(self, project: Project, entity_id: str, parent: QWidget | None = None):
        super().__init__(parent)
        self.project, self.entity_id = project, entity_id
        form = self.form = QFormLayout(self)
        self.name = QLineEdit()
        form.addRow("Name", self.name)
        self.name.editingFinished.connect(self._name_edited)
        entity = self._entity()
        # Folders stay at the origin (so moving things in and out doesn't move them), and the
        # sky is everywhere: neither has a transform to edit.
        self.position = self.rotation = self.scale = None
        if not entity.is_folder and "sky" not in entity.components:
            self.position = Vec3Editor(0.1)
            self.rotation = Vec3Editor(5.0, -360.0, 360.0)
            self.scale = Vec3Editor(0.1, 0.001)
            form.addRow("Position", self.position)
            form.addRow("Rotation", self.rotation)
            form.addRow("Scale", self.scale)
            for label, editor in (("position", self.position), ("rotation", self.rotation),
                                  ("scale", self.scale)):
                for box in editor.boxes:
                    box.valueChanged.connect(lambda _v, k=label: self._transform_edited(k))

        self.sky: dict[str, QPushButton] = {}
        if "sky" in entity.components:
            for key, label in self.SKY_COLOURS.items():
                button = QPushButton()
                button.clicked.connect(lambda _checked=False, k=key: self._pick_sky(k))
                form.addRow(label, button)
                self.sky[key] = button
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
        self._build_physics_rows(entity)
        missing = [k for k in self.ADDABLE if k not in entity.components]
        self.add_component = QPushButton("Add Component")
        menu = QMenu(self.add_component)
        for key in missing:
            menu.addAction(self.ADDABLE[key], lambda k=key: self.add(k))
        self.add_component.setMenu(menu)
        self.add_component.setEnabled(bool(missing))
        if self.position is not None:  # things in the world, not folders or the sky
            form.addRow("", self.add_component)
        self._kinds = set(entity.components)
        project.entities_changed.connect(self._changed)
        self.refresh()

    def _build_physics_rows(self, entity: Entity) -> None:
        form = self.form
        self.collider_shape = self.collider_size = self.collider_radius = None
        self.collider_height = self.collider_offset = None
        if "collider" in entity.components:
            self.collider_shape = QComboBox()
            for key, label in self.COLLIDER_SHAPES.items():
                self.collider_shape.addItem(label, key)
            if entity.components["collider"].get("shape") == "heightfield":
                self.collider_shape.addItem("Heightfield", "heightfield")
            self.collider_shape.currentIndexChanged.connect(
                lambda _i: self._set_value("collider", "shape", self.collider_shape.currentData(),
                                           "collider shape"))
            form.addRow("Collider", _row(self.collider_shape, _remove_button(
                "Remove the collider", lambda: self.remove_component("collider"))))
            self.collider_size = Vec3Editor(0.1, 0.001)
            self.collider_radius = _spin(0.001, 100000.0, 0.05)
            self.collider_height = _spin(0.002, 100000.0, 0.1)
            self.collider_offset = Vec3Editor(0.1)
            for editor, key in ((self.collider_size, "size"), (self.collider_offset, "offset")):
                for box in editor.boxes:
                    box.valueChanged.connect(lambda _v, e=editor, k=key: self._set_value(
                        "collider", k, e.value(), f"collider {k}"))
            for box, key in ((self.collider_radius, "radius"), (self.collider_height, "height")):
                box.valueChanged.connect(lambda _v, b=box, k=key: self._set_value(
                    "collider", k, round(b.value(), 4), f"collider {k}"))
            form.addRow("Size", self.collider_size)
            form.addRow("Radius", self.collider_radius)
            form.addRow("Height", self.collider_height)
            form.addRow("Offset", self.collider_offset)
        self.motion = self.mass = self.layer = None
        if "rigidbody" in entity.components:
            self.motion = QComboBox()
            for key, label in self.MOTIONS.items():
                self.motion.addItem(label, key)
            self.motion.currentIndexChanged.connect(lambda _i: self._set_value(
                "rigidbody", "motion", self.motion.currentData(), "motion"))
            form.addRow("Rigid body", _row(self.motion, _remove_button(
                "Remove the rigid body (it becomes static scenery)",
                lambda: self.remove_component("rigidbody"))))
            self.mass = _spin(0.0, 1.0e6, 1.0, 2)
            self.mass.setSuffix(" kg")
            self.mass.setSpecialValueText("From its size")
            self.mass.valueChanged.connect(lambda _v: self._set_value(
                "rigidbody", "mass", round(self.mass.value(), 4), "mass"))
            form.addRow("Mass", self.mass)
            self.layer = QComboBox()
            for i, name in enumerate(self.project.physics_layers()):
                if name or i == 0:
                    self.layer.addItem(f"{i}: {name or 'default'}", i)
            self.layer.currentIndexChanged.connect(lambda _i: self._set_value(
                "rigidbody", "layer", self.layer.currentData(), "collision layer"))
            form.addRow("Layer", self.layer)
        self.player_rows: dict[str, QDoubleSpinBox] = {}
        if "player" in entity.components:
            for key, label, low, high, step, suffix in (
                    ("speed", "Walk speed", 0.0, 100.0, 0.5, " m/s"),
                    ("jump", "Jump speed", 0.0, 50.0, 0.5, " m/s"),
                    ("height", "Height", 0.02, 20.0, 0.1, " m"),
                    ("radius", "Radius", 0.01, 10.0, 0.05, " m")):
                box = _spin(low, high, step, 2)
                box.setSuffix(suffix)
                box.valueChanged.connect(lambda _v, b=box, k=key, lab=label: self._set_value(
                    "player", k, round(b.value(), 4), lab.lower()))
                form.addRow(label, box)
                self.player_rows[key] = box
        self.friction = self.bounciness = None
        if "physics_material" in entity.components:
            self.friction = _spin(0.0, 2.0, 0.05, 2)
            self.bounciness = _spin(0.0, 1.0, 0.05, 2)
            for box, key in ((self.friction, "friction"), (self.bounciness, "bounciness")):
                box.valueChanged.connect(lambda _v, b=box, k=key: self._set_value(
                    "physics_material", k, round(b.value(), 4), k))
            form.addRow("Friction", _row(self.friction, _remove_button(
                "Remove the physics material", lambda: self.remove_component(
                    "physics_material"))))
            form.addRow("Bounciness", self.bounciness)

    def _refresh_physics(self, entity: Entity) -> None:
        def show(combo: QComboBox, value: Any) -> None:
            combo.blockSignals(True)
            if combo.findData(value) < 0:
                combo.addItem(f"{value} (unknown)", value)
            combo.setCurrentIndex(combo.findData(value))
            combo.blockSignals(False)

        def number(box: QDoubleSpinBox, value: float) -> None:
            box.blockSignals(True)
            box.setValue(float(value))
            box.blockSignals(False)

        if self.collider_shape is not None:
            c = entity.components["collider"]
            shape = c.get("shape", "box")
            show(self.collider_shape, shape)
            self.collider_size.set_value(c.get("size", [1.0, 1.0, 1.0]))
            number(self.collider_radius, c.get("radius", 0.5))
            number(self.collider_height, c.get("height", 2.0))
            self.collider_offset.set_value(c.get("offset", [0.0, 0.0, 0.0]))
            rows = self.COLLIDER_ROWS.get(shape, set())
            for key, widget in (("size", self.collider_size), ("radius", self.collider_radius),
                                ("height", self.collider_height),
                                ("offset", self.collider_offset)):
                self.form.setRowVisible(widget, key in rows)
        player_defaults = {"speed": 4.0, "jump": 4.5, "height": 1.0, "radius": 0.3}
        for key, box in self.player_rows.items():
            number(box, entity.components["player"].get(key, player_defaults[key]))
        if self.motion is not None:
            b = entity.components["rigidbody"]
            show(self.motion, b.get("motion", "dynamic"))
            number(self.mass, b.get("mass", 0.0))
            show(self.layer, b.get("layer", 0))
        if self.friction is not None:
            m = entity.components["physics_material"]
            number(self.friction, m.get("friction", 0.5))
            number(self.bounciness, m.get("bounciness", 0.0))

    def _set_value(self, component: str, key: str, value: Any, label: str) -> None:
        entity = self._entity()
        if component not in entity.components or entity.components[component].get(key) == value:
            return
        entity.components[component] = {**entity.components[component], key: value}
        self._push(entity, label)

    def default_collider(self, entity: Entity) -> dict[str, Any]:
        """A collider that fits the entity's mesh: a sphere for a sphere, a box otherwise."""
        asset = entity.components.get("mesh", {}).get("asset", "")
        if asset == "prim_sphere":
            return {"shape": "sphere", "radius": 0.5}
        if asset.startswith("asset:"):
            return {"shape": "convex"}  # wraps the model
        return {"shape": "box", "size": [1.0, 1.0, 1.0]}

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
        if self.position is not None:
            t = entity.transform()
            self.position.set_value(t["position"])
            self.rotation.set_value(t["rotation"])
            self.scale.set_value(t["scale"])
        for key, button in self.sky.items():
            style_color_button(button, entity.components["sky"].get(key, sky_component()[key]))
        if self.mesh is not None:
            asset = entity.components["mesh"].get("asset", "")
            self.mesh.blockSignals(True)
            if self.mesh.findData(asset) < 0:  # e.g. a file that has gone from objects/
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
        self._refresh_physics(entity)
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
        elif kind == "collider":
            entity.components["collider"] = self.default_collider(entity)
        elif kind == "rigidbody":
            entity.components["rigidbody"] = {"motion": "dynamic"}
            # A rigid body needs a shape to collide with.
            entity.components.setdefault("collider", self.default_collider(entity))
        elif kind == "physics_material":
            entity.components["physics_material"] = {"friction": 0.5, "bounciness": 0.0}
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

    def _pick_sky(self, key: str) -> None:
        rgb = self._entity().components["sky"].get(key, sky_component()[key])
        chosen = QColorDialog.getColor(QColor.fromRgbF(*rgb[:3]), self, self.SKY_COLOURS[key])
        if chosen.isValid():
            self.set_sky_color(key, [chosen.redF(), chosen.greenF(), chosen.blueF()])

    def set_sky_color(self, key: str, rgb: list[float]) -> None:
        entity = self._entity()
        entity.components["sky"] = {**entity.components["sky"],
                                    key: [round(c, 4) for c in rgb[:3]]}
        self._push(entity, self.SKY_COLOURS[key].lower())

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
