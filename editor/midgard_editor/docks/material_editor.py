# SPDX-License-Identifier: Apache-2.0
"""The Material editor (roadmap §5.5): glTF's metal/roughness material, its numbers and its five
texture slots, with a preview drawn by the engine as you change it.

It edits a material in one of two places: an entity's material component (Inspector ▸ Edit…), or
one of an imported object's materials (Objects panel ▸ Edit Material…), kept in the object's
settings in place of the model's own. Every change is undoable; a slider dragged from one value
to another is one step in the History. The viewport, and the game while it runs, show changes as
they are made.
"""
import copy
import logging
from pathlib import Path
from typing import Any

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QColor, QGuiApplication, QPixmap, QUndoCommand
from PySide6.QtWidgets import (
    QColorDialog,
    QComboBox,
    QDialog,
    QDoubleSpinBox,
    QFileDialog,
    QFormLayout,
    QFrame,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QSlider,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

from .. import meshes, objects, textures
from ..objects import ObjectError
from ..project import Project
from .objects_panel import PreviewLabel

log = logging.getLogger(__name__)

DEFAULTS = {"color": [1.0, 1.0, 1.0, 1.0], "metallic": 0.0, "roughness": 0.8,
            "emissive": [0.0, 0.0, 0.0], "emissive_strength": 1.0, "normal_scale": 1.0,
            "occlusion_strength": 1.0}
PREVIEW_ID = "__material_preview__"
SHAPES = {"prim_sphere": "Sphere", "prim_cube": "Cube", "prim_cylinder": "Cylinder"}


# ---- what a Material editor edits ---------------------------------------------------------

class EntityMaterial:
    """An entity's material component."""

    def __init__(self, project: Project, entity_id: str):
        self.project, self.entity_id = project, entity_id

    @property
    def key(self) -> tuple:
        return ("entity", self.entity_id)

    @property
    def title(self) -> str:
        entity = self.project.entity(self.entity_id)
        return entity.name if entity else self.entity_id

    def get(self) -> dict[str, Any]:
        entity = self.project.entity(self.entity_id)
        return copy.deepcopy(entity.components.get("material", {})) if entity else {}

    def set(self, material: dict[str, Any]) -> None:
        entity = self.project.entity(self.entity_id)
        if entity is not None:
            entity.components["material"] = copy.deepcopy(material)
            self.project.put_entity(entity)

    def original(self) -> dict[str, Any] | None:
        return None  # nothing to go back to

    def connect(self, slot) -> None:
        self.project.entities_changed.connect(slot)


class ObjectMaterial:
    """One of an imported object's materials (by its index in the model)."""

    def __init__(self, project: Project, object_id: str, index: int):
        self.project, self.object_id, self.index = project, object_id, index

    @property
    def key(self) -> tuple:
        return ("object", self.object_id, self.index)

    def _model(self):
        obj = self.project.object(self.object_id)
        if obj is None or not obj.source:
            raise ObjectError("the object is gone")
        return obj, objects.CACHE.cooked(self.project.objects_dir, obj)

    @property
    def title(self) -> str:
        try:
            obj, cooked = self._model()
        except ObjectError:
            return self.object_id
        name = cooked.materials[self.index]["name"] or f"material {self.index + 1}"
        return f"{obj.name}: {name}"

    def get(self) -> dict[str, Any]:
        obj, cooked = self._model()
        return objects.object_material(obj, cooked, self.index)

    def original(self) -> dict[str, Any] | None:
        obj, cooked = self._model()
        return objects.model_material(obj, cooked, self.index)

    def set(self, material: dict[str, Any]) -> None:
        obj = self.project.object(self.object_id)
        if obj is None:
            return
        settings = copy.deepcopy(obj.settings)
        overrides = settings.setdefault("materials", {})
        if material == self.original():  # the model's own again: nothing to keep
            overrides.pop(str(self.index), None)
        else:
            overrides[str(self.index)] = copy.deepcopy(material)
        if not overrides:
            settings.pop("materials")
        self.project.put_object(objects.ObjectDef(obj.id, obj.name, obj.source, settings,
                                                  obj.parts))

    def connect(self, slot) -> None:
        self.project.objects_changed.connect(slot)


class MaterialCommand(QUndoCommand):
    """One Material editor change. Changes to the same setting of the same material in a row
    (a slider being dragged) merge into one step."""
    ID = 5504

    def __init__(self, target, old: dict, new: dict, field: str, label: str):
        super().__init__(f"Change {target.title} {label}")
        self.target, self.old, self.new, self.field = target, old, new, field

    def id(self) -> int:
        return self.ID

    def mergeWith(self, other: QUndoCommand) -> bool:
        if (not isinstance(other, MaterialCommand) or other.target.key != self.target.key
                or other.field != self.field):
            return False
        self.new = other.new
        return True

    def redo(self) -> None:
        self.target.set(self.new)

    def undo(self) -> None:
        self.target.set(self.old)


# ---- the editor ---------------------------------------------------------------------------

def _rgb(values: list[float]) -> QColor:
    return QColor.fromRgbF(*(min(max(v, 0.0), 1.0) for v in values[:3]))


def _style(button: QPushButton, values: list[float]) -> None:
    c = _rgb(values)
    text = "#000" if c.lightnessF() > 0.5 else "#fff"
    button.setStyleSheet(f"background-color: {c.name()}; color: {text};")
    button.setText(c.name())


class _Amount(QWidget):
    """A slider and a number box for a value in a range."""
    changed = Signal(float)

    def __init__(self, low: float, high: float, decimals: int = 2):
        super().__init__()
        self.low, self.high = low, high
        self.slider = QSlider(Qt.Orientation.Horizontal)
        self.slider.setRange(0, 1000)
        self.box = QDoubleSpinBox()
        self.box.setRange(low, high)
        self.box.setDecimals(decimals)
        self.box.setSingleStep(10 ** -decimals * 5 if decimals else 1)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.slider, 1)
        layout.addWidget(self.box)
        self.slider.valueChanged.connect(
            lambda v: self._set(low + (high - low) * v / 1000, from_slider=True))
        self.box.valueChanged.connect(lambda v: self._set(v, from_slider=False))

    def _set(self, value: float, from_slider: bool) -> None:
        self.set_value(value)
        self.changed.emit(round(value, 4))

    def set_value(self, value: float) -> None:
        for w in (self.slider, self.box):
            w.blockSignals(True)
        self.box.setValue(value)
        self.slider.setValue(round(1000 * (value - self.low) / (self.high - self.low)))
        for w in (self.slider, self.box):
            w.blockSignals(False)

    def value(self) -> float:
        return self.box.value()


