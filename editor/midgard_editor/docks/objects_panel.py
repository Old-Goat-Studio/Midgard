# SPDX-License-Identifier: Apache-2.0
"""The Objects panel: the project's object library and the Object editor (roadmap §5.5).

On the left, every object with its thumbnail, and the model files in objects/ that aren't
objects yet. On the right, the selected object: a preview (drag to turn it, wheel to zoom), its
import settings (scale, up axis), how it collides, its materials, and its levels of detail.
Place in Scene (or the Scene panel's Add ▸ Object) puts an instance of it in the scene. Every
change is undoable.
"""
import hashlib
import json
import logging
from pathlib import Path

from PySide6.QtCore import QPoint, QSize, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QIcon, QImage, QMouseEvent, QPixmap, QWheelEvent
from PySide6.QtWidgets import (
    QComboBox,
    QDoubleSpinBox,
    QFileDialog,
    QFormLayout,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMessageBox,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QSplitter,
    QStackedLayout,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from ..objects import (
    CACHE,
    COLLIDERS,
    ObjectDef,
    ObjectError,
    ObjectRenderer,
    copy_model,
    new_models,
    object_content,
    object_for_model,
    shared_renderer,
)
from ..project import Project
from ..undo import AddObjectCommand, DeleteObjectCommand, SetObjectCommand

log = logging.getLogger(__name__)

KIND_ROLE = Qt.ItemDataRole.UserRole + 1  # "object" or "file"
ID_ROLE = Qt.ItemDataRole.UserRole + 2  # the object id, or the file relative to objects/
THUMB = 96
PREVIEW_HEIGHT = 240
LOD_STEP = 20.0  # metres between a new level of detail and the one before


def _model_filter() -> str:
    return "glTF models (*.gltf *.glb)"


class PreviewLabel(QLabel):
    """The preview: drag to turn the object, the wheel to zoom."""
    changed = Signal()

    def __init__(self, height: int = PREVIEW_HEIGHT):
        super().__init__()
        self.yaw, self.pitch, self.zoom = 35.0, 22.0, 1.0
        self._last: QPoint | None = None
        self.setAlignment(Qt.AlignmentFlag.AlignCenter)
        # Its picture never widens the form: it's drawn at whatever width the label has.
        self.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Fixed)
        self.setMinimumWidth(160)
        self.setFixedHeight(height)
        self.setToolTip("Drag to turn the object, wheel to zoom")

        self._resized = QTimer(self)  # drawn again once the panel stops changing size
        self._resized.setSingleShot(True)
        self._resized.setInterval(120)
        self._resized.timeout.connect(self.changed)

    def resizeEvent(self, e) -> None:
        super().resizeEvent(e)
        if e.oldSize().width() != e.size().width():
            self._resized.start()

    def mousePressEvent(self, e: QMouseEvent) -> None:
        self._last = e.position().toPoint()

    def mouseReleaseEvent(self, _e: QMouseEvent) -> None:
        self._last = None

    def mouseMoveEvent(self, e: QMouseEvent) -> None:
        if self._last is None:
            return
        d = e.position().toPoint() - self._last
        self._last = e.position().toPoint()
        self.yaw -= d.x() * 0.6
        self.pitch = max(-85.0, min(85.0, self.pitch + d.y() * 0.6))
        self.changed.emit()

    def wheelEvent(self, e: QWheelEvent) -> None:
        self.zoom = max(0.3, min(4.0, self.zoom * (0.9 if e.angleDelta().y() > 0 else 1.1)))
        self.changed.emit()


