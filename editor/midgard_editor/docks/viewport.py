# SPDX-License-Identifier: Apache-2.0
"""The Viewport panel: the engine's renderer drawing the scene being edited (roadmap §2,
channel 3, and §3 "Viewport docks").

A QWindow gives the engine a native window (an HWND on Windows, an X11 window on Linux), wrapped
with QWidget.createWindowContainer() so it sits in a dock like any panel. The engine keeps the
scene; the renderer is attached to the window when it appears and detached just before Qt
destroys it. Docking, floating or moving the panel to another monitor can replace the native
window, so this is what keeps the viewport from drawing into a window that has gone.

Mouse: drag with the left or right button to orbit, the middle button to pan, the wheel to zoom.
"""
import json
import logging
import math
from dataclasses import dataclass, field

from PySide6.QtCore import QEvent, QPoint, Qt, QTimer
from PySide6.QtGui import (
    QGuiApplication,
    QMouseEvent,
    QPlatformSurfaceEvent,
    QWheelEvent,
    QWindow,
)
from PySide6.QtWidgets import QLabel, QStackedLayout, QWidget

from .. import engine
from ..cook import BUILTIN_PREFIX, mesh_asset_data
from ..project import Project
from ..scene_model import MAIN_SCENE

log = logging.getLogger("midgard")
PLATFORMS = ("windows", "xcb")  # window systems the engine can draw into


@dataclass
class OrbitCamera:
    """A camera circling a target: yaw and pitch in degrees, distance in metres."""
    target: list[float] = field(default_factory=lambda: [0.0, 0.5, 0.0])
    yaw: float = 35.0
    pitch: float = 25.0
    distance: float = 8.0
    fov: float = 55.0

    @classmethod
    def looking(cls, eye: list[float], target: list[float], fov: float = 55.0) -> "OrbitCamera":
        d = [eye[i] - target[i] for i in range(3)]
        distance = max(math.sqrt(sum(c * c for c in d)), 0.5)
        return cls(list(target), math.degrees(math.atan2(d[0], d[2])),
                   math.degrees(math.asin(max(-1.0, min(1.0, d[1] / distance)))), distance, fov)

    def eye(self) -> list[float]:
        yaw, pitch = math.radians(self.yaw), math.radians(self.pitch)
        offset = (math.sin(yaw) * math.cos(pitch), math.sin(pitch),
                  math.cos(yaw) * math.cos(pitch))
        return [self.target[i] + offset[i] * self.distance for i in range(3)]

    def orbit(self, dx: float, dy: float) -> None:
        self.yaw -= dx * 0.4
        self.pitch = max(-89.0, min(89.0, self.pitch + dy * 0.4))

    def zoom(self, steps: float) -> None:
        self.distance = max(0.2, min(5000.0, self.distance * 0.85 ** steps))

    def pan(self, dx: float, dy: float, height: int) -> None:
        """Moves the target with the mouse: dx, dy in pixels of a view `height` pixels tall."""
        scale = 2 * self.distance * math.tan(math.radians(self.fov) / 2) / max(height, 1)
        yaw, pitch = math.radians(self.yaw), math.radians(self.pitch)
        right = (math.cos(yaw), 0.0, -math.sin(yaw))
        up = (-math.sin(pitch) * math.sin(yaw), math.cos(pitch), -math.sin(pitch) * math.cos(yaw))
        for i in range(3):
            self.target[i] += (-dx * right[i] + dy * up[i]) * scale


class ViewportWindow(QWindow):
    """The native window the engine draws into. It forwards what it sees to the panel."""

    def __init__(self, panel: "ViewportPanel"):
        super().__init__()
        self.panel = panel
        self._last: QPoint | None = None

    def event(self, e: QEvent) -> bool:
        if (e.type() == QEvent.Type.PlatformSurface and e.surfaceEventType() ==
                QPlatformSurfaceEvent.SurfaceEventType.SurfaceAboutToBeDestroyed):
            self.panel.detach()  # before the native window goes
        return super().event(e)

    def exposeEvent(self, _e) -> None:
        if self.isExposed():
            self.panel.attach()
            self.panel.request_render()

    def resizeEvent(self, _e) -> None:
        self.panel.window_resized()

    def mousePressEvent(self, e: QMouseEvent) -> None:
        self._last = e.position().toPoint()

    def mouseReleaseEvent(self, _e: QMouseEvent) -> None:
        self._last = None

    def mouseMoveEvent(self, e: QMouseEvent) -> None:
        if self._last is None:
            return
        pos = e.position().toPoint()
        dx, dy = pos.x() - self._last.x(), pos.y() - self._last.y()
        self._last = pos
        if e.buttons() & Qt.MouseButton.MiddleButton:
            self.panel.camera.pan(dx, dy, self.height())
        elif e.buttons() & (Qt.MouseButton.LeftButton | Qt.MouseButton.RightButton):
            self.panel.camera.orbit(dx, dy)
        self.panel.camera_changed()

    def wheelEvent(self, e: QWheelEvent) -> None:
        self.panel.camera.zoom(e.angleDelta().y() / 120.0)
        self.panel.camera_changed()


