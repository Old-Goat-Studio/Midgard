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
    QCheckBox,
    QColorDialog,
    QComboBox,
    QDoubleSpinBox,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMenu,
    QPushButton,
    QSizePolicy,
    QToolButton,
    QWidget,
)

from .. import engine, environments, scenes, scripting, transforms
from ..group_objects import ask_save_group_as_object
from ..project import Project
from ..scene_model import Entity, sky_component
from ..undo import SetEntityCommand
from .camera_editor import CameraDialog
from .material_editor import EntityMaterial, MaterialDialog
from .rig_dialog import RigDialog, rig_summary


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
    open_object = Signal(str)  # an object to show in the Objects panel
    rebuild = Signal(str)  # the entity's set of components changed: build the form again

    ADDABLE = {"script": "Script", "trigger": "Trigger", "collider": "Collider",
               "rigidbody": "Rigid body", "physics_material": "Physics material",
               "scene_exit": "Scene exit", "arrival": "Arrival point",
               "character": "Character"}
    ACTIVATIONS = {"enter": "Walking into it", "use": "Using it (Use: E or gamepad X, inside it)",
                   "script": "Only from a script"}
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
        self.environment = None
        if "sky" in entity.components:
            self.environment = QComboBox()
            self.environment.setToolTip("How the world looks through its day: the sky, sunlight, "
                                        "ambient light and fog follow the clock, and so does the "
                                        "sun. None: the flat colours below.")
            self.environment.addItem("None (flat colours)", "")
            for env in environments.load_all(project):
                self.environment.addItem(env["name"], env["id"])
            self.environment.currentIndexChanged.connect(
                lambda _i: self.set_sky_environment(self.environment.currentData()))
            form.addRow("Environment", self.environment)
            for key, label in self.SKY_COLOURS.items():
                button = QPushButton()
                button.clicked.connect(lambda _checked=False, k=key: self._pick_sky(k))
                form.addRow(label, button)
                self.sky[key] = button
        self.color = self.fov = self.intensity = self.mesh = None
        self.follows_clock = None
        if "mesh" in entity.components:
            self.mesh = QComboBox()
            for asset, label in project.mesh_assets().items():
                self.mesh.addItem(label, asset)
            self.mesh.currentIndexChanged.connect(self._mesh_chosen)
            form.addRow("Mesh", self.mesh)
        if "material" in entity.components:
            self.color = QPushButton()
            self.color.clicked.connect(self._pick_color)
            edit = QPushButton("Edit…")
            edit.setToolTip("Metal, roughness, emission and textures, in the Material editor")
            edit.clicked.connect(self.edit_material)
            form.addRow("Colour", _row(self.color, edit))
        if "camera" in entity.components:
            self.fov = _spin(10.0, 120.0, 1.0, 1)
            self.fov.setSuffix("°")
            self.fov.valueChanged.connect(lambda _v: self._component_value("camera", "fov",
                                                                           self.fov.value()))
            edit = QPushButton("Edit…")
            edit.setToolTip("The views of the player's camera, in the Camera editor")
            edit.clicked.connect(self.edit_camera)
            form.addRow("Field of view", _row(self.fov, edit))
        if "light" in entity.components:
            self.intensity = _spin(0.0, 100.0, 0.1, 2)
            self.intensity.valueChanged.connect(
                lambda _v: self._component_value("light", "intensity", self.intensity.value()))
            form.addRow("Intensity", self.intensity)
            if entity.components["light"].get("type", "directional") == "directional":
                self.follows_clock = QCheckBox("Follows the clock")
                self.follows_clock.setToolTip(
                    "The sun rises and sets with the game's clock (and the Calendar dock's "
                    "scrubber). Where it is placed is where it stands at solar noon.")
                self.follows_clock.toggled.connect(lambda on: self._set_value(
                    "light", "follows_clock", on, "follows the clock"))
                form.addRow("", self.follows_clock)
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
        self._build_scene_exit(entity, form)
        self._build_character(entity, form)
        self.object = None
        if "object" in entity.components:
            self.object = QComboBox()
            self.object.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Fixed)
            for obj in sorted(project.objects(), key=lambda o: o.name.lower()):
                self.object.addItem(obj.name, obj.id)
            self.object.currentIndexChanged.connect(
                lambda _i: self.set_object(self.object.currentData()))
            edit = QPushButton("Edit")
            edit.setToolTip("Show the object in the Objects panel")
            edit.clicked.connect(lambda: self.open_object.emit(
                self._entity().components["object"].get("id", "")))
            form.addRow("Object", _row(self.object, edit))
            project.objects_changed.connect(self._objects_changed)
        self._build_physics_rows(entity)
        if "group" in entity.components:
            centre = QPushButton("Centre on contents")
            centre.setToolTip("Move the group's centre to the middle of its parts (they stay put)")
            centre.clicked.connect(self.centre_group)
            save = QPushButton("Save as Object…")
            save.setToolTip("Make an object of the group, to place many times from "
                            "Add ▸ Object")
            save.clicked.connect(lambda: ask_save_group_as_object(self.project, self.entity_id,
                                                                  self.window()))
            form.addRow("Group", _row(centre, save))
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
                    ("run_multiplier", "Running speed", 1.0, 5.0, 0.1, " ×"),
                    ("jump", "Jump speed", 0.0, 50.0, 0.5, " m/s"),
                    ("height", "Height", 0.02, 20.0, 0.1, " m"),
                    ("radius", "Radius", 0.01, 10.0, 0.05, " m")):
                box = _spin(low, high, step, 2)
                box.setSuffix(suffix)
                box.valueChanged.connect(lambda _v, b=box, k=key, lab=label: self._set_value(
                    "player", k, round(b.value(), 4), lab.lower()))
                if key == "run_multiplier":
                    box.setToolTip("The walk speed times this while the run action is held "
                                   "(Left Shift; 1: no running)")
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
        player_defaults = {"speed": 4.0, "run_multiplier": 2.0, "jump": 4.5, "height": 1.0,
                           "radius": 0.3}
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
        """A collider that fits the entity's mesh: a sphere for a sphere, a convex shape for a
        cylinder, a wedge or a model, a box otherwise."""
        asset = entity.components.get("mesh", {}).get("asset", "")
        if asset == "prim_sphere":
            return {"shape": "sphere", "radius": 0.5}
        if asset.startswith("asset:") or asset in ("prim_cylinder", "prim_wedge"):
            return {"shape": "convex"}  # wraps the shape
        return {"shape": "box", "size": [1.0, 1.0, 1.0]}

    def centre_group(self) -> None:
        """Puts the group's centre in the middle of its parts' positions, keeping every part where
        it is in the world. One undo step."""
        entities = {e.id: e for e in self.project.entities()}
        group = entities[self.entity_id]
        parts = [e for e in entities.values() if e.parent == group.id and "transform"
                 in e.components]
        if not parts:
            return
        worlds = {p.id: transforms.world_matrix(entities, p.id) for p in parts}
        points = [transforms.position_of(m) for m in worlds.values()]
        lo = [min(p[i] for p in points) for i in range(3)]
        hi = [max(p[i] for p in points) for i in range(3)]
        centre = [(lo[i] + hi[i]) / 2 for i in range(3)]
        group_world = transforms.world_matrix(entities, group.id)
        for i in range(3):
            group_world[i][3] = centre[i]
        new_group = copy.deepcopy(group)
        new_group.components["transform"] = transforms.relative_to(
            transforms.world_matrix(entities, group.parent), group_world)
        stack = self.project.undo_stack
        stack.beginMacro(f"Centre {group.name} on contents")
        self._push(new_group, "centre")
        entities[group.id] = new_group
        for part in parts:
            moved = copy.deepcopy(part)
            moved.components["transform"] = transforms.relative_to(
                transforms.world_matrix(entities, group.id), worlds[part.id])
            self.project.undo_stack.push(SetEntityCommand(self.project, moved, "kept in place"))
        stack.endMacro()

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
        if self.object is not None:
            object_id = entity.components["object"].get("id", "")
            self.object.blockSignals(True)
            if self.object.findData(object_id) < 0:  # not in the library (deleted)
                self.object.addItem(f"{object_id} (missing)", object_id)
            self.object.setCurrentIndex(self.object.findData(object_id))
            self.object.blockSignals(False)
        if self.position is not None:
            t = entity.transform()
            self.position.set_value(t["position"])
            self.rotation.set_value(t["rotation"])
            self.scale.set_value(t["scale"])
        for key, button in self.sky.items():
            style_color_button(button, entity.components["sky"].get(key, sky_component()[key]))
        if self.environment is not None:
            link = entity.components["sky"].get("environment", "")
            self.environment.blockSignals(True)
            if self.environment.findData(link) < 0:  # not in the project (deleted)
                self.environment.addItem(f"{link} (missing)", link)
            self.environment.setCurrentIndex(self.environment.findData(link))
            self.environment.blockSignals(False)
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
        self._refresh_scene_exit(entity)
        self._refresh_character(entity)
        if self.follows_clock is not None:
            self.follows_clock.blockSignals(True)
            self.follows_clock.setChecked(bool(entity.components["light"].get("follows_clock")))
            self.follows_clock.blockSignals(False)
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
        value = editor.value()
        if key == "scale" and "group" in entity.components:
            # A group scales evenly: the changed box sets all three.
            old = entity.transform()["scale"]
            changed = next((v for v, o in zip(value, old, strict=True) if v != o), value[0])
            value = [changed] * 3
            editor.set_value(value)
        if transform.get(key) == value:
            return
        transform[key] = value
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
        elif kind == "scene_exit":
            entity.components["scene_exit"] = {"scene": "", "arrival": "", "activation": "enter",
                                               "size": [2.0, 2.5, 1.0], "fade": 0.5}
        elif kind == "arrival":
            entity.components["arrival"] = {"name": entity.name}
        elif kind == "character":
            models = list(self.project.character_assets())
            entity.components["character"] = {"model": models[0] if models else "",
                                              "clip": "", "speed": 1.0}
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

    # ---- characters (milestone 5.13) --------------------------------------------------------
    def _build_character(self, entity, form: QFormLayout) -> None:
        self.character_model = self.character_clips = self.character_clip = None
        self.character_speed = self.character_rig = self.character_rig_button = None
        if "character" not in entity.components:
            return
        models = self.project.character_assets()
        self.character_model = QComboBox()
        self.character_model.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Fixed)
        self.character_model.addItem("(choose a model)", "")
        for asset, label in models.items():
            self.character_model.addItem(label, asset)
        self.character_model.setToolTip("A skinned .glb or .gltf in the project's objects/ "
                                        "folder")
        self.character_model.currentIndexChanged.connect(lambda _i: self._character_value(
            "model", self.character_model.currentData() or "", "model"))
        form.addRow("Character", _row(self.character_model, _remove_button(
            "No longer a character", lambda: self.remove_component("character"))))
        self.character_clips = QComboBox()
        self.character_clips.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Fixed)
        self.character_clips.addItem("Its own", "")
        for asset, label in models.items():
            self.character_clips.addItem(label, asset)
        self.character_clips.setToolTip("Where its animations come from: its own file, or "
                                        "another (an animation library) with the same joints")
        self.character_clips.currentIndexChanged.connect(lambda _i: self._character_value(
            "clips", self.character_clips.currentData() or "", "animations"))
        form.addRow("Animations from", self.character_clips)
        self.character_clip = QComboBox()
        self.character_clip.currentIndexChanged.connect(lambda _i: self._character_value(
            "clip", self.character_clip.currentData() or "", "clip"))
        form.addRow("Playing", self.character_clip)
        self.character_speed = QDoubleSpinBox()
        self.character_speed.setRange(0.0, 10.0)
        self.character_speed.setSingleStep(0.1)
        self.character_speed.setKeyboardTracking(False)
        self.character_speed.valueChanged.connect(lambda v: self._set_value(
            "character", "speed", round(v, 3), "animation speed"))
        form.addRow("Speed", self.character_speed)
        self.character_rig = QLabel()
        self.character_rig.setWordWrap(True)
        self.character_rig_button = QPushButton("Rig...")
        self.character_rig_button.setToolTip("Which of the model's joints is each humanoid bone")
        self.character_rig_button.clicked.connect(self.edit_rig)
        form.addRow("Rig", _row(self.character_rig, self.character_rig_button))
        self._refresh_character(entity)

    def model_info(self, asset: str) -> dict | None:
        """A model file's joints, clips and rig (skinned_info; none if it can't be read, or
        isn't skinned)."""
        from ..cook import mesh_asset_data
        module = engine.module()
        data = mesh_asset_data(self.project, asset) if asset else None
        if module is None or data is None:
            return None
        try:
            return module.skinned_info(data)
        except ValueError:
            return None

    def clips_in(self, asset: str) -> list[str]:
        """The clips in a model file (none if it can't be read, or isn't skinned)."""
        info = self.model_info(asset)
        return [c["name"] for c in info["clips"]] if info else []

    def rig_dialog(self) -> RigDialog | None:
        """The rig dialog for the character's model (none if it hasn't a skinned one)."""
        c = self._entity().components.get("character", {})
        info = self.model_info(c.get("model", ""))
        if info is None:
            return None
        return RigDialog(info["joints"], info["rig"], dict(c.get("rig", {})),
                         engine.module().humanoid_bones(), self)

    def edit_rig(self) -> None:
        dialog = self.rig_dialog()
        if dialog is not None and dialog.exec():
            self._character_value("rig", dialog.chosen(), "rig")

    def _character_value(self, key: str, value, label: str) -> None:
        entity = self._entity()
        if "character" not in entity.components:
            return
        unset = {} if key == "rig" else None  # (no rig: the model's names say it all)
        if entity.components["character"].get(key, unset) == value:
            return
        character = {**entity.components["character"], key: value}
        if key == "rig" and not value:
            del character["rig"]
        if key in ("model", "clips"):  # (another file: its clips, the first of them playing)
            clips = self.clips_in(character.get("clips") or character.get("model", ""))
            if character.get("clip") not in clips:
                character["clip"] = clips[0] if clips else ""
        entity.components["character"] = character
        self._push(entity, label)

    def _refresh_character(self, entity) -> None:
        if self.character_model is None or "character" not in entity.components:
            return
        c = entity.components["character"]
        for combo, value in ((self.character_model, c.get("model", "")),
                             (self.character_clips, c.get("clips", ""))):
            combo.blockSignals(True)
            if combo.findData(value) < 0:
                combo.addItem(f"{value} (missing)", value)
            combo.setCurrentIndex(combo.findData(value))
            combo.blockSignals(False)
        self.character_clip.blockSignals(True)
        self.character_clip.clear()
        clips = self.clips_in(c.get("clips") or c.get("model", ""))
        if not clips:
            self.character_clip.addItem("(its first, or none)", "")
        for name in clips:
            self.character_clip.addItem(name, name)
        current = c.get("clip", "")
        if self.character_clip.findData(current) < 0:
            self.character_clip.addItem(f"{current} (missing)", current)
        self.character_clip.setCurrentIndex(self.character_clip.findData(current))
        self.character_clip.blockSignals(False)
        self.character_speed.blockSignals(True)
        self.character_speed.setValue(float(c.get("speed", 1.0)))
        self.character_speed.blockSignals(False)
        info = self.model_info(c.get("model", ""))
        self.character_rig_button.setEnabled(info is not None)
        self.character_rig.setText(
            rig_summary(info["rig"], c.get("rig", {}), engine.module().humanoid_bones())
            if info else "(no skinned model)")

    # ---- moving between scenes (milestone 5.11a) -------------------------------------------
    def _build_scene_exit(self, entity, form: QFormLayout) -> None:
        self.arrival_name = self.exit_scene = self.exit_arrival = None
        self.exit_activation = self.exit_size = self.exit_fade = None
        if "arrival" in entity.components:
            self.arrival_name = QLineEdit(entity.components["arrival"].get("name", ""))
            self.arrival_name.setToolTip("What scene exits call this place (players arrive "
                                         "here facing along the arrow)")
            self.arrival_name.editingFinished.connect(lambda: self._set_value(
                "arrival", "name", self.arrival_name.text().strip(), "arrival name"))
            form.addRow("Arrival", _row(self.arrival_name, _remove_button(
                "No longer an arrival point", lambda: self.remove_component("arrival"))))
        if "scene_exit" in entity.components:
            exit_ = entity.components["scene_exit"]
            self.exit_scene = QComboBox()
            self.exit_scene.addItem("(choose a scene)", "")
            for s in scenes.load_all(self.project):
                self.exit_scene.addItem(s["name"], s["id"])
            if self.exit_scene.findData(exit_.get("scene", "")) < 0:
                self.exit_scene.addItem(f"{exit_['scene']} (missing)", exit_["scene"])
            self.exit_scene.setCurrentIndex(self.exit_scene.findData(exit_.get("scene", "")))
            self.exit_scene.currentIndexChanged.connect(lambda _i: self._exit_scene_chosen())
            form.addRow("Leads to", _row(self.exit_scene, _remove_button(
                "No longer a scene exit", lambda: self.remove_component("scene_exit"))))
            self.exit_arrival = QComboBox()
            self._fill_arrivals(exit_.get("scene", ""), exit_.get("arrival", ""))
            self.exit_arrival.currentIndexChanged.connect(lambda _i: self._set_value(
                "scene_exit", "arrival", self.exit_arrival.currentData() or "", "arrival"))
            form.addRow("Arriving at", self.exit_arrival)
            self.exit_activation = QComboBox()
            for key, label in self.ACTIVATIONS.items():
                self.exit_activation.addItem(label, key)
            self.exit_activation.setCurrentIndex(
                max(self.exit_activation.findData(exit_.get("activation", "enter")), 0))
            self.exit_activation.currentIndexChanged.connect(lambda _i: self._set_value(
                "scene_exit", "activation", self.exit_activation.currentData(), "activation"))
            form.addRow("Taken by", self.exit_activation)
            self.exit_size = Vec3Editor(0.1, 0.01)
            self.exit_size.set_value(exit_.get("size", [2.0, 2.5, 1.0]))
            for box in self.exit_size.boxes:
                box.valueChanged.connect(lambda _v: self._exit_size_edited())
            form.addRow("Exit size", self.exit_size)
            self.exit_fade = QDoubleSpinBox()
            self.exit_fade.setRange(0.0, 10.0)
            self.exit_fade.setSingleStep(0.1)
            self.exit_fade.setSuffix(" s")
            self.exit_fade.setValue(float(exit_.get("fade", 0.5)))
            self.exit_fade.setKeyboardTracking(False)
            self.exit_fade.valueChanged.connect(lambda v: self._set_value(
                "scene_exit", "fade", round(v, 2), "fade"))
            form.addRow("Fade", self.exit_fade)

    def _refresh_scene_exit(self, entity) -> None:
        """The arrival and scene exit rows as the entity has them (after an undo, say)."""
        if self.arrival_name is not None and "arrival" in entity.components:
            self.arrival_name.setText(entity.components["arrival"].get("name", ""))
        if self.exit_scene is None or "scene_exit" not in entity.components:
            return
        exit_ = entity.components["scene_exit"]
        for combo, value in ((self.exit_scene, exit_.get("scene", "")),
                             (self.exit_activation, exit_.get("activation", "enter"))):
            combo.blockSignals(True)
            combo.setCurrentIndex(max(combo.findData(value), 0))
            combo.blockSignals(False)
        self._fill_arrivals(exit_.get("scene", ""), exit_.get("arrival", ""))
        self.exit_size.set_value(exit_.get("size", [2.0, 2.5, 1.0]))
        self.exit_fade.blockSignals(True)
        self.exit_fade.setValue(float(exit_.get("fade", 0.5)))
        self.exit_fade.blockSignals(False)

    def _fill_arrivals(self, scene: str, current: str) -> None:
        """The arrival points in the exit's target scene, and its player start."""
        self.exit_arrival.blockSignals(True)
        self.exit_arrival.clear()
        self.exit_arrival.addItem("The scene's player start", "")
        if scene:
            for e in self.project.entities(scene):
                if "arrival" in e.components:
                    self.exit_arrival.addItem(e.components["arrival"].get("name") or e.name,
                                              e.id)
        if self.exit_arrival.findData(current) < 0:
            self.exit_arrival.addItem(f"{current} (missing)", current)
        self.exit_arrival.setCurrentIndex(self.exit_arrival.findData(current))
        self.exit_arrival.blockSignals(False)

    def _exit_scene_chosen(self) -> None:
        """Another target scene: its player start, until an arrival there is chosen."""
        entity = self._entity()
        scene = self.exit_scene.currentData() or ""
        exit_ = entity.components["scene_exit"]
        if exit_.get("scene") == scene:
            return
        entity.components["scene_exit"] = {**exit_, "scene": scene, "arrival": ""}
        self._push(entity, "scene exit's scene")
        self._fill_arrivals(scene, "")

    def _exit_size_edited(self) -> None:
        entity = self._entity()
        size = self.exit_size.value()
        if entity.components["scene_exit"].get("size") != size:
            entity.components["scene_exit"] = {**entity.components["scene_exit"], "size": size}
            self._push(entity, "exit size")

    def _trigger_edited(self) -> None:
        entity = self._entity()
        size = self.trigger.value()
        if entity.components["trigger"].get("size") != size:
            entity.components["trigger"] = {**entity.components["trigger"], "size": size}
            self._push(entity, "trigger size")

    def edit_material(self):
        """The entity's material in the Material editor (a window of its own)."""
        return MaterialDialog.show_for(self.project, EntityMaterial(self.project, self.entity_id),
                                       self.window())

    def edit_camera(self):
        """The camera's views in the Camera editor (a window of its own), previewed in the
        main window's viewport."""
        viewport = getattr(self.window(), "viewport", None)
        return CameraDialog.show_for(self.project, self.entity_id, viewport, self.window())

    def set_object(self, object_id: str | None) -> None:
        entity = self._entity()
        if not object_id or entity.components["object"].get("id") == object_id:
            return
        entity.components["object"] = {**entity.components["object"], "id": object_id}
        self._push(entity, "object")

    def _objects_changed(self, _object_id: str) -> None:
        """Objects renamed, added or removed: the list of them again."""
        if self.object is None:
            return
        self.object.blockSignals(True)
        self.object.clear()
        for obj in sorted(self.project.objects(), key=lambda o: o.name.lower()):
            self.object.addItem(obj.name, obj.id)
        self.object.blockSignals(False)
        self.refresh()

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

    def set_sky_environment(self, environment_id: str | None) -> None:
        entity = self._entity()
        sky = dict(entity.components["sky"])
        if (environment_id or "") == sky.get("environment", ""):
            return
        if environment_id:
            sky["environment"] = environment_id
        else:
            sky.pop("environment", None)
        entity.components["sky"] = sky
        self._push(entity, "environment")

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
