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

from PySide6.QtCore import QEvent, QPoint, Qt, QTimer, Signal
from PySide6.QtGui import (
    QAction,
    QGuiApplication,
    QMouseEvent,
    QPlatformSurfaceEvent,
    QWheelEvent,
    QWindow,
)
from PySide6.QtWidgets import QLabel, QStackedLayout, QToolBar, QVBoxLayout, QWidget

from .. import (
    engine,
    environments,
    model_materials,
    surface_shaders,
    terrain_stamps,
    textures,
    world_types,
)
from ..cook import BUILTIN_PREFIX, character_assets_used, mesh_asset_data
from ..objects import ObjectError, object_content
from ..project import Project
from ..scene_model import parents_first
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

    def ray(self, x: float, y: float, width: int, height: int) -> tuple[list[float], list[float]]:
        """The ray from the eye through pixel (x, y) of a view width x height: origin and
        direction (unit)."""
        eye = self.eye()
        forward = [self.target[i] - eye[i] for i in range(3)]
        n = math.sqrt(sum(c * c for c in forward)) or 1.0
        forward = [c / n for c in forward]
        right = [forward[1] * 0.0 - forward[2] * 1.0, forward[2] * 0.0 - forward[0] * 0.0,
                 forward[0] * 1.0 - forward[1] * 0.0]  # forward x (0, 1, 0)
        n = math.sqrt(sum(c * c for c in right)) or 1.0
        right = [c / n for c in right]
        up = [right[1] * forward[2] - right[2] * forward[1],
              right[2] * forward[0] - right[0] * forward[2],
              right[0] * forward[1] - right[1] * forward[0]]
        t = math.tan(math.radians(self.fov) / 2)
        aspect = width / max(height, 1)
        sx = (2.0 * (x + 0.5) / max(width, 1) - 1.0) * t * aspect
        sy = (1.0 - 2.0 * (y + 0.5) / max(height, 1)) * t
        d = [forward[i] + right[i] * sx + up[i] * sy for i in range(3)]
        n = math.sqrt(sum(c * c for c in d)) or 1.0
        return eye, [c / n for c in d]

    def pan(self, dx: float, dy: float, height: int) -> None:
        """Moves the target with the mouse: dx, dy in pixels of a view `height` pixels tall."""
        scale = 2 * self.distance * math.tan(math.radians(self.fov) / 2) / max(height, 1)
        yaw, pitch = math.radians(self.yaw), math.radians(self.pitch)
        right = (math.cos(yaw), 0.0, -math.sin(yaw))
        up = (-math.sin(pitch) * math.sin(yaw), math.cos(pitch), -math.sin(pitch) * math.cos(yaw))
        for i in range(3):
            self.target[i] += (-dx * right[i] + dy * up[i]) * scale


def drag_action(buttons, modifiers, sculpting: bool) -> str | None:
    """What a drag in the view does: "pan" (the middle button, or Shift with the left), "stroke"
    (the left button while sculpting), "orbit" (the left or right button), or None."""
    left = bool(buttons & Qt.MouseButton.LeftButton)
    if buttons & Qt.MouseButton.MiddleButton or (
            left and modifiers & Qt.KeyboardModifier.ShiftModifier):
        return "pan"
    if left and sculpting:
        return "stroke"
    if left or buttons & Qt.MouseButton.RightButton:
        return "orbit"
    return None


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
        if drag_action(e.button(), e.modifiers(), self.panel.sculpting()) == "stroke":
            pos = e.position()  # a stroke on the terrain, not an orbit
            self.panel.begin_stroke(pos.x(), pos.y(), self.width(), self.height())

    def mouseReleaseEvent(self, _e: QMouseEvent) -> None:
        self._last = None
        self.panel.end_stroke()

    def mouseMoveEvent(self, e: QMouseEvent) -> None:
        if self._last is None:
            return
        if self.panel.stroking() and e.buttons() & Qt.MouseButton.LeftButton:
            pos = e.position()
            self.panel.extend_stroke(pos.x(), pos.y(), self.width(), self.height())
            return
        pos = e.position().toPoint()
        dx, dy = pos.x() - self._last.x(), pos.y() - self._last.y()
        self._last = pos
        self.panel.set_preview(None)  # the designer takes the view back
        action = drag_action(e.buttons(), e.modifiers(), False)
        if action == "pan":
            self.panel.camera.pan(dx, dy, self.height())
        elif action == "orbit":
            self.panel.camera.orbit(dx, dy)
        self.panel.camera_changed()

    def wheelEvent(self, e: QWheelEvent) -> None:
        self.panel.set_preview(None)
        self.panel.camera.zoom(e.angleDelta().y() / 120.0)
        self.panel.camera_changed()