class ViewportPanel(QWidget):
    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.project: Project | None = None
        self.camera = OrbitCamera()
        self.view = None  # the engine's _midgard.Viewport, when there is one
        self._sent_meshes: set[str] = set()
        self._render_pending = False
        self.message = QLabel()
        self.message.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.message.setWordWrap(True)
        self.stack = QStackedLayout(self)
        self.stack.addWidget(self.message)
        self.window: ViewportWindow | None = None

        module = engine.module()
        platform = QGuiApplication.platformName()
        if module is None:
            self._show_message("The 3D view needs the engine module (_midgard). Build the engine "
                               "(see the README), or use a packaged editor.")
        elif platform not in PLATFORMS:
            self._show_message(f"The 3D view can't draw on this display ({platform}). On Linux, "
                               "turn on Edit ▸ Preferences ▸ Use X11 and restart the editor.")
        else:
            self.view = module.Viewport()
            self.window = ViewportWindow(self)
            container = QWidget.createWindowContainer(self.window, self)
            container.setMinimumSize(160, 120)
            container.setFocusPolicy(Qt.FocusPolicy.ClickFocus)
            self.setToolTip("Drag to orbit, middle-drag to pan, wheel to zoom")
            self.stack.addWidget(container)
            self.stack.setCurrentWidget(container)
            self.window.screenChanged.connect(lambda _s: self.window_resized())
        self.set_project(None)

    def _show_message(self, text: str) -> None:
        self.message.setText(text)
        self.stack.setCurrentWidget(self.message)

    # ---- the project's scene ------------------------------------------------------------
    def set_project(self, project: Project | None) -> None:
        if self.view is None:  # no 3D view here: nothing to keep in step
            self.project = project
            return
        if self.project is not None:
            self.project.entities_changed.disconnect(self._entities_changed)
            self.project.scene_changed.disconnect(self._scene_changed)
        self.project = project
        if project is not None:
            project.entities_changed.connect(self._entities_changed)
            project.scene_changed.connect(self._scene_changed)
            camera = next((e for e in project.entities() if "camera" in e.components), None)
            if camera is not None:  # start where the game's camera starts
                self.camera = OrbitCamera.looking(camera.transform()["position"],
                                                  [0.0, 0.5, 0.0])
        self._sync_all()

    def _sync_all(self) -> None:
        self.view.clear()
        if self.project is not None:
            self.view.set_settings(json.dumps(self.project.scene_settings(MAIN_SCENE)))
            for entity in self.project.entities():
                self._put(entity)
        self.camera_changed()

    def _put(self, entity) -> None:
        asset = entity.components.get("mesh", {}).get("asset")
        if asset and not asset.startswith(BUILTIN_PREFIX) and asset not in self._sent_meshes:
            data = mesh_asset_data(self.project, asset)
            if data is not None:
                try:
                    self.view.set_mesh(asset, data)
                except RuntimeError as e:
                    log.warning("Viewport: mesh %s: %s", asset, e)
                self._sent_meshes.add(asset)
        try:
            self.view.put_entity(json.dumps({"id": entity.id, "name": entity.name,
                                             "parent": entity.parent,
                                             "components": entity.components}))
        except RuntimeError as e:
            log.warning("Viewport: %s: %s", entity.name, e)

    def _entities_changed(self, entity_id: str) -> None:
        if not entity_id:  # added or removed: start again (scenes are small for now)
            self._sync_all()
            return
        entity = self.project.entity(entity_id)
        if entity is None:
            self.view.remove_entity(entity_id)
        else:
            self._put(entity)
        self.request_render()

    def _scene_changed(self, scene_id: str) -> None:
        if scene_id == MAIN_SCENE:
            self.view.set_settings(json.dumps(self.project.scene_settings(scene_id)))
            self.request_render()

    # ---- the window ---------------------------------------------------------------------
    def _pixels(self) -> tuple[int, int]:
        ratio = self.window.devicePixelRatio()
        return max(1, round(self.window.width() * ratio)), max(1, round(self.window.height() *
                                                                        ratio))

    def attach(self) -> None:
        if self.view is None or self.view.attached or self.window is None:
            return
        width, height = self._pixels()
        try:
            self.view.attach_window(QGuiApplication.platformName(), int(self.window.winId()),
                                    width, height)
        except RuntimeError as e:
            log.error("The 3D view could not start: %s", e)
            self._show_message(f"The 3D view could not start: {e}")
            self.view = None
            return
        log.info("Viewport drawing on %s", self.view.device_name)

    def detach(self) -> None:
        if self.view is not None and self.view.attached:
            self.view.detach()

    def window_resized(self) -> None:
        if self.view is not None and self.view.attached:
            self.view.resize(*self._pixels())
            self.request_render()

    def camera_changed(self) -> None:
        if self.view is not None:
            self.view.set_camera(self.camera.eye(), self.camera.target, self.camera.fov)
            self.request_render()

    def request_render(self) -> None:
        """Draws on the next pass of the event loop; many changes make one frame."""
        if self.view is not None and not self._render_pending:
            self._render_pending = True
            QTimer.singleShot(0, self._render)

    def _render(self) -> None:
        self._render_pending = False
        if self.view is None or self.window is None or not self.window.isExposed():
            return
        if not self.view.attached:
            self.attach()
        if self.view is not None and self.view.attached and not self.view.render():
            self.request_render()  # the window changed size: draw again into the new one
