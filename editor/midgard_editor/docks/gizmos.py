# SPDX-License-Identifier: Apache-2.0
"""Transform gizmos: three small controls in the viewport's top-right corner that move, rotate
and scale the selected entity (the Viewport toolbar's Gizmos button shows or hides them). Only
the gizmos and their captions are drawn: the scene shows through the rest.

Each draws the world's X, Y and Z axes (red, green, blue) as the viewport's camera sees them.
Drag a handle along its axis: Move slides the entity along that world axis, Rotate turns it about
that world axis (even inside a turned group), Scale stretches it along its own axis (the white
centre scales it evenly; a group always scales evenly). The entity updates
as you drag, and each drag is one step on the undo stack.
"""
import copy
import math
from collections.abc import Callable

from PySide6.QtCore import QPointF, QSize, Qt, Signal
from PySide6.QtGui import QColor, QMouseEvent, QPainter, QPaintEvent, QPen
from PySide6.QtWidgets import (
    QGraphicsDropShadowEffect,
    QHBoxLayout,
    QLabel,
    QVBoxLayout,
    QWidget,
)

from .. import transforms
from ..project import Project
from ..scene_model import Entity
from ..undo import SetEntityCommand

AXES = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))
AXIS_COLOURS = (QColor(230, 70, 60), QColor(90, 200, 80), QColor(70, 130, 240))
AXIS_NAMES = "XYZ"
UNIFORM = 3  # the Scale gizmo's centre handle: all three axes at once
KINDS = {"position": "Move", "rotation": "Rotate", "scale": "Scale"}


def view_axes(yaw: float, pitch: float) -> list[tuple[float, float, float]]:
    """Each world axis on screen for an orbit camera at (yaw, pitch) in degrees: its (x, y)
    direction in pixels-to-the-right and pixels-down, and how much it points away (depth)."""
    y, p = math.radians(yaw), math.radians(pitch)
    forward = (-math.sin(y) * math.cos(p), -math.sin(p), -math.cos(y) * math.cos(p))
    right = (math.cos(y), 0.0, -math.sin(y))
    up = (right[1] * forward[2] - right[2] * forward[1],
          right[2] * forward[0] - right[0] * forward[2],
          right[0] * forward[1] - right[1] * forward[0])
    dot = lambda a, b: sum(a[i] * b[i] for i in range(3))  # noqa: E731
    return [(dot(a, right), -dot(a, up), dot(a, forward)) for a in AXES]


class AxisGizmo(QWidget):
    """One gizmo (Move, Rotate or Scale): the three axes, each with a handle to drag."""

    SIZE = 76
    REACH = 28  # pixels from the centre to a handle
    HANDLE = 7

    def __init__(self, kind: str, panel: "GizmoPanel"):
        super().__init__(panel)
        self.kind, self.panel = kind, panel
        self.setFixedSize(QSize(self.SIZE, self.SIZE))
        self.setToolTip(f"{KINDS[kind]} the selected entity: drag a handle along its axis"
                        + (" (the centre scales evenly)" if kind == "scale" else ""))
        self.dragging: int | None = None
        self.press = QPointF()

    def centre(self) -> QPointF:
        return QPointF(self.SIZE / 2, self.SIZE / 2)

    def handles(self) -> list[tuple[int, QPointF, float]]:
        """(axis, where its handle is, depth), farthest first so the nearest is drawn on top."""
        out = []
        for i, (x, y, depth) in enumerate(view_axes(*self.panel.camera_angles())):
            out.append((i, self.centre() + QPointF(x, y) * self.REACH, depth))
        return sorted(out, key=lambda h: -h[2])

    def direction(self, axis: int) -> QPointF:
        """The axis's direction on screen, unit length (straight up if it points at you)."""
        if axis == UNIFORM:
            return QPointF(0.7071, -0.7071)  # up and to the right: bigger
        x, y, _depth = view_axes(*self.panel.camera_angles())[axis]
        length = math.hypot(x, y)
        return QPointF(x / length, y / length) if length > 0.2 else QPointF(0.0, -1.0)

    def handle_at(self, point: QPointF) -> int | None:
        if self.kind == "scale" and (point - self.centre()).manhattanLength() < self.HANDLE + 2:
            return UNIFORM
        best, best_distance = None, self.HANDLE + 4.0
        for axis, where, _depth in self.handles():
            distance = math.hypot(point.x() - where.x(), point.y() - where.y())
            if distance <= best_distance:
                best, best_distance = axis, distance
        return best

    # ---- drawing ------------------------------------------------------------------------
    def paintEvent(self, _event: QPaintEvent) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        enabled = self.isEnabled()
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor(20, 22, 26, 70))  # faint, so the handles read over any scene
        painter.drawEllipse(self.centre(), self.SIZE / 2 - 1, self.SIZE / 2 - 1)
        for axis, where, _depth in self.handles():
            colour = AXIS_COLOURS[axis] if enabled else QColor(110, 110, 110)
            if self.dragging == axis:
                colour = colour.lighter(140)
            painter.setPen(QPen(colour, 2.5))
            painter.drawLine(self.centre(), where)
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(colour)
            if self.kind == "scale":
                painter.drawRect(int(where.x()) - self.HANDLE + 1, int(where.y()) - self.HANDLE + 1,
                                 2 * self.HANDLE - 2, 2 * self.HANDLE - 2)
            elif self.kind == "rotation":
                painter.setPen(QPen(colour, 3))
                painter.setBrush(Qt.BrushStyle.NoBrush)
                painter.drawEllipse(where, self.HANDLE - 1, self.HANDLE - 1)
            else:
                painter.drawEllipse(where, self.HANDLE, self.HANDLE)
            painter.setPen(QColor(20, 20, 20))
            painter.drawText(int(where.x()) - 4, int(where.y()) + 4, AXIS_NAMES[axis])
        if self.kind == "scale":
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QColor(235, 235, 235) if enabled else QColor(110, 110, 110))
            c = self.centre()
            painter.drawRect(int(c.x()) - 5, int(c.y()) - 5, 10, 10)
        painter.end()

    # ---- dragging -----------------------------------------------------------------------
    def mousePressEvent(self, event: QMouseEvent) -> None:
        axis = self.handle_at(event.position())
        if axis is None or event.button() != Qt.MouseButton.LeftButton:
            return
        if self.panel.begin(self.kind, axis):
            self.dragging, self.press = axis, event.position()
            self.update()

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        if self.dragging is None:
            return
        d = event.position() - self.press
        along = self.direction(self.dragging)
        self.panel.drag(d.x() * along.x() + d.y() * along.y())

    def mouseReleaseEvent(self, _event: QMouseEvent) -> None:
        if self.dragging is not None:
            self.dragging = None
            self.panel.end()
            self.update()


