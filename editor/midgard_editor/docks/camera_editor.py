# SPDX-License-Identifier: Apache-2.0
"""The Camera editor (roadmap §8, Cameras; milestone 5.6): the settings of the player's camera
for each of its views, first person, third person and top-down, with the view the game starts
in and the time it takes to blend between them (the camera component in schema/components.md).

"Preview in viewport" shows the view whose tab is open from where the game would put the camera,
worked out by the engine's own camera rig, and follows each change as it is made. Every change is
undoable; a slider dragged from one value to another is one step in the History. The game, while
it runs, takes changes as they are made and keeps the player's current view.
"""
import copy
from dataclasses import dataclass
from typing import Any

from PySide6.QtCore import Qt
from PySide6.QtGui import QUndoCommand
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDialog,
    QFormLayout,
    QLabel,
    QPushButton,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from ..project import Project
from .material_editor import _Amount

VIEWS = {"first": "First person", "third": "Third person", "top": "Top-down"}
BLEND = 0.35  # seconds (CameraRigSettings in midgard/scene.h)

# Each view's defaults, as the engine has them (CameraRigSettings in midgard/scene.h). Third
# person's field of view is the camera's own "fov", and its distance, when not set, is where the
# camera was placed.
DEFAULTS: dict[str, dict[str, float]] = {
    "first": {"fov": 70.0, "distance": 0.0, "min_distance": 0.0, "max_distance": 0.0,
              "height": 0.35, "shoulder": 0.0, "min_pitch": -80.0, "max_pitch": 80.0,
              "radius": 0.1, "smoothing": 0.0},
    "third": {"fov": 55.0, "distance": 6.0, "min_distance": 1.5, "max_distance": 30.0,
              "height": 0.5, "shoulder": 0.0, "min_pitch": -10.0, "max_pitch": 80.0,
              "radius": 0.25, "smoothing": 0.0},
    "top": {"fov": 45.0, "distance": 18.0, "min_distance": 8.0, "max_distance": 40.0,
            "height": 0.0, "shoulder": 0.0, "min_pitch": 60.0, "max_pitch": 60.0,
            "radius": 0.25, "smoothing": 0.0},
}


@dataclass(frozen=True)
class Setting:
    key: str
    label: str
    low: float
    high: float
    decimals: int
    suffix: str
    tip: str
    views: tuple[str, ...]


SETTINGS = (
    Setting("fov", "Field of view", 10, 150, 0, "°", "How wide the view is, top to bottom",
            ("first", "third", "top")),
    Setting("distance", "Distance", 0, 100, 1, " m",
            "How far from the player the camera starts (third person: 0 starts where the camera "
            "is placed in the scene)", ("third", "top")),
    Setting("min_distance", "Closest zoom", 0, 100, 1, " m",
            "How close the wheel can bring the camera (third person: zooming in further goes "
            "to first person)", ("third", "top")),
    Setting("max_distance", "Farthest zoom", 0, 200, 1, " m", "How far the wheel can take it",
            ("third", "top")),
    Setting("height", "Height", -2, 5, 2, " m",
            "Above the player's centre: the eyes in first person, what the camera looks at "
            "otherwise", ("first", "third", "top")),
    Setting("shoulder", "Shoulder offset", -3, 3, 2, " m",
            "Over the shoulder: metres to the right (negative: to the left)", ("third",)),
    Setting("min_pitch", "Lowest angle", -89, 89, 0, "°",
            "How far down it can look (third person: how low the camera can go)",
            ("first", "third")),
    Setting("max_pitch", "Highest angle", -89, 89, 0, "°",
            "How far up it can look (third person: how high the camera can go)",
            ("first", "third")),
    Setting("radius", "Collision radius", 0, 5, 2, " m",
            "The camera keeps this far from walls and anything solid between it and the player",
            ("third", "top")),
    Setting("smoothing", "Smoothing", 0, 5, 2, " s",
            "How far behind the player the camera lags (0: none)", ("first", "third", "top")),
)
TOP_ANGLE = Setting("min_pitch", "Angle", 10, 89, 0, "°",
                    "How steeply it looks down on the player", ("top",))


def view_settings(camera: dict[str, Any], view: str) -> dict[str, float]:
    """A view's settings as the game uses them: the camera component's over the defaults."""
    own = camera.get("views", {}).get(view, {})
    values = {**DEFAULTS[view], **own}
    if view == "third":
        values["fov"] = own.get("fov", camera.get("fov", 55.0))
        values["distance"] = own.get("distance", 0.0)  # 0: where the camera was placed
    return values