class ObjectsPanel(QWidget):
    place_requested = Signal(str)  # an object id to put in the scene

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.project: Project | None = None
        self.renderer: ObjectRenderer | None = None  # made when first needed
        self._thumbs_pending: list[str] = []
        self._thumb_timer = QTimer(self)
        self._thumb_timer.setInterval(0)
        self._thumb_timer.timeout.connect(self._next_thumbnail)
        self._loading = False  # filling in the form: its signals aren't edits

        # The library.
        self.list = QListWidget()
        self.list.setIconSize(QSize(THUMB, THUMB))
        self.list.setSpacing(2)
        self.list.setWordWrap(True)
        self.list.currentItemChanged.connect(lambda *_a: self.show_selected())
        self.list.itemDoubleClicked.connect(lambda _i: self.place_selected())
        self.import_button = QPushButton("Import Model…")
        self.import_button.setToolTip("Copy a glTF model (.gltf or .glb) into objects/ and make "
                                      "an object of it")
        self.import_button.clicked.connect(self.choose_import)
        self.place_button = QPushButton("Place in Scene")
        self.place_button.setToolTip("Put an instance of the object in the scene (double-click "
                                     "does the same)")
        self.place_button.clicked.connect(self.place_selected)
        self.delete_button = QPushButton("Delete")
        self.delete_button.setToolTip("Take the object out of the library (its files stay in "
                                      "objects/)")
        self.delete_button.clicked.connect(self.delete_selected)
        buttons = QHBoxLayout()
        for b in (self.import_button, self.place_button, self.delete_button):
            buttons.addWidget(b)
        buttons.addStretch()
        left = QWidget()
        left_layout = QVBoxLayout(left)
        left_layout.setContentsMargins(4, 4, 4, 4)
        left_layout.addLayout(buttons)
        left_layout.addWidget(self.list)

        # The Object editor.
        self.preview = PreviewLabel()
        self.preview.changed.connect(self.render_preview)
        self.name = QLineEdit()
        self.name.editingFinished.connect(self._name_edited)
        self.source = QLabel()
        self.source.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.scale = QDoubleSpinBox()
        self.scale.setRange(0.0001, 10000.0)
        self.scale.setDecimals(4)
        self.scale.setSingleStep(0.1)
        self.scale.setToolTip("0.01 for a model made in centimetres, 0.0254 for inches")
        self.scale.editingFinished.connect(lambda: self.set_setting("scale", self.scale.value()))
        self.up_axis = QComboBox()
        self.up_axis.addItem("Y up (glTF's own)", False)
        self.up_axis.addItem("Z up", True)
        self.up_axis.currentIndexChanged.connect(
            lambda _i: self.set_setting("z_up", self.up_axis.currentData()))
        self.collider = QComboBox()
        for key, label in COLLIDERS.items():
            self.collider.addItem(label, key)
        self.collider.currentIndexChanged.connect(
            lambda _i: self.set_setting("collider", self.collider.currentData()))
        self.materials = QListWidget()
        self.materials.setMaximumHeight(110)
        self.materials.setToolTip("The model's materials, one part each: double-click one to "
                                  "change it in the Material editor")
        self.materials.itemDoubleClicked.connect(lambda _i: self.edit_material())
        self.edit_material_button = QPushButton("Edit Material…")
        self.edit_material_button.clicked.connect(lambda: self.edit_material())
        self.lods = QTableWidget(0, 2)
        self.lods.setHorizontalHeaderLabels(["Model file", "From (m)"])
        self.lods.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        self.lods.verticalHeader().setVisible(False)
        self.lods.setMaximumHeight(120)
        self.lods.setToolTip("Simpler versions of the model, drawn from a distance on. Each "
                             "needs the same materials as the model.")
        add_lod = QPushButton("Add…")
        add_lod.clicked.connect(self.choose_lod)
        self.remove_lod_button = QPushButton("Remove")
        self.remove_lod_button.clicked.connect(
            lambda: self.remove_lod(self.lods.currentRow()))
        lod_buttons = QHBoxLayout()
        lod_buttons.addWidget(add_lod)
        lod_buttons.addWidget(self.remove_lod_button)
        lod_buttons.addStretch()
        lod_box = QWidget()
        lod_layout = QVBoxLayout(lod_box)
        lod_layout.setContentsMargins(0, 0, 0, 0)
        lod_layout.addWidget(self.lods)
        lod_layout.addLayout(lod_buttons)
        self.warnings = QLabel()
        self.warnings.setWordWrap(True)
        self.warnings.setStyleSheet("color: #d08030;")
        self.make_object = QPushButton("Make Object")
        self.make_object.setToolTip("Make an object of this model file")
        self.make_object.clicked.connect(self.make_selected_object)

        editor = QWidget()
        form = self.form = QFormLayout(editor)
        form.addRow(self.preview)
        form.addRow("Name", self.name)
        form.addRow("Model file", self.source)
        form.addRow("Scale", self.scale)
        form.addRow("Up axis", self.up_axis)
        form.addRow("Collider", self.collider)
        form.addRow("Materials", self.materials)
        form.addRow("", self.edit_material_button)
        form.addRow("Levels of detail", lod_box)
        form.addRow(self.make_object)
        form.addRow(self.warnings)
        self.imported_rows = [self.scale, self.up_axis, self.collider, lod_box]
        # The form fits the panel's width (long names are cut short, not scrolled to).
        self.source.setWordWrap(True)
        for combo in (self.up_axis, self.collider):
            combo.setSizeAdjustPolicy(QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)
            combo.setMinimumContentsLength(8)
        for w in (self.name, self.source, self.scale, self.up_axis, self.collider,
                  self.materials, lod_box):
            w.setSizePolicy(QSizePolicy.Policy.Ignored, w.sizePolicy().verticalPolicy())
        scroll = QScrollArea()
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        scroll.setWidgetResizable(True)
        scroll.setWidget(editor)
        self.editor = scroll

        splitter = QSplitter(Qt.Orientation.Horizontal)
        splitter.addWidget(left)
        splitter.addWidget(scroll)
        splitter.setStretchFactor(1, 1)
        self.empty = QLabel("No project open.")
        self.empty.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.stack = QStackedLayout(self)
        self.stack.addWidget(self.empty)
        self.stack.addWidget(splitter)
        self.set_project(None)

    # ---- the library ----------------------------------------------------------------------
    def set_project(self, project: Project | None) -> None:
        if self.project is not None:
            self.project.objects_changed.disconnect(self.refresh)
        self.project = project
        self.stack.setCurrentIndex(0 if project is None else 1)
        if project is not None:
            project.objects_changed.connect(self.refresh)
        self.refresh()

    def selected(self) -> tuple[str, str] | None:
        """("object", id) or ("file", path in objects/) for the selected item."""
        item = self.list.currentItem()
        return (item.data(KIND_ROLE), item.data(ID_ROLE)) if item else None

    def selected_object(self) -> ObjectDef | None:
        sel = self.selected()
        if self.project is None or sel is None or sel[0] != "object":
            return None
        return self.project.object(sel[1])

    def refresh(self, *_args) -> None:
        current = self.selected()
        self.list.blockSignals(True)
        self.list.clear()
        if self.project is not None:
            objects = self.project.objects()
            for obj in sorted(objects, key=lambda o: o.name.lower()):
                item = QListWidgetItem(obj.name)
                item.setData(KIND_ROLE, "object")
                item.setData(ID_ROLE, obj.id)
                item.setToolTip(obj.source or "Made in the editor")
                icon = self._cached_thumbnail(obj)
                if icon is not None:
                    item.setIcon(icon)
                elif obj.id not in self._thumbs_pending:  # asked for once while it waits
                    self._thumbs_pending.append(obj.id)
                    self._thumb_timer.start()
                self.list.addItem(item)
            for relative in new_models(self.project.objects_dir, objects):
                item = QListWidgetItem(f"{relative} (not an object yet)")
                item.setData(KIND_ROLE, "file")
                item.setData(ID_ROLE, relative)
                font = item.font()
                font.setItalic(True)
                item.setFont(font)
                item.setForeground(QColor(140, 140, 140))
                self.list.addItem(item)
        restored = False
        for i in range(self.list.count()):
            item = self.list.item(i)
            if current and (item.data(KIND_ROLE), item.data(ID_ROLE)) == current:
                self.list.setCurrentItem(item)
                restored = True
        if not restored and self.list.count():
            self.list.setCurrentRow(0)
        self.list.blockSignals(False)
        self.show_selected()

    def select_object(self, object_id: str) -> None:
        for i in range(self.list.count()):
            item = self.list.item(i)
            if item.data(KIND_ROLE) == "object" and item.data(ID_ROLE) == object_id:
                self.list.setCurrentItem(item)

    # ---- thumbnails -----------------------------------------------------------------------
    def _renderer(self) -> ObjectRenderer:
        if self.renderer is None:
            self.renderer = shared_renderer()
        return self.renderer

    def _thumb_key(self, obj: ObjectDef) -> str:
        key = ((CACHE.key(self.project.objects_dir, obj),
                json.dumps(obj.settings.get("materials", {}), sort_keys=True))
               if obj.source else json.dumps(obj.parts, sort_keys=True))
        return hashlib.sha256(repr(key).encode()).hexdigest()

    def _thumb_file(self, obj: ObjectDef) -> Path:
        return self.project.root / "build" / "thumbnails" / f"{obj.id}.png"

    def _cached_thumbnail(self, obj: ObjectDef) -> QIcon | None:
        file = self._thumb_file(obj)
        try:
            if file.with_suffix(".key").read_text("utf-8") == self._thumb_key(obj):
                return QIcon(QPixmap(str(file)))
        except OSError:
            pass
        return None

    def thumbnail(self, obj: ObjectDef) -> QImage | None:
        """Draws (and keeps) an object's thumbnail. None without a renderer, or if the object
        can't be shown."""
        try:
            content = object_content(self.project, obj)
        except ObjectError:
            return None
        image = self._draw(obj, content, THUMB * 2, THUMB * 2)
        if image is not None:
            file = self._thumb_file(obj)
            try:
                file.parent.mkdir(parents=True, exist_ok=True)
                image.save(str(file))
                file.with_suffix(".key").write_text(self._thumb_key(obj), "utf-8")
            except OSError as e:
                log.warning("Could not keep the thumbnail of %s: %s", obj.name, e)
        return image

    def _next_thumbnail(self) -> None:
        """One thumbnail per pass of the event loop, so opening a big library stays smooth."""
        if not self._thumbs_pending or self.project is None:
            self._thumb_timer.stop()
            return
        object_id = self._thumbs_pending.pop(0)
        obj = self.project.object(object_id)
        image = self.thumbnail(obj) if obj is not None else None
        if image is None:
            return
        for i in range(self.list.count()):
            item = self.list.item(i)
            if item.data(KIND_ROLE) == "object" and item.data(ID_ROLE) == object_id:
                item.setIcon(QIcon(QPixmap.fromImage(image)))

    def _draw(self, obj: ObjectDef, content, width: int, height: int,
              yaw: float = 35.0, pitch: float = 22.0, zoom: float = 1.0) -> QImage | None:
        renderer = self._renderer()
        try:
            for content_id, (_path, data) in content.files.items():
                renderer.send(content_id, data)
        except RuntimeError as e:
            log.warning("Object %s: %s", obj.name, e)
            return None
        return renderer.render(obj.id, content.parts, content.bounds, width, height,
                               yaw, pitch, zoom)

    # ---- the Object editor ----------------------------------------------------------------
    def show_selected(self) -> None:
        sel = self.selected()
        obj = self.selected_object()
        is_file = sel is not None and sel[0] == "file"
        self.place_button.setEnabled(sel is not None)
        self.delete_button.setEnabled(obj is not None)
        self.editor.setEnabled(sel is not None)
        self.edit_material_button.setEnabled(False)
        self.make_object.setVisible(is_file)
        self._loading = True
        try:
            self.materials.clear()
            self.lods.setRowCount(0)
            self.warnings.clear()
            if obj is None:
                self.name.setText("")
                self.source.setText(sel[1] if is_file else "")
                for w in self.imported_rows:
                    w.setEnabled(False)
                self.preview.clear()
                self.preview.setText("A model file in objects/: make an object of it to place "
                                     "it" if is_file else "")
                return
            self.name.setText(obj.name)
            self.source.setText(obj.source or "Made in the editor (from its parts)")
            for w in self.imported_rows:
                w.setEnabled(bool(obj.source))
            self.scale.setValue(float(obj.settings.get("scale", 1.0)))
            self.up_axis.setCurrentIndex(1 if obj.settings.get("z_up") else 0)
            self.collider.setCurrentIndex(max(self.collider.findData(
                obj.settings.get("collider", "box")), 0))
            for i, lod in enumerate(obj.settings.get("lods", [])):
                self.lods.insertRow(i)
                self.lods.setItem(i, 0, QTableWidgetItem(lod.get("source", "")))
                self.lods.item(i, 0).setFlags(Qt.ItemFlag.ItemIsEnabled |
                                              Qt.ItemFlag.ItemIsSelectable)
                box = QDoubleSpinBox()
                box.setRange(0.0, 100000.0)
                box.setSuffix(" m")
                box.setValue(float(lod.get("distance", 0.0)))
                box.editingFinished.connect(
                    lambda b=box, row=i: self.set_lod_distance(row, b.value()))
                self.lods.setCellWidget(i, 1, box)
            self.remove_lod_button.setEnabled(bool(obj.settings.get("lods")))
        finally:
            self._loading = False
        try:
            content = object_content(self.project, obj)
        except ObjectError as e:
            self.warnings.setText(f"Can't import the model: {e}")
            self.preview.clear()
            self.preview.setText("No preview")
            return
        if obj.source:
            for index, m in enumerate(content.materials):
                r, g, b, _a = (min(max(c, 0.0), 1.0) ** (1 / 2.2) for c in m["color"])
                swatch = QPixmap(16, 16)
                swatch.fill(QColor.fromRgbF(r, g, b))
                label = m["name"] or "(unnamed)"
                if m["textures"]:
                    label += "  —  textured: " + ", ".join(m["textures"]).replace("_", " ")
                if str(index) in obj.settings.get("materials", {}):
                    label += "  —  changed"
                item = QListWidgetItem(QIcon(swatch), label)
                item.setData(Qt.ItemDataRole.UserRole, index)
                self.materials.addItem(item)
            if self.materials.count():
                self.materials.setCurrentRow(0)
        else:
            for part in obj.parts:
                self.materials.addItem(part.get("name", "part"))
        self.edit_material_button.setEnabled(bool(obj.source and content.materials))
        self.warnings.setText("\n".join(content.warnings))
        self.render_preview()

    def edit_material(self, index: int | None = None):
        """Opens the Material editor on one of the selected object's materials (the current
        one in the list by default). Returns its window."""
        from .material_editor import MaterialDialog, ObjectMaterial  # (it uses this module)
        obj = self.selected_object()
        if index is None:
            item = self.materials.currentItem()
            index = item.data(Qt.ItemDataRole.UserRole) if item else None
        if obj is None or not obj.source or index is None:
            return None
        return MaterialDialog.show_for(self.project, ObjectMaterial(self.project, obj.id, index),
                                       self.window())

    def render_preview(self) -> None:
        obj = self.selected_object()
        if obj is None or self.project is None:
            return
        try:
            content = object_content(self.project, obj)
        except ObjectError:
            return
        width = min(max(self.preview.width(), 160), 1024)
        height = PREVIEW_HEIGHT
        image = self._draw(obj, content, width, height, self.preview.yaw, self.preview.pitch,
                           self.preview.zoom)
        if image is None:
            reason = self._renderer().error or "nothing to draw"
            self.preview.clear()
            self.preview.setText(f"No preview: {reason}")
        else:
            self.preview.setPixmap(QPixmap.fromImage(image))

    # ---- edits (each one undoable) --------------------------------------------------------
    def _push(self, new: ObjectDef, label: str) -> None:
        self.project.undo_stack.push(SetObjectCommand(self.project, new, label))

    def _name_edited(self) -> None:
        obj = self.selected_object()
        name = self.name.text().strip()
        if self._loading or obj is None or not name or name == obj.name:
            return
        self._push(ObjectDef(obj.id, name, obj.source, obj.settings, obj.parts), "name")

    def set_setting(self, key: str, value) -> None:
        obj = self.selected_object()
        if self._loading or obj is None or not obj.source:
            return
        default = {"scale": 1.0, "z_up": False, "collider": "box"}.get(key)
        if obj.settings.get(key, default) == value:
            return
        settings = dict(obj.settings)
        if value == default:
            settings.pop(key, None)
        else:
            settings[key] = value
        label = {"scale": "scale", "z_up": "up axis", "collider": "collider"}.get(key, key)
        self._push(ObjectDef(obj.id, obj.name, obj.source, settings, obj.parts), label)

    def _set_lods(self, obj: ObjectDef, lods: list[dict], label: str) -> None:
        settings = dict(obj.settings)
        if lods:
            settings["lods"] = lods
        else:
            settings.pop("lods", None)
        self._push(ObjectDef(obj.id, obj.name, obj.source, settings, obj.parts), label)

    def add_lod(self, file: Path) -> None:
        obj = self.selected_object()
        if obj is None or not obj.source:
            return
        relative = copy_model(Path(file), self.project.objects_dir)
        lods = [dict(lod) for lod in obj.settings.get("lods", [])]
        distance = max([lod.get("distance", 0.0) for lod in lods], default=0.0) + LOD_STEP
        lods.append({"source": relative, "distance": distance})
        self._set_lods(obj, lods, "levels of detail")

    def set_lod_distance(self, row: int, distance: float) -> None:
        obj = self.selected_object()
        if self._loading or obj is None:
            return
        lods = [dict(lod) for lod in obj.settings.get("lods", [])]
        if 0 <= row < len(lods) and lods[row].get("distance") != distance:
            lods[row]["distance"] = distance
            self._set_lods(obj, lods, "level of detail distance")

    def remove_lod(self, row: int) -> None:
        obj = self.selected_object()
        lods = [dict(lod) for lod in (obj.settings.get("lods", []) if obj else [])]
        if 0 <= row < len(lods):
            del lods[row]
            self._set_lods(obj, lods, "levels of detail")

    def choose_lod(self) -> None:
        file, _ = QFileDialog.getOpenFileName(self, "Add a Level of Detail",
                                              str(self.project.objects_dir), _model_filter())
        if file:
            self.add_lod(Path(file))

    def import_file(self, file: Path) -> str:
        """Copies a model into objects/ (if it isn't there already) and makes an object of it.
        Returns the object's id."""
        relative = copy_model(Path(file), self.project.objects_dir)
        objects = self.project.objects()
        existing = next((o for o in objects if o.source == relative), None)
        if existing is not None:
            self.select_object(existing.id)
            return existing.id
        obj = object_for_model(objects, relative)
        self.project.undo_stack.push(AddObjectCommand(self.project, obj))
        self.select_object(obj.id)
        return obj.id

    def choose_import(self) -> None:
        if self.project is None:
            return
        file, _ = QFileDialog.getOpenFileName(self, "Import a Model", str(Path.home()),
                                              _model_filter())
        if file:
            self.import_file(Path(file))

    def make_selected_object(self) -> str | None:
        sel = self.selected()
        if self.project is None or sel is None or sel[0] != "file":
            return None
        return self.import_file(self.project.objects_dir / sel[1])

    def place_selected(self) -> None:
        sel = self.selected()
        if sel is None:
            return
        object_id = sel[1] if sel[0] == "object" else self.make_selected_object()
        if object_id:
            self.place_requested.emit(object_id)

    def delete_selected(self, confirm: bool = True) -> None:
        obj = self.selected_object()
        if obj is None:
            return
        used = [e.name for e in self.project.entities()
                if e.components.get("object", {}).get("id") == obj.id]
        if used and confirm:
            answer = QMessageBox.question(
                self, "Delete Object",
                f"{len(used)} {'entity places' if len(used) == 1 else 'entities place'} "
                f"{obj.name} ({', '.join(used[:3])}{'…' if len(used) > 3 else ''}). They will "
                "show nothing until the object is back (Undo brings it back). Delete it?")
            if answer != QMessageBox.StandardButton.Yes:
                return
        self.project.undo_stack.push(DeleteObjectCommand(self.project, obj))