class GizmoPanel(QWidget):
    """Move, Rotate and Scale side by side, acting on the selected entity."""

    changed = Signal()  # the selected entity moved while dragging (the viewport redraws)

    def __init__(self, camera_angles: Callable[[], tuple[float, float]],
                 metres_per_pixel: Callable[[], float], parent: QWidget | None = None):
        super().__init__(parent)
        self.camera_angles, self.metres_per_pixel = camera_angles, metres_per_pixel
        self.project: Project | None = None
        self.entity_id: str | None = None
        self.start: Entity | None = None  # the entity as a drag began
        self.kind, self.axis = "position", 0
        row = QHBoxLayout(self)
        row.setContentsMargins(6, 6, 6, 4)
        row.setSpacing(6)
        self.gizmos: dict[str, AxisGizmo] = {}
        for kind, label in KINDS.items():
            column = QVBoxLayout()
            column.setSpacing(2)
            gizmo = AxisGizmo(kind, self)
            caption = QLabel(label)
            caption.setAlignment(Qt.AlignmentFlag.AlignCenter)
            caption.setStyleSheet("color: #f2f2f2; background: transparent;")
            outline = QGraphicsDropShadowEffect(caption)  # readable over sky and ground alike
            outline.setColor(QColor(0, 0, 0, 230))
            outline.setOffset(0, 0)
            outline.setBlurRadius(5)
            caption.setGraphicsEffect(outline)
            column.addWidget(gizmo)
            column.addWidget(caption)
            row.addLayout(column)
            self.gizmos[kind] = gizmo
        self.set_entity(None, None)

    def set_entity(self, project: Project | None, entity_id: str | None) -> None:
        """The entity the gizmos act on (folders and the sky have no transform to change)."""
        self.project, self.entity_id = project, entity_id
        entity = project.entity(entity_id) if project and entity_id else None
        usable = entity is not None and not entity.is_folder and "sky" not in entity.components
        self.setEnabled(usable)
        self.setToolTip("" if usable else "Select an entity in the Scene panel to move it")
        self.refresh()

    def refresh(self) -> None:
        for gizmo in self.gizmos.values():
            gizmo.update()

    # ---- a drag: begin, drag (as often as the mouse moves), end -------------------------
    def begin(self, kind: str, axis: int) -> bool:
        if not self.isEnabled() or self.project is None or self.entity_id is None:
            return False
        entity = self.project.entity(self.entity_id)
        if entity is None:
            return False
        self.start, self.kind, self.axis = copy.deepcopy(entity), kind, axis
        return True

    def drag(self, pixels: float) -> None:
        """The handle has moved `pixels` along its axis since the drag began."""
        if self.start is None or self.project is None:
            return
        entity = copy.deepcopy(self.start)
        t = entity.transform()
        if self.kind == "scale":
            factor = max(0.001, 1.0 + pixels * 0.01)
            even = self.axis == UNIFORM or "group" in entity.components  # groups scale evenly
            for axis in (range(3) if even else (self.axis,)):
                t["scale"][axis] = max(0.001, t["scale"][axis] * factor)
            entity.components["transform"] = {k: [round(v, 4) for v in t[k]] for k in t}
        else:
            # Along or about the world's axis, whatever the parents' turn: the change is made in
            # the world, then expressed relative to the parent.
            entities = {e.id: e for e in self.project.entities()}
            entities[entity.id] = entity
            world = transforms.world_matrix(entities, entity.id)
            if self.kind == "position":
                world[self.axis][3] += pixels * self.metres_per_pixel()
            else:
                turn = transforms.axis_rotation(AXES[self.axis], pixels * 0.5)  # degrees
                for col in range(3):
                    column = [world[row][col] for row in range(3)]
                    for row in range(3):
                        world[row][col] = sum(turn[row][k] * column[k] for k in range(3))
            try:
                local = transforms.relative_to(transforms.world_matrix(entities, entity.parent),
                                               world)
            except ValueError:
                return
            if self.kind == "rotation":
                local["scale"] = t["scale"]  # turning never changes the size
            entity.components["transform"] = local
        self.project.put_entity(entity)  # live; the undo step is made when the drag ends
        self.changed.emit()

    def end(self) -> None:
        if self.start is None or self.project is None:
            return
        now = self.project.entity(self.start.id)
        if now is not None and now.components != self.start.components:
            command = SetEntityCommand(self.project, now, KINDS[self.kind].lower())
            command.old = self.start  # from before the drag, not its last live step
            self.project.undo_stack.push(command)
        self.start = None