class MaterialEditor(QWidget):
    def __init__(self, project: Project, target, parent: QWidget | None = None):
        super().__init__(parent)
        self.project, self.target = project, target
        self._last_material: dict[str, Any] | None = None
        form = QFormLayout(self)

        self.preview = PreviewLabel(180)
        self.preview.changed.connect(self.render_preview)
        self.shape = QComboBox()
        for asset, label in SHAPES.items():
            self.shape.addItem(label, asset)
        self.shape.currentIndexChanged.connect(lambda _i: self.render_preview())
        form.addRow(self.preview)
        form.addRow("Preview on", self.shape)

        self.color = QPushButton()
        self.color.clicked.connect(lambda: self._pick("color", "Base colour"))
        self.alpha = _Amount(0.0, 1.0)
        self.alpha.changed.connect(lambda v: self.set_alpha(v))
        self.metallic = _Amount(0.0, 1.0)
        self.metallic.setToolTip("0: not metal (plastic, wood, stone); 1: bare metal")
        self.metallic.changed.connect(lambda v: self.set_value("metallic", v, "metal"))
        self.roughness = _Amount(0.0, 1.0)
        self.roughness.setToolTip("0: mirror-smooth; 1: completely matt")
        self.roughness.changed.connect(lambda v: self.set_value("roughness", v, "roughness"))
        self.emissive = QPushButton()
        self.emissive.clicked.connect(lambda: self._pick("emissive", "Emission colour"))
        self.emissive_strength = _Amount(0.0, 50.0)
        self.emissive_strength.changed.connect(
            lambda v: self.set_value("emissive_strength", v, "emission strength"))
        self.normal_scale = _Amount(0.0, 4.0)
        self.normal_scale.setToolTip("How strongly the normal texture bends the surface")
        self.normal_scale.changed.connect(
            lambda v: self.set_value("normal_scale", v, "normal strength"))
        self.occlusion_strength = _Amount(0.0, 1.0)
        self.occlusion_strength.setToolTip("How much the occlusion texture darkens crevices")
        self.occlusion_strength.changed.connect(
            lambda v: self.set_value("occlusion_strength", v, "occlusion strength"))
        form.addRow("Base colour", self.color)
        form.addRow("Opacity", self.alpha)
        form.addRow("Metal", self.metallic)
        form.addRow("Roughness", self.roughness)
        form.addRow("Emission colour", self.emissive)
        form.addRow("Emission strength", self.emissive_strength)
        form.addRow("Normal strength", self.normal_scale)
        form.addRow("Occlusion strength", self.occlusion_strength)

        self.slot_labels: dict[str, QLabel] = {}
        for slot, label in textures.SLOTS.items():
            name = QLabel()
            name.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
            name.setMinimumWidth(80)
            choose = QPushButton("Choose…")
            choose.clicked.connect(lambda _c=False, s=slot: self.choose_texture(s))
            clear = QToolButton()
            clear.setText("✕")
            clear.setToolTip("No texture in this slot")
            clear.clicked.connect(lambda _c=False, s=slot: self.set_texture(s, None))
            row = QWidget()
            layout = QHBoxLayout(row)
            layout.setContentsMargins(0, 0, 0, 0)
            layout.addWidget(name, 1)
            layout.addWidget(choose)
            layout.addWidget(clear)
            form.addRow(f"{label} texture", row)
            self.slot_labels[slot] = name

        self.reset = QPushButton("Back to the Model's Material")
        self.reset.setToolTip("Undo every change to this material (one step you can undo)")
        self.reset.clicked.connect(self.reset_to_original)
        self.reset.setVisible(isinstance(target, ObjectMaterial))
        form.addRow(self.reset)
        self.problem = QLabel()
        self.problem.setWordWrap(True)
        self.problem.setStyleSheet("color: #d08030;")
        form.addRow(self.problem)
        # (Qt lets go of the connection when the editor closes, as with the Inspector's forms.)
        target.connect(self._target_changed)
        self.refresh()

    # ---- showing the material -------------------------------------------------------------
    def material(self) -> dict[str, Any]:
        try:
            return self.target.get()
        except ObjectError:
            return {}

    def _target_changed(self, *_args) -> None:
        if self.material() != self._last_material:
            self.refresh()

    def refresh(self) -> None:
        m = self.material()
        self._last_material = copy.deepcopy(m)
        value = {**DEFAULTS, **m}
        _style(self.color, value["color"])
        self.alpha.set_value(value["color"][3] if len(value["color"]) > 3 else 1.0)
        _style(self.emissive, value["emissive"])
        for key in ("metallic", "roughness", "emissive_strength", "normal_scale",
                    "occlusion_strength"):
            getattr(self, key).set_value(float(value[key]))
        slots = m.get("textures", {})
        for slot, label in self.slot_labels.items():
            label.setText(textures.describe(slots[slot]) if slots.get(slot) else "None")
            label.setToolTip(slots.get(slot, ""))
        if isinstance(self.target, ObjectMaterial):
            try:
                self.reset.setEnabled(m != self.target.original())
            except ObjectError:
                self.reset.setEnabled(False)
        self.render_preview()

    def render_preview(self) -> None:
        m = self.material()
        renderer = objects.shared_renderer()
        asset = self.shape.currentData()
        parts = [{"name": "preview", "components": {"mesh": {"asset": asset}, "material": m}}]
        problems = []
        try:
            renderer.send(asset, meshes.to_glb(meshes.primitive(asset)))
            for texture in textures.material_textures(m):
                data = self._texture_data(texture)
                if data is None:
                    problems.append(f"The texture {textures.describe(texture)} can't be shown.")
                else:
                    renderer.send(texture, data)
        except RuntimeError as e:
            problems.append(str(e))
        self.problem.setText("\n".join(problems))
        width = min(max(self.preview.width(), 160), 1024)
        image = renderer.render(PREVIEW_ID, parts, [-0.5, -0.5, -0.5, 0.5, 0.5, 0.5], width,
                                self.preview.height(), self.preview.yaw, self.preview.pitch,
                                self.preview.zoom)
        if image is None:
            self.preview.clear()
            self.preview.setText(f"No preview: {renderer.error or 'nothing to draw'}")
        else:
            self.preview.setPixmap(QPixmap.fromImage(image))

    def _texture_data(self, texture: str) -> bytes | None:
        if textures.parse(texture) is not None:
            return textures.texture_data(self.project.root, texture)
        if isinstance(self.target, ObjectMaterial):  # the model's own textures
            try:
                _obj, cooked = self.target._model()
            except ObjectError:
                return None
            item = cooked.files.get(texture)
            return item[1] if item else None
        return None

    # ---- changes (each one undoable) ------------------------------------------------------
    def _push(self, new: dict[str, Any], field: str, label: str) -> None:
        old = self.material()
        if new == old:
            return
        self.project.undo_stack.push(MaterialCommand(self.target, old, new, field, label))

    def set_value(self, key: str, value: float, label: str) -> None:
        self._push({**self.material(), key: value}, key, label)

    def set_color(self, key: str, rgb: list[float]) -> None:
        m = self.material()
        if key == "color":
            alpha = m.get("color", DEFAULTS["color"])[3:4] or [1.0]
            rgb = list(rgb[:3]) + alpha
        self._push({**m, key: [round(c, 4) for c in rgb]}, key,
                   "colour" if key == "color" else "emission colour")

    def set_alpha(self, alpha: float) -> None:
        m = self.material()
        color = list(m.get("color", DEFAULTS["color"]))[:3]
        self._push({**m, "color": color + [alpha]}, "alpha", "opacity")

    def _pick(self, key: str, title: str) -> None:
        current = {**DEFAULTS, **self.material()}[key]
        chosen = QColorDialog.getColor(_rgb(current), self, title)
        if chosen.isValid():
            self.set_color(key, [chosen.redF(), chosen.greenF(), chosen.blueF()])

    def set_texture(self, slot: str, texture: str | None) -> None:
        m = self.material()
        slots = dict(m.get("textures", {}))
        if texture:
            slots[slot] = texture
        else:
            slots.pop(slot, None)
        new = {k: v for k, v in m.items() if k != "textures"}
        if slots:
            new["textures"] = slots
        self._push(new, "texture " + slot, textures.SLOTS[slot].lower() + " texture")

    def use_image(self, slot: str, file: Path) -> None:
        """An image file in a slot (copied into textures/ if it is outside the project)."""
        relative = textures.copy_texture(Path(file), self.project.root)
        self.set_texture(slot, textures.ref(relative, slot))

    def choose_texture(self, slot: str) -> None:
        start = self.project.textures_dir if self.project.textures_dir.is_dir() else \
            self.project.root
        file, _ = QFileDialog.getOpenFileName(
            self, f"{textures.SLOTS[slot]} Texture", str(start),
            "Images (" + " ".join("*" + s for s in textures.IMAGE_SUFFIXES) + ")")
        if file:
            self.use_image(slot, Path(file))

    def reset_to_original(self) -> None:
        original = self.target.original()
        if original is not None:
            self._push(original, "reset", "material (back to the model's)")


