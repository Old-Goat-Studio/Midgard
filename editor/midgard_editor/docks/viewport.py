# SPDX-License-Identifier: Apache-2.0
"""The Viewport panel: the engine's renderer drawing the scene being edited (roadmap §2,
channel 3, and §3 "Viewport docks").

A QWindow gives the engine a native window (an HWND on Windows, an X11 window on Linux), wrapped
with QWidget.createWindowContainer() so it sits in a dock like any panel. The engine keeps the
scene; the renderer is attached to the window when it appears and detached just before Qt
destroys it. Docking, floating or moving the panel to another monitor can replace the native
window, so this is what keeps the viewport from drawing into a window that has gone.

Mouse: drag with the left or right button to orbit, the middle button to pan, the wheel to zoom.

The toolbar along its top holds the view's switches (Colliders: the physics shapes' outlines;
Gizmos: the Move, Rotate and Scale controls in the top-right corner, see gizmos.py). Its actions
are the panel's, so menus can share them.
"""
import json
import logging
import math
from dataclasses import dataclass, field

from PySide6.QtCore import QEvent, QPoint, Qt, QTimer
from PySide6.QtGui import (
    QAction,
    QGuiApplication,
    QMouseEvent,
    QPlatformSurfaceEvent,
    QWheelEvent,
    QWindow,
)
from PySide6.QtWidgets import QLabel, QStackedLayout, QToolBar, QVBoxLayout, QWidget

from .. import engine
from ..cook import BUILTIN_PREFIX, mesh_asset_data
from ..project import Project
from ..scene_model import MAIN_SCENE, parents_first
from .gizmos import GizmoPanel

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

        # The toolbar, then the view (or a message saying why there isn't one).
        self.toolbar = QToolBar("Viewport")
        self.toolbar.setToolButtonStyle(Qt.ToolButtonStyle.ToolButtonTextOnly)
        self.colliders_action = QAction("Show &Colliders", self)
        self.colliders_action.setIconText("Colliders")
        self.colliders_action.setCheckable(True)
        self.colliders_action.setToolTip(
            "Outline the physics shapes: static grey-green, dynamic orange, kinematic blue, "
            "triggers yellow, the player cyan (F4 does the same in the game)")
        self.colliders_action.toggled.connect(self.set_show_colliders)
        self.toolbar.addAction(self.colliders_action)
        self.gizmos_action = QAction("Show &Gizmos", self)
        self.gizmos_action.setIconText("Gizmos")
        self.gizmos_action.setCheckable(True)
        self.gizmos_action.setChecked(True)
        self.gizmos_action.setToolTip("Move, rotate and scale the selected entity with the "
                                      "controls in the view's top-right corner")
        self.toolbar.addAction(self.gizmos_action)
        self.selected: str | None = None
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)
        layout.addWidget(self.toolbar)
        view_area = self.view_area = QWidget()
        layout.addWidget(view_area, 1)
        self.stack = QStackedLayout(view_area)
        self.stack.addWidget(self.message)
        # The gizmos float over the view's top-right corner. The view is a native window, which
        # nothing inside the panel can show through, so the gizmos are a small see-through window
        # of their own that keeps to that corner (and hides when the view isn't showing).
        self.gizmos = GizmoPanel(lambda: (self.camera.yaw, self.camera.pitch),
                                 lambda: self.camera.distance * 0.0025, self)
        self.gizmos.setWindowFlags(Qt.WindowType.Tool | Qt.WindowType.FramelessWindowHint
                                   | Qt.WindowType.NoDropShadowWindowHint)
        self.gizmos.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.gizmos.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self.gizmos.adjustSize()
        self.gizmos.changed.connect(self.request_render)
        self.gizmos_action.toggled.connect(self.set_show_gizmos)
        self._gizmo_host = None  # the window (and screen) the gizmos float above
        self._follow = QTimer(self)  # docks move, float and hide without telling this panel
        self._follow.setInterval(100)
        self._follow.timeout.connect(self._place_gizmos)
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
            container = QWidget.createWindowContainer(self.window, view_area)
            container.setMinimumSize(160, 120)
            container.setFocusPolicy(Qt.FocusPolicy.ClickFocus)
            self.setToolTip("Drag to orbit, middle-drag to pan, wheel to zoom")
            self.stack.addWidget(container)
            self.stack.setCurrentWidget(container)
            self.window.screenChanged.connect(lambda _s: self.window_resized())
        self.toolbar.setEnabled(self.view is not None)
        self.set_project(None)
        self.set_show_gizmos(True)

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
            self.project.scene_settings_changed.disconnect(self._settings_changed)
        self.project = project
        self.set_selected(None)
        if project is not None:
            project.entities_changed.connect(self._entities_changed)
            project.scene_settings_changed.connect(self._settings_changed)
            camera = next((e for e in project.entities() if "camera" in e.components), None)
            if camera is not None:  # start where the game's camera starts
                self.camera = OrbitCamera.looking(camera.transform()["position"],
                                                  [0.0, 0.5, 0.0])
        self._sync_all()

    def _place_gizmos(self) -> None:
        """Keeps the gizmos in the view's top-right corner while the view is on screen."""
        top = self.view_area.window()  # (self.window is the view's own QWindow)
        wanted = (self.gizmos_action.isChecked() and self.view is not None
                  and self.view_area.isVisible() and not top.isMinimized())
        if not wanted:
            self.gizmos.hide()
            return
        # A window stays above the window that owns it, and no other. When the view moves to
        # another window (its dock floated, docked again, or put on another screen), the gizmos
        # move to that window and are shown afresh above it.
        host = (top, top.screen())
        if host != self._gizmo_host:
            self._gizmo_host = host
            flags = self.gizmos.windowFlags()
            self.gizmos.hide()
            self.gizmos.setParent(top, flags)
        corner = self.view_area.mapToGlobal(
            QPoint(max(self.view_area.width() - self.gizmos.width() - 8, 0), 8))
        if self.gizmos.pos() != corner:
            self.gizmos.move(corner)
        if not self.gizmos.isVisible():
            self.gizmos.show()
            self.gizmos.raise_()

    def set_show_gizmos(self, show: bool) -> None:
        """The Move, Rotate and Scale controls in the view's top-right corner."""
        if self.gizmos_action.isChecked() != show:
            self.gizmos_action.setChecked(show)  # comes back here through toggled
            return
        if show and self.view is not None:
            self._follow.start()
        else:
            self._follow.stop()
        self._place_gizmos()

    def set_selected(self, entity_id: str | None) -> None:
        """The entity the gizmos act on (the Scene panel's selection)."""
        self.selected = entity_id
        self.gizmos.set_entity(self.project, entity_id)

    def set_show_colliders(self, show: bool) -> None:
        """Outlines of colliders (by how they move), triggers and the player."""
        if self.colliders_action.isChecked() != show:
            self.colliders_action.setChecked(show)  # comes back here through toggled
            return
        if self.view is not None:
            self.view.show_colliders = show
            self.request_render()

    def _settings_changed(self, _scene: str) -> None:
        if self.project is not None:
            self.view.set_settings(json.dumps(self.project.scene_settings(MAIN_SCENE)))
            self.request_render()

    def _sync_all(self) -> None:
        self.view.clear()
        if self.project is not None:
            # The fallback for a scene without a Sky entity, as in the game.
            self.view.set_settings(json.dumps(self.project.scene_settings(MAIN_SCENE)))
            for entity in parents_first(self.project.entities()):
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
        self.gizmos.refresh()  # the axes as the camera now sees them
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