class ViewportPanel(QWidget):
    preview_ended = Signal()  # the Camera editor's preview gave the view back

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.project: Project | None = None
        self.camera = OrbitCamera()
        self.preview_view: str | None = None  # a game camera view shown (the Camera editor)
        self.view = None  # the engine's _midgard.Viewport, when there is one
        self._models = model_materials.ModelPreparer(self)  # models' materials, made aside
        self._models.ready.connect(self._model_ready)
        self._sent_meshes: set[str] = set()
        self._sent_content: dict[str, int] = {}  # object content id -> hash of what was sent
        self._sent_objects: dict[str, str] = {}  # object id -> the parts JSON sent
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
        self.navigation_action = QAction("Show &Navigation", self)
        self.navigation_action.setIconText("Navigation")
        self.navigation_action.setCheckable(True)
        self.navigation_action.setToolTip(
            "Outline where characters can walk: the navigation mesh, made from the scenery (or "
            "the terrain round where the view looks), green (F4 shows it in the game too)")
        self.navigation_action.toggled.connect(self.set_show_navigation)
        self.toolbar.addAction(self.navigation_action)
        self.lights_action = QAction("Show &Lights", self)
        self.lights_action.setIconText("Lights")
        self.lights_action.setCheckable(True)
        self.lights_action.setChecked(True)
        self.lights_action.setToolTip(
            "A light bulb where each point and spot light is, in its colour (faint where "
            "something hides it); a spot's cone, and the selected light's reach. Only here: "
            "the game shows just the light")
        self.lights_action.toggled.connect(self.set_show_lights)
        self.toolbar.addAction(self.lights_action)
        self.gizmos_action = QAction("Show &Gizmos", self)
        self.gizmos_action.setIconText("Gizmos")
        self.gizmos_action.setCheckable(True)
        self.gizmos_action.setChecked(True)
        self.gizmos_action.setToolTip("Move, rotate and scale the selected entity with the "
                                      "controls in the view's top-right corner")
        self.toolbar.addAction(self.gizmos_action)
        self.selected: str | None = None
        # Sculpting (the Terrain dock): a callable giving the brush ({"tool", "radius", ...}),
        # or None while sculpting is off; and the stroke being drawn.
        self.brush = None
        self._stroke: dict | None = None
        self._stroke_at: list[float] = [0.0, 0.0, 0.0]
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
            if hasattr(self.view, "show_lights"):
                self.view.show_lights = True
            self.window = ViewportWindow(self)
            container = QWidget.createWindowContainer(self.window, view_area)
            container.setMinimumSize(160, 120)
            container.setFocusPolicy(Qt.FocusPolicy.ClickFocus)
            self.setToolTip("Drag to orbit, middle-drag or Shift + drag to pan, wheel to zoom")
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
            self.project.objects_changed.disconnect(self._objects_changed)
            self.project.environments_changed.disconnect(self._environments_changed)
            self.project.terrain_changed.disconnect(self._terrain_changed)
            self.project.files_changed.disconnect(self._files_changed)
        self.project = project
        self.preview_view = None  # (the Camera editors close with the old project)
        self._sent_objects.clear()
        self.set_selected(None)
        if project is not None:
            project.entities_changed.connect(self._entities_changed)
            project.scene_settings_changed.connect(self._settings_changed)
            project.objects_changed.connect(self._objects_changed)
            project.environments_changed.connect(self._environments_changed)
            project.terrain_changed.connect(self._terrain_changed)
            project.world_types_changed.connect(lambda _id: self._settings_changed(project.scene))
            project.files_changed.connect(self._files_changed)
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
        if self.view is not None and hasattr(self.view, "selected"):
            self.view.selected = entity_id or ""  # (a selected light shows its reach)
            self.request_render()

    def set_show_colliders(self, show: bool) -> None:
        """Outlines of colliders (by how they move), triggers and the player."""
        if self.colliders_action.isChecked() != show:
            self.colliders_action.setChecked(show)  # comes back here through toggled
            return
        if self.view is not None:
            self.view.show_colliders = show
            self.request_render()

    def set_show_lights(self, show: bool) -> None:
        """Light bulbs where the point and spot lights are."""
        if self.lights_action.isChecked() != show:
            self.lights_action.setChecked(show)  # comes back here through toggled
            return
        if self.view is not None and hasattr(self.view, "show_lights"):
            self.view.show_lights = show
            self.request_render()

    def set_show_navigation(self, show: bool) -> None:
        """Where characters can walk: the navigation mesh's edges."""
        if self.navigation_action.isChecked() != show:
            self.navigation_action.setChecked(show)  # comes back here through toggled
            return
        if self.view is not None:
            self.view.show_navigation = show
            self.request_render()

    def _settings_changed(self, scene: str) -> None:
        if self.project is not None and scene == self.project.scene:
            self.view.set_settings(json.dumps(world_types.resolved_settings(
                self.project, self.project.scene_settings())))
            self.request_render()

    def _sync_all(self) -> None:
        self.view.clear()
        if self.project is not None:
            # The fallback for a scene without a Sky entity, as in the game.
            self.view.set_settings(json.dumps(world_types.resolved_settings(
                self.project, self.project.scene_settings())))
            self.send_environments()
            self.send_terrain()
            for entity in parents_first(self.project.entities()):
                self._put(entity)
        self.camera_changed()

    # ---- terrain: sculpting and painting ---------------------------------------------------
    def send_terrain(self, extra: dict | None = None) -> None:
        """The scene's terrain stamps (and the stroke being drawn, as a preview)."""
        if self.view is None or self.project is None or \
                not hasattr(self.view, "set_terrain_stamps"):
            return
        stamps = terrain_stamps.stamps(self.project)
        if extra is not None:
            stamps.append(extra)
        try:
            self.view.set_terrain_stamps(json.dumps(stamps))
        except ValueError as e:
            log.warning("Viewport: terrain stamps: %s", e)

    def _terrain_changed(self, _scene: str) -> None:
        self.send_terrain()
        self.request_render()

    def sculpting(self) -> bool:
        return self.brush is not None and self.brush() is not None

    def stroking(self) -> bool:
        return self._stroke is not None

    def _terrain_at(self, x: float, y: float, width: int, height: int,
                    face: int | None = None):
        if self.view is None or not hasattr(self.view, "terrain_hit"):
            return None
        origin, direction = self.camera.ray(x, y, width, height)
        return self.view.terrain_hit(origin, direction, 20000.0, face)

    def begin_stroke(self, x: float, y: float, width: int, height: int) -> bool:
        """Starts a stroke where the pointer meets the terrain (False: it doesn't)."""
        brush = self.brush() if self.brush is not None else None
        hit = self._terrain_at(x, y, width, height) if brush is not None else None
        if hit is None:
            return False
        face, u, v, point, ground = hit
        self._stroke = {**brush, "face": face, "points": [[u, v]]}
        if self._stroke["tool"] == "flatten":
            self._stroke["height"] = round(ground, 3)  # flat at where the stroke began
        self._stroke_at = point
        self.send_terrain(self._stroke)
        self.request_render()
        return True

    def extend_stroke(self, x: float, y: float, width: int, height: int) -> None:
        if self._stroke is None:
            return
        # On a sphere a stroke carries on across face edges, in its first face's terms.
        hit = self._terrain_at(x, y, width, height, self._stroke["face"])
        if hit is None:
            return
        face, u, v, point, _ground = hit
        moved = math.sqrt(sum((point[i] - self._stroke_at[i]) ** 2 for i in range(3)))
        if face != self._stroke["face"] or moved < self._stroke["radius"] * 0.25:
            return  # (another shape's face, or too far round) a point every quarter radius
        self._stroke["points"].append([u, v])
        self._stroke_at = point
        self.send_terrain(self._stroke)
        self.request_render()

    def end_stroke(self) -> None:
        """The stroke becomes a terrain stamp (one undoable step)."""
        stroke, self._stroke = self._stroke, None
        if stroke is None or self.project is None:
            return
        self.project.undo_stack.push(terrain_stamps.StampCommand(self.project, stroke))

    def _environments_changed(self, _environment_id: str) -> None:
        self.send_environments()
        self.request_render()

    def send_environments(self) -> None:
        """The project's environments, for the Sky to link to."""
        if self.view is None or self.project is None or \
                not hasattr(self.view, "set_environment"):
            return
        for env in environments.load_all(self.project):
            try:
                self.view.set_environment(env["id"], env["name"], json.dumps(env["curves"]),
                                          json.dumps(env["settings"]))
            except RuntimeError as e:
                log.warning("Viewport: environment %s: %s", env["name"], e)

    def _send_object(self, object_id: str) -> None:
        """An object's template, with its meshes and textures, for the instances that place
        it (an unknown object, or one that can't be imported, shows nothing)."""
        obj = self.project.object(object_id)
        parts: list = []
        if obj is not None:
            try:
                content = object_content(self.project, obj)
            except ObjectError as e:
                log.warning("Viewport: object %s: %s", obj.name, e)
            else:
                parts = content.parts
                for part in parts:  # (surface shaders the parts' materials use)
                    self._send_shader(surface_shaders.components_shader(
                        part.get("components", {})))
                for content_id, (_path, data) in content.files.items():
                    if self._sent_content.get(content_id) == hash(data):
                        continue
                    try:
                        if data[:4] == b"glTF":
                            self.view.set_mesh(content_id, data)
                        else:
                            self.view.set_texture(content_id, data)
                    except RuntimeError as e:
                        log.warning("Viewport: %s: %s", content_id, e)
                    self._sent_content[content_id] = hash(data)
        parts_json = json.dumps(parts)
        if self._sent_objects.get(object_id) != parts_json:
            try:
                self.view.set_object(object_id, parts_json)
            except RuntimeError as e:
                log.warning("Viewport: object %s: %s", object_id, e)
            self._sent_objects[object_id] = parts_json

    def _files_changed(self, paths: list) -> None:
        """Mesh files saved again on disk: sent again when their entities are (the watcher
        tells about those next)."""
        for path in paths:
            if path.startswith("objects/"):
                self._sent_meshes.discard("asset:" + path[len("objects/"):])
            elif path.startswith("library/"):  # (the Asset Library's models)
                self._sent_meshes.discard("lib:" + path[len("library/"):])

    def _objects_changed(self, object_id: str) -> None:
        """An object changed (or objects came or went): the instances in view follow."""
        if self.project is None:
            return
        if object_id:
            wanted = [object_id] if object_id in self._sent_objects else []
        else:  # one may have been deleted, or come back with Undo
            wanted = list(dict.fromkeys(
                list(self._sent_objects) + [e.components["object"].get("id", "")
                                            for e in self.project.entities()
                                            if "object" in e.components]))
        for i in wanted:
            if i:
                self._send_object(i)
        self.request_render()

    def _put(self, entity) -> None:
        object_id = entity.components.get("object", {}).get("id")
        if object_id and object_id not in self._sent_objects:
            self._send_object(object_id)
        for texture in textures.components_textures(entity.components):
            data = textures.texture_data(self.project.root, texture)
            if data is not None and self._sent_content.get(texture) != hash(data):
                try:
                    self.view.set_texture(texture, data)
                except RuntimeError as e:
                    log.warning("Viewport: texture %s: %s", texture, e)
                self._sent_content[texture] = hash(data)
        self._send_shader(surface_shaders.components_shader(entity.components))
        assets = [entity.components.get("mesh", {}).get("asset"),
                  *character_assets_used(entity.components)]  # (a character's files too)
        for asset in assets:
            if not asset or asset.startswith(BUILTIN_PREFIX) or asset in self._sent_meshes:
                continue
            data = mesh_asset_data(self.project, asset)
            if data is not None:
                try:
                    self.view.set_mesh(asset, data)
                except RuntimeError as e:
                    log.warning("Viewport: mesh %s: %s", asset, e)
                self._sent_meshes.add(asset)
        # A model used as a mesh: drawn with its own material under the entity's. Made the
        # first time on a worker thread (a minute, for a character with big textures): the
        # model is drawn plainly until then.
        model = model_materials.model_of(entity.components)
        if model and not model_materials.ready(self.project, model):
            self._models.prepare(self.project, model)
            components = entity.components
            self._put_entity(entity, components)
            return
        components = model_materials.effective(self.project, entity.components)
        for texture, data in model_materials.textures_of(self.project,
                                                         entity.components).items():
            if self._sent_content.get(texture) != hash(data):
                try:
                    self.view.set_texture(texture, data)
                except RuntimeError as e:
                    log.warning("Viewport: texture %s: %s", texture, e)
                self._sent_content[texture] = hash(data)
        self._put_entity(entity, components)

    def _send_shader(self, shader: str) -> None:
        """A surface shader (5.17), compiled here; one that doesn't compile is drawn magenta."""
        if not shader:
            return
        data = surface_shaders.compile_shader(self.project.root, shader).spirv or b""
        if self._sent_content.get(shader) != hash(data):
            try:
                self.view.set_texture(shader, data)
            except RuntimeError as e:
                log.warning("Viewport: surface shader %s: %s", shader, e)
            self._sent_content[shader] = hash(data)

    def _put_entity(self, entity, components: dict) -> None:
        try:
            self.view.put_entity(json.dumps({"id": entity.id, "name": entity.name,
                                             "parent": entity.parent,
                                             "components": components}))
        except RuntimeError as e:
            log.warning("Viewport: %s: %s", entity.name, e)

    def _model_ready(self, asset: str) -> None:
        """A model's material is made: the entities using it drawn with it."""
        self._models.done(asset)
        if self.view is None or self.project is None:
            return
        for entity in parents_first(self.project.entities()):
            if model_materials.model_of(entity.components) == asset:
                self._put(entity)
        self.request_render()

    def _entities_changed(self, entity_id: str) -> None:
        if not entity_id:  # added or removed: start again (scenes are small for now)
            self._sync_all()
            return
        if self.project.entity_scene(entity_id) not in (None, self.project.scene):
            return  # (an undo in a scene that isn't open)
        entity = self.project.entity(entity_id)
        if entity is None:
            self.view.remove_entity(entity_id)
        else:
            self._put(entity)
        self._show_preview()  # the camera, or the player, may have changed
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

    def keep_above_ground(self) -> None:
        """With terrain, the view stays above it: what it looks at at least half a metre up, and
        the eye at least a metre (the terrain is only drawn from above)."""
        if self.view is None or not hasattr(self.view, "terrain_ground"):
            return
        for point, clearance in ((self.camera.target, 0.5), (self.camera.eye(), 1.0)):
            at = self.view.terrain_ground(point)
            if at is None:
                return
            above, _ground, up = at
            if above < clearance:  # lift the whole view (it keeps looking the same way)
                for i in range(3):
                    self.camera.target[i] += up[i] * (clearance - above)

    def camera_changed(self) -> None:
        self.keep_above_ground()
        self.gizmos.refresh()  # the axes as the camera now sees them
        if self.view is not None:
            if not self._show_preview():
                self.view.set_camera(self.camera.eye(), self.camera.target, self.camera.fov)
            self.request_render()

    # ---- the Calendar dock's time of day -------------------------------------------------
    def set_solar_phase(self, phase: float | None) -> None:
        """Suns that follow the clock show this time of day (0 sunrise, 0.25 noon, 0.5
        sunset); None puts them back where they were placed."""
        if self.view is not None:
            self.view.solar_phase = phase
            self.request_render()

    def set_season(self, season: float) -> None:
        """The environment's season tints as at this season (1 summer, -1 winter, 0 none)."""
        if self.view is not None and hasattr(self.view, "season"):
            self.view.season = season
            self.request_render()

    # ---- the Camera editor's preview ----------------------------------------------------
    def set_preview(self, view: str | None) -> bool:
        """Shows the game camera's view ("first", "third" or "top") from where the game would
        put it, following edits, until set_preview(None) or the designer moves the view.
        False if there is nothing to show (no 3D view, or no player and camera)."""
        if view is None:
            if self.preview_view is None:
                return True
            self.preview_view = None
            self.camera_changed()
            self.preview_ended.emit()
            return True
        self.preview_view = view
        shown = self._show_preview()
        self.request_render()
        return shown

    def _show_preview(self) -> bool:
        if self.preview_view is None or self.view is None:
            return False
        pose = self.view.preview_camera(self.preview_view)
        if pose is None:
            return False
        eye, forward, _up, fov = pose
        self.view.set_camera(eye, [eye[i] + forward[i] for i in range(3)], fov)
        return True

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