class CameraCommand(QUndoCommand):
    """One Camera editor change. Changes to the same setting of the same camera in a row (a
    slider being dragged) merge into one step."""
    ID = 5601

    def __init__(self, project: Project, entity_id: str, old: dict, new: dict, field: str,
                 label: str):
        entity = project.entity(entity_id)
        super().__init__(f"Change {entity.name if entity else entity_id} {label}")
        self.project, self.entity_id = project, entity_id
        self.old, self.new, self.field = old, new, field

    def id(self) -> int:
        return self.ID

    def mergeWith(self, other: QUndoCommand) -> bool:
        if (not isinstance(other, CameraCommand) or other.entity_id != self.entity_id
                or other.field != self.field):
            return False
        self.new = other.new
        return True

    def _set(self, camera: dict) -> None:
        entity = self.project.entity(self.entity_id)
        if entity is not None:
            entity.components["camera"] = copy.deepcopy(camera)
            self.project.put_entity(entity)

    def redo(self) -> None:
        self._set(self.new)

    def undo(self) -> None:
        self._set(self.old)


class CameraEditor(QWidget):
    """The settings of one camera entity. `viewport` (the editor's ViewportPanel, if there is
    one) shows the preview."""

    def __init__(self, project: Project, entity_id: str, viewport=None,
                 parent: QWidget | None = None):
        super().__init__(parent)
        self.project, self.entity_id, self.viewport = project, entity_id, viewport
        layout = QVBoxLayout(self)
        common = QFormLayout()
        self.start = QComboBox()
        for view, label in VIEWS.items():
            self.start.addItem(label, view)
        self.start.setToolTip("The view the game starts in (a player's own last view wins in "
                              "an exported game)")
        self.start.currentIndexChanged.connect(lambda _i: self.set_start(self.start.currentData()))
        self.blend = _Amount(0.0, 3.0)
        self.blend.box.setSuffix(" s")
        self.blend.setToolTip("How long changing view takes (0: at once)")
        self.blend.changed.connect(lambda v: self.set_common("blend", v, "blend time"))
        common.addRow("Starts in", self.start)
        common.addRow("Blend time", self.blend)
        layout.addLayout(common)

        self.tabs = QTabWidget()
        self.amounts: dict[tuple[str, str], _Amount] = {}
        for view, label in VIEWS.items():
            page = QWidget()
            form = QFormLayout(page)
            settings = [TOP_ANGLE if view == "top" and s.key == "min_pitch" else s
                        for s in SETTINGS if view in s.views or (view == "top" and
                                                                 s.key == "min_pitch")]
            for s in settings:
                amount = _Amount(s.low, s.high, s.decimals)
                amount.box.setSuffix(s.suffix)
                amount.setToolTip(s.tip)
                if view == "third" and s.key == "distance":
                    amount.box.setSpecialValueText("Where placed")
                amount.changed.connect(lambda v, vw=view, st=s: self.set_view_value(vw, st, v))
                form.addRow(s.label, amount)
                self.amounts[(view, s.key)] = amount
            reset = QPushButton("Back to the Defaults")
            reset.setToolTip(f"Every {label.lower()} setting back to Midgard's own "
                             "(one step you can undo)")
            reset.clicked.connect(lambda _c=False, vw=view: self.reset_view(vw))
            form.addRow(reset)
            self.tabs.addTab(page, label)
        self.tabs.currentChanged.connect(lambda _i: self.update_preview())
        layout.addWidget(self.tabs)

        self.preview = QCheckBox("Preview in viewport")
        self.preview.setToolTip("Show this tab's view in the viewport, from where the game would "
                                "put the camera (walls don't stop it there). Moving the "
                                "viewport's camera ends the preview.")
        self.preview.setEnabled(viewport is not None and viewport.view is not None)
        self.preview.toggled.connect(lambda _on: self.update_preview())
        layout.addWidget(self.preview)
        self.note = QLabel()
        self.note.setWordWrap(True)
        self.note.setStyleSheet("color: #d08030;")
        layout.addWidget(self.note)
        layout.addStretch(1)
        if viewport is not None:
            viewport.preview_ended.connect(self._preview_ended)
        project.entities_changed.connect(self._entities_changed)
        self.refresh()

    # ---- showing the settings -------------------------------------------------------------
    def camera(self) -> dict[str, Any]:
        entity = self.project.entity(self.entity_id)
        return copy.deepcopy(entity.components.get("camera", {})) if entity else {}

    def view(self) -> str:
        return list(VIEWS)[self.tabs.currentIndex()]

    def _entities_changed(self, _entity_id: str) -> None:
        self.refresh()

    def refresh(self) -> None:
        camera = self.camera()
        self.start.blockSignals(True)
        self.start.setCurrentIndex(max(self.start.findData(camera.get("view", "third")), 0))
        self.start.blockSignals(False)
        self.blend.set_value(float(camera.get("blend", BLEND)))
        for (view, key), amount in self.amounts.items():
            amount.set_value(float(view_settings(camera, view)[key]))
        self.update_preview()

    def update_preview(self) -> None:
        if self.viewport is None:
            return
        if not self.preview.isChecked():
            if self.viewport.preview_view is not None:
                self.viewport.set_preview(None)
            return
        if not self.viewport.set_preview(self.view()):
            self.note.setText("Nothing to preview: the scene needs a player for the camera to "
                              "follow.")
        else:
            self.note.clear()

    def _preview_ended(self) -> None:
        self.preview.blockSignals(True)
        self.preview.setChecked(False)
        self.preview.blockSignals(False)

    def end_preview(self) -> None:
        self.preview.setChecked(False)

    # ---- changes (each one undoable) ------------------------------------------------------
    def _push(self, new: dict[str, Any], field: str, label: str) -> None:
        old = self.camera()
        if new == old or self.project.entity(self.entity_id) is None:
            return
        self.project.undo_stack.push(CameraCommand(self.project, self.entity_id, old, new, field,
                                                   label))

    def set_start(self, view: str) -> None:
        camera = self.camera()
        if view == "third":
            camera.pop("view", None)
        else:
            camera["view"] = view
        self._push(camera, "view", "starting view")

    def set_common(self, key: str, value: float, label: str) -> None:
        self._push({**self.camera(), key: round(value, 4)}, key, label)

    def set_view_value(self, view: str, setting: Setting, value: float) -> None:
        camera = self.camera()
        value = round(value, 4)
        views = camera.setdefault("views", {})
        settings = views.setdefault(view, {})
        if view == "third" and setting.key == "fov":
            camera["fov"] = value  # the camera's own field of view is third person's
            settings.pop("fov", None)
        elif view == "third" and setting.key == "distance" and value == 0.0:
            settings.pop("distance", None)  # where the camera is placed
        elif view == "top" and setting.key == "min_pitch":
            settings["min_pitch"] = settings["max_pitch"] = value  # one fixed angle
        else:
            settings[setting.key] = value
            # The zoom range and the angles stay in order: the other end follows.
            pairs = {"min_distance": ("max_distance", max), "max_distance": ("min_distance", min),
                     "min_pitch": ("max_pitch", max), "max_pitch": ("min_pitch", min)}
            if setting.key in pairs:
                other, keep = pairs[setting.key]
                current = view_settings(camera, view)[other]
                if keep(current, value) != current:
                    settings[other] = value
        if not settings:
            views.pop(view)
        if not views:
            camera.pop("views")
        self._push(camera, f"{view}.{setting.key}",
                   f"{VIEWS[view].lower()} {setting.label.lower()}")

    def reset_view(self, view: str) -> None:
        camera = self.camera()
        camera.get("views", {}).pop(view, None)
        if "views" in camera and not camera["views"]:
            camera.pop("views")
        if view == "third":
            camera.pop("fov", None)
        self._push(camera, f"{view}.reset", f"{VIEWS[view].lower()} settings (back to defaults)")