class MaterialDialog(QDialog):
    """The Material editor in a window of its own, kept open beside the editor (one per
    material: asking for the same material again brings its window forward)."""
    _open: dict[tuple, "MaterialDialog"] = {}

    def __init__(self, project: Project, target, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWindowTitle(f"Material — {target.title}")
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose)
        self.editor = MaterialEditor(project, target)
        # It scrolls when the screen is shorter than it is, and opens no taller than the screen.
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        scroll.setFrameShape(QFrame.Shape.NoFrame)
        scroll.setWidget(self.editor)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(scroll)
        screen = parent.screen() if parent is not None else None
        screen = screen or QGuiApplication.primaryScreen()
        available = screen.availableGeometry()
        wanted = self.editor.sizeHint().height() + 8
        self.resize(440, max(320, min(wanted, int(available.height() * 0.85))))
        self.key = target.key
        self.destroyed.connect(lambda *_a, k=target.key: MaterialDialog._open.pop(k, None))

    @classmethod
    def close_all(cls) -> None:
        """Another project: the open Material editors close."""
        for dialog in list(cls._open.values()):
            dialog.close()
        cls._open.clear()

    @classmethod
    def show_for(cls, project: Project, target, parent: QWidget | None = None) -> "MaterialDialog":
        dialog = cls._open.get(target.key)
        if dialog is None:
            dialog = cls(project, target, parent)
            cls._open[target.key] = dialog
        dialog.show()
        dialog.raise_()
        dialog.activateWindow()
        return dialog