class CameraDialog(QDialog):
    """The Camera editor in a window of its own, kept open beside the editor (one per camera:
    asking for the same camera again brings its window forward)."""
    _open: dict[str, "CameraDialog"] = {}

    def __init__(self, project: Project, entity_id: str, viewport=None,
                 parent: QWidget | None = None):
        super().__init__(parent)
        entity = project.entity(entity_id)
        self.setWindowTitle(f"Camera — {entity.name if entity else entity_id}")
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose)
        self.editor = CameraEditor(project, entity_id, viewport)
        layout = QVBoxLayout(self)
        layout.addWidget(self.editor)
        self.resize(420, self.sizeHint().height())
        self.destroyed.connect(lambda *_a, k=entity_id: CameraDialog._open.pop(k, None))

    def done(self, result: int) -> None:
        self.editor.end_preview()  # the viewport's own camera again
        super().done(result)

    def closeEvent(self, e) -> None:
        self.editor.end_preview()
        super().closeEvent(e)

    @classmethod
    def close_all(cls) -> None:
        """Another project: the open Camera editors close."""
        for dialog in list(cls._open.values()):
            dialog.close()
        cls._open.clear()

    @classmethod
    def show_for(cls, project: Project, entity_id: str, viewport=None,
                 parent: QWidget | None = None) -> "CameraDialog":
        dialog = cls._open.get(entity_id)
        if dialog is None:
            dialog = cls(project, entity_id, viewport, parent)
            cls._open[entity_id] = dialog
        dialog.show()
        dialog.raise_()
        dialog.activateWindow()
        return dialog
