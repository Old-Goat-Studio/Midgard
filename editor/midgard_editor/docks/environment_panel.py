# SPDX-License-Identifier: Apache-2.0
"""The Environment editor (roadmap §6; milestone 5.8): how a world looks through its day.

Choose an environment (or make one), then a curve: the sky's colours and brightness, the sun's
glow and disc, the sunlight, the ambient light and the fog. Each is a curve over the day (solar
phase: sunrise, noon, sunset, midnight). In the curve view, drag a key to move it, double-click
to add one (or, on a colour key, to choose its colour; on a number, to type it), and press
Delete to remove the selected one. A curve that isn't set follows the analytic sky (shown faint
behind it); "Start from the analytic sky" turns that into keys to change. The time-of-day slider
previews the look in the viewport (as the Calendar dock's scrubber does), and the running game
follows every change. Seasons: tints for the sunlight, ambient light and sky at the depth of
winter and of summer (a colder light in winter, say), blended through the year; the season
slider previews them. Every change is undoable; dragging a key is one step in the History.
"""
import copy
import json
from typing import Any

from PySide6.QtCore import QPointF, QRectF, Qt, Signal
from PySide6.QtGui import QColor, QKeyEvent, QMouseEvent, QPainter, QPainterPath, QPen
from PySide6.QtWidgets import (
    QColorDialog,
    QComboBox,
    QDoubleSpinBox,
    QFormLayout,
    QGridLayout,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMessageBox,
    QPushButton,
    QScrollArea,
    QSlider,
    QStackedLayout,
    QVBoxLayout,
    QWidget,
)

from .. import engine, environments
from ..environments import (
    COLOURS,
    GROUPS,
    LABELS,
    SEASONS,
    SETTINGS,
    TINTS,
    WHITE,
    EnvironmentCommand,
)
from ..project import Project

NAME_ROLE = Qt.ItemDataRole.UserRole
MARKS = ((0.0, "Sunrise"), (0.25, "Noon"), (0.5, "Sunset"), (0.75, "Midnight"))
SAMPLE_COUNT = 96


def _qcolor(rgb) -> QColor:
    return QColor.fromRgbF(*(min(max(float(c), 0.0), 1.0) for c in rgb[:3]))


class CurveView(QWidget):
    """One curve over the day. Numbers are drawn as a line, colours as a strip; the analytic
    sky's values are drawn faintly behind (or below) for reference."""
    edited = Signal(list)  # the curve's keys, after a change

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.setMinimumHeight(150)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setMouseTracking(False)
        self.name = ""
        self.keys: list = []
        self.reference: list = []  # SAMPLE_COUNT values of the analytic sky (empty: none)
        self.preview: float | None = None  # the previewed time of day
        self.selected: int | None = None
        self._dragging = False

    # ---- what it shows ---------------------------------------------------------------------------
    @property
    def colour(self) -> bool:
        return self.name in COLOURS

    def show_curve(self, name: str, keys: list, reference: list) -> None:
        if name != self.name:
            self.selected = None
        self.name, self.keys, self.reference = name, copy.deepcopy(keys), reference
        if self.selected is not None and self.selected >= len(self.keys):
            self.selected = None
        self.update()

    def set_preview(self, phase: float | None) -> None:
        self.preview = phase
        self.update()

    def value_at(self, phase: float):
        """The curve here, or the analytic sky's where it isn't set."""
        value = environments.evaluate_curve(self.keys, phase)
        if value is None and self.reference:
            value = self.reference[int(phase % 1.0 * SAMPLE_COUNT) % SAMPLE_COUNT]
        if value is None:
            value = [0.5, 0.5, 0.5] if self.colour else 0.0
        return copy.deepcopy(value)

    # ---- geometry --------------------------------------------------------------------------------
    def _plot(self) -> QRectF:
        return QRectF(8, 6, max(self.width() - 16, 1), max(self.height() - 26, 1))

    def _top(self) -> float:
        """The number at the top of the plot."""
        values = [k[1] for k in self.keys if not isinstance(k[1], list)]
        values += [v for v in self.reference if not isinstance(v, list) and v is not None]
        return max(max(values, default=1.0) * 1.15, 1e-6)

    def _point(self, phase: float, value) -> QPointF:
        r = self._plot()
        x = r.left() + phase * r.width()
        if self.colour:
            return QPointF(x, r.top() + r.height() * 0.35)
        return QPointF(x, r.bottom() - float(value) / self._top() * r.height())

    def _phase_at(self, x: float) -> float:
        r = self._plot()
        return min(max((x - r.left()) / r.width(), 0.0), 0.999)

    def _value_at_y(self, y: float) -> float:
        r = self._plot()
        return max((r.bottom() - y) / r.height() * self._top(), 0.0)

    def _key_near(self, pos: QPointF) -> int | None:
        best, best_d = None, 9.0
        for i, (phase, value) in enumerate(self.keys):
            p = self._point(phase, value)
            d = ((p.x() - pos.x()) ** 2 + (p.y() - pos.y()) ** 2) ** 0.5
            if d < best_d:
                best, best_d = i, d
        return best

    # ---- drawing ---------------------------------------------------------------------------------
    def paintEvent(self, _e) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        r = self._plot()
        pal = self.palette()
        painter.fillRect(self.rect(), pal.base())
        # Night (the second half of the day) a little darker.
        night = QColor(pal.text().color())
        night.setAlphaF(0.07)
        painter.fillRect(QRectF(r.left() + r.width() * 0.5, r.top(), r.width() * 0.5, r.height()),
                         night)
        painter.setPen(QPen(pal.mid().color(), 1))
        for phase, label in MARKS:
            x = r.left() + phase * r.width()
            painter.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()))
            painter.drawText(QRectF(x + 3, r.bottom() + 2, 80, 18), label)
        if self.colour:
            self._paint_colours(painter, r)
        else:
            self._paint_numbers(painter, r)
        if self.preview is not None:
            x = r.left() + self.preview % 1.0 * r.width()
            painter.setPen(QPen(pal.highlight().color(), 2))
            painter.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()))
        for i, (phase, value) in enumerate(self.keys):
            p = self._point(phase, value)
            painter.setPen(QPen(pal.highlight().color() if i == self.selected
                                else pal.text().color(), 2))
            painter.setBrush(_qcolor(value) if self.colour else pal.base())
            painter.drawEllipse(p, 6.0, 6.0)

    def _paint_colours(self, painter: QPainter, r: QRectF) -> None:
        strip = QRectF(r.left(), r.top(), r.width(), r.height() * 0.7)
        below = QRectF(r.left(), r.top() + r.height() * 0.78, r.width(), r.height() * 0.22)
        step = r.width() / SAMPLE_COUNT
        for i in range(SAMPLE_COUNT):
            phase = (i + 0.5) / SAMPLE_COUNT
            x = r.left() + i * step
            painter.fillRect(QRectF(x, strip.top(), step + 1, strip.height()),
                             _qcolor(self.value_at(phase)))
            if self.reference and self.reference[i] is not None:
                painter.fillRect(QRectF(x, below.top(), step + 1, below.height()),
                                 _qcolor(self.reference[i]))
        painter.setPen(self.palette().text().color())
        if self.reference:
            painter.drawText(below.adjusted(4, 0, 0, 0), Qt.AlignmentFlag.AlignVCenter,
                             "analytic sky")
        if not self.keys:
            painter.drawText(strip, Qt.AlignmentFlag.AlignCenter,
                             "Not set: the analytic sky" if self.reference else
                             "Not set: the scene's own")

    def _paint_numbers(self, painter: QPainter, r: QRectF) -> None:
        pal = self.palette()
        if self.reference:
            faint = QColor(pal.text().color())
            faint.setAlphaF(0.35)
            painter.setPen(QPen(faint, 1, Qt.PenStyle.DashLine))
            path = QPainterPath()
            for i, value in enumerate(self.reference):
                p = self._point((i + 0.5) / SAMPLE_COUNT, value or 0.0)
                path.moveTo(p) if i == 0 else path.lineTo(p)
            painter.drawPath(path)
        if self.keys:
            painter.setPen(QPen(pal.text().color(), 2))
            path = QPainterPath()
            for i in range(SAMPLE_COUNT + 1):
                phase = min(i / SAMPLE_COUNT, 0.9999)
                p = self._point(phase, environments.evaluate_curve(self.keys, phase))
                path.moveTo(p) if i == 0 else path.lineTo(p)
            painter.drawPath(path)
        painter.setPen(pal.text().color())
        painter.drawText(QRectF(r.left() + 4, r.top(), 200, 16), f"{self._top():.3g}")
        if not self.keys:
            painter.drawText(r, Qt.AlignmentFlag.AlignCenter,
                             "Not set: the analytic sky" if self.reference else "Not set")

    # ---- editing (each change emits edited) ---------------------------------------------------
    def _sorted(self) -> None:
        chosen = self.keys[self.selected] if self.selected is not None else None
        self.keys.sort(key=lambda k: k[0])
        if chosen is not None:
            self.selected = next(i for i, k in enumerate(self.keys) if k is chosen)

    def add_key(self, phase: float, value=None) -> int:
        phase = round(min(max(phase, 0.0), 0.999), 4)
        if any(abs(k[0] - phase) < 1e-4 for k in self.keys):
            phase = round(min(phase + 0.001, 0.999), 4)
        key = [phase, copy.deepcopy(value) if value is not None else self.value_at(phase)]
        self.keys.append(key)
        self.selected = len(self.keys) - 1
        self._sorted()
        self.edited.emit(copy.deepcopy(self.keys))
        self.update()
        return self.selected

    def move_key(self, index: int, phase: float, value=None) -> None:
        self.selected = index
        key = self.keys[index]
        key[0] = round(min(max(phase, 0.0), 0.999), 4)
        if value is not None:
            key[1] = [round(c, 4) for c in value] if isinstance(value, list) else round(value, 4)
        self._sorted()
        self.edited.emit(copy.deepcopy(self.keys))
        self.update()

    def remove_key(self, index: int) -> None:
        del self.keys[index]
        self.selected = None
        self.edited.emit(copy.deepcopy(self.keys))
        self.update()

    def mousePressEvent(self, e: QMouseEvent) -> None:
        self.setFocus()
        self.selected = self._key_near(e.position())
        self._dragging = self.selected is not None
        self.update()

    def mouseMoveEvent(self, e: QMouseEvent) -> None:
        if not self._dragging or self.selected is None:
            return
        pos = e.position()
        self.move_key(self.selected, self._phase_at(pos.x()),
                      None if self.colour else self._value_at_y(pos.y()))

    def mouseReleaseEvent(self, _e: QMouseEvent) -> None:
        self._dragging = False

    def mouseDoubleClickEvent(self, e: QMouseEvent) -> None:
        index = self._key_near(e.position())
        if index is None:
            self.add_key(self._phase_at(e.position().x()))
            return
        value = self.keys[index][1]
        if self.colour:
            chosen = QColorDialog.getColor(_qcolor(value), self, LABELS.get(self.name, ""))
            if chosen.isValid():
                self.move_key(index, self.keys[index][0],
                              [chosen.redF(), chosen.greenF(), chosen.blueF()])
        else:
            number, ok = QInputDialog.getDouble(self, LABELS.get(self.name, ""), "Value",
                                                float(value), 0.0, 1e6, 4)
            if ok:
                self.move_key(index, self.keys[index][0], number)

    def keyPressEvent(self, e: QKeyEvent) -> None:
        if e.key() in (Qt.Key.Key_Delete, Qt.Key.Key_Backspace) and self.selected is not None:
            self.remove_key(self.selected)
        else:
            super().keyPressEvent(e)


class EnvironmentPanel(QWidget):
    phase_previewed = Signal(float)  # the time of day to preview (the viewport's sun follows)
    season_previewed = Signal(float)  # how far into summer (1) or winter (-1) to preview

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.project: Project | None = None
        self.env: dict[str, Any] | None = None
        self._refreshing = False

        self.chooser = QComboBox()
        self.chooser.currentIndexChanged.connect(lambda _i: self.refresh())
        self.new_button = QPushButton("New")
        self.new_button.clicked.connect(lambda: self.new_environment())
        self.duplicate_button = QPushButton("Duplicate")
        self.duplicate_button.clicked.connect(lambda: self.duplicate())
        self.delete_button = QPushButton("Delete")
        self.delete_button.clicked.connect(lambda: self.delete_environment())
        top = QHBoxLayout()
        top.addWidget(self.chooser, 1)
        for b in (self.new_button, self.duplicate_button, self.delete_button):
            top.addWidget(b)
        self.name = QLineEdit()
        self.name.editingFinished.connect(self._name_edited)

        self.curves = QListWidget()
        self.curves.setMaximumHeight(170)
        for group, names in GROUPS.items():
            header = QListWidgetItem(group)
            header.setFlags(Qt.ItemFlag.NoItemFlags)
            font = header.font()
            font.setBold(True)
            header.setFont(font)
            self.curves.addItem(header)
            for name in names:
                item = QListWidgetItem(LABELS[name])
                item.setData(NAME_ROLE, name)
                self.curves.addItem(item)
        self.curves.currentItemChanged.connect(lambda *_a: self.show_curve())
        self.view = CurveView()
        self.view.edited.connect(self._keys_edited)
        self.from_analytic = QPushButton("Start from the Analytic Sky")
        self.from_analytic.setToolTip("Keys that follow the analytic sky through the day, to "
                                      "change from there")
        self.from_analytic.clicked.connect(self.start_from_analytic)
        self.clear = QPushButton("Clear")
        self.clear.setToolTip("No keys: the analytic sky again (or the scene's own colours)")
        self.clear.clicked.connect(lambda: self.set_curve(self.current_curve(), []))
        curve_buttons = QHBoxLayout()
        curve_buttons.addWidget(self.from_analytic)
        curve_buttons.addWidget(self.clear)
        curve_buttons.addStretch()

        settings = QFormLayout()
        self.settings: dict[str, QDoubleSpinBox] = {}
        for key, (label, default, low, high, step, decimals) in SETTINGS.items():
            box = QDoubleSpinBox()
            box.setRange(low, high)
            box.setSingleStep(step)
            box.setDecimals(decimals)
            box.setValue(default)
            box.valueChanged.connect(lambda v, k=key: self.set_setting(k, v))
            settings.addRow(label, box)
            self.settings[key] = box
        self.inside = QPushButton("Inside-World Atmosphere")
        self.inside.setToolTip("For hollow worlds and habitats: the far side of the world seen "
                               "through haze, the same at every height (sets the fog)")
        self.inside.clicked.connect(self.inside_atmosphere)
        settings.addRow("Preset", self.inside)

        # Seasons: a tint for each light at the depth of winter and of summer.
        seasons = QGridLayout()
        seasons.addWidget(QLabel("<b>Seasons</b>"), 0, 0)
        self.tints: dict[tuple[str, str], QPushButton] = {}
        for column, (season, season_label) in enumerate(SEASONS.items(), start=1):
            seasons.addWidget(QLabel(season_label), 0, column)
            for row, (key, label) in enumerate(TINTS.items(), start=1):
                if column == 1:
                    seasons.addWidget(QLabel(label), row, 0)
                swatch = QPushButton()
                swatch.setFixedSize(54, 20)
                swatch.setToolTip(f"{label} in {season_label.lower()}: click to choose a tint; "
                                  "white changes nothing")
                swatch.clicked.connect(lambda _c=False, s=season, k=key: self.choose_tint(s, k))
                seasons.addWidget(swatch, row, column)
                self.tints[(season, key)] = swatch
        self.season_preset = QPushButton("Colder Winter, Warmer Summer")
        self.season_preset.setToolTip("Bluer light and sky in winter, warmer in summer")
        self.season_preset.clicked.connect(self.colder_winter)
        self.clear_seasons = QPushButton("No Tints")
        self.clear_seasons.setToolTip("The same colours all year")
        self.clear_seasons.clicked.connect(self.no_season_tints)
        season_buttons = QHBoxLayout()
        season_buttons.addWidget(self.season_preset)
        season_buttons.addWidget(self.clear_seasons)
        season_buttons.addStretch()

        self.preview = QSlider(Qt.Orientation.Horizontal)
        self.preview.setRange(0, 999)
        self.preview.setValue(250)
        self.preview.setToolTip("Preview a time of day in the viewport")
        self.preview.valueChanged.connect(lambda v: self.preview_phase(v / 1000.0))
        self.preview_label = QLabel()
        preview_row = QHBoxLayout()
        preview_row.addWidget(QLabel("Time of day"))
        preview_row.addWidget(self.preview, 1)
        preview_row.addWidget(self.preview_label)
        self.season = QSlider(Qt.Orientation.Horizontal)
        self.season.setRange(-100, 100)
        self.season.setValue(0)
        self.season.setToolTip("Preview a season in the viewport: winter to the left, summer to "
                               "the right")
        self.season.valueChanged.connect(lambda v: self.preview_season(v / 100.0))
        self.season_label = QLabel()
        season_row = QHBoxLayout()
        season_row.addWidget(QLabel("Season"))
        season_row.addWidget(self.season, 1)
        season_row.addWidget(self.season_label)
        self.problems = QLabel()
        self.problems.setWordWrap(True)
        self.problems.setStyleSheet("color: #d08030;")

        page = QWidget()
        layout = QVBoxLayout(page)
        layout.addLayout(top)
        form = QFormLayout()
        form.addRow("Name", self.name)
        layout.addLayout(form)
        layout.addWidget(self.curves)
        layout.addWidget(self.view, 1)
        layout.addLayout(curve_buttons)
        layout.addLayout(settings)
        layout.addLayout(seasons)
        layout.addLayout(season_buttons)
        layout.addLayout(preview_row)
        layout.addLayout(season_row)
        layout.addWidget(self.problems)
        # The panel fits its width (it scrolls up and down only).
        self.chooser.setSizeAdjustPolicy(
            QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)
        self.chooser.setMinimumContentsLength(8)
        for box in self.settings.values():
            box.setMaximumWidth(110)
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        scroll.setWidget(page)
        self.empty = QLabel("No project open.")
        self.empty.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.stack = QStackedLayout(self)
        self.stack.addWidget(self.empty)
        self.stack.addWidget(scroll)
        self._references: dict[str, list] = {}
        self.preview_season(0.0)
        self.set_project(None)

    # ---- the project -----------------------------------------------------------------------------
    def set_project(self, project: Project | None) -> None:
        if self.project is not None:
            self.project.environments_changed.disconnect(self._environments_changed)
        self.project = project
        self.stack.setCurrentIndex(0 if project is None else 1)
        if project is not None:
            project.environments_changed.connect(self._environments_changed)
        self._reload(keep=None)

    def _environments_changed(self, _environment_id: str) -> None:
        self._reload(keep=self.chooser.currentData())

    def _reload(self, keep: str | None) -> None:
        self.chooser.blockSignals(True)
        self.chooser.clear()
        if self.project is not None:
            for env in environments.load_all(self.project):
                self.chooser.addItem(env["name"], env["id"])
        index = self.chooser.findData(keep) if keep else -1
        self.chooser.setCurrentIndex(index if index >= 0 else (0 if self.chooser.count() else -1))
        self.chooser.blockSignals(False)
        if self.curves.currentItem() is None:
            self.curves.setCurrentRow(1)  # the first curve (row 0 is a heading)
        self.refresh()

    def select(self, environment_id: str) -> None:
        self.chooser.setCurrentIndex(max(self.chooser.findData(environment_id), 0))

    def current_curve(self) -> str:
        item = self.curves.currentItem()
        return item.data(NAME_ROLE) if item is not None and item.data(NAME_ROLE) else "zenith"

    def refresh(self) -> None:
        eid = self.chooser.currentData()
        self.env = environments.load(self.project, eid) if self.project and eid else None
        self._refreshing = True
        try:
            has = self.env is not None
            for w in (self.name, self.curves, self.view, self.from_analytic, self.clear,
                      self.duplicate_button, self.delete_button, self.inside,
                      self.season_preset, self.clear_seasons, *self.tints.values(),
                      *self.settings.values()):
                w.setEnabled(has)
            if not has:
                self.name.setText("")
                self.problems.setText("")
                return
            self.name.setText(self.env["name"])
            for key, box in self.settings.items():
                box.setValue(float(self.env["settings"].get(key, SETTINGS[key][1])))
            for (season, key), swatch in self.tints.items():
                rgb = environments.tint(self.env, season, key)
                swatch.setStyleSheet(f"background-color: {_qcolor(rgb).name()};")
            for i in range(self.curves.count()):
                item = self.curves.item(i)
                name = item.data(NAME_ROLE)
                if name:
                    font = item.font()
                    font.setItalic(name not in self.env["curves"])  # italic: not set
                    item.setFont(font)
            self.show_curve()
            self._check()
        finally:
            self._refreshing = False

    def _reference(self, name: str) -> list:
        if name not in self._references:
            self._references[name] = [
                environments.analytic_value(name, (i + 0.5) / SAMPLE_COUNT)
                for i in range(SAMPLE_COUNT)]
            if all(v is None for v in self._references[name]):
                self._references[name] = []
        return self._references[name]

    def show_curve(self) -> None:
        if self.env is None:
            return
        name = self.current_curve()
        self.view.show_curve(name, self.env["curves"].get(name, []), self._reference(name))
        self.from_analytic.setEnabled(bool(self._reference(name)))

    def _check(self) -> None:
        module = engine.module()
        if module is None or not hasattr(module, "Environment") or self.env is None:
            self.problems.setText("")
            return
        try:
            problems = module.Environment(json.dumps(self.env["curves"]),
                                          json.dumps(self.env["settings"])).problems()
        except ValueError as e:
            problems = [str(e)]
        self.problems.setText("\n".join(problems))

    # ---- changes (each one undoable) --------------------------------------------------------
    def _push(self, new: dict[str, Any], label: str, field: str) -> None:
        if self.project is None or self.env is None or new == self.env:
            return
        self.project.undo_stack.push(EnvironmentCommand(self.project, self.env, new, label,
                                                        field))

    def set_curve(self, name: str, keys: list) -> None:
        if self.env is None:
            return
        new = copy.deepcopy(self.env)
        if keys:
            new["curves"][name] = copy.deepcopy(keys)
        else:
            new["curves"].pop(name, None)
        self._push(new, LABELS.get(name, name).lower(), "curve " + name)

    def _keys_edited(self, keys: list) -> None:
        self.set_curve(self.view.name, keys)

    def start_from_analytic(self) -> None:
        name = self.current_curve()
        keys = environments.analytic_keys(name)
        if keys:
            self.set_curve(name, keys)

    def set_setting(self, key: str, value: float) -> None:
        if self._refreshing or self.env is None:
            return
        new = copy.deepcopy(self.env)
        new["settings"][key] = round(value, 4)
        self._push(new, SETTINGS[key][0].lower(), "setting " + key)

    def inside_atmosphere(self) -> None:
        if self.env is None:
            return
        new = copy.deepcopy(self.env)
        new["curves"].update(copy.deepcopy(environments.INSIDE_ATMOSPHERE["curves"]))
        new["settings"].update(environments.INSIDE_ATMOSPHERE["settings"])
        self._push(new, "inside-world atmosphere", "")

    def set_tint(self, season: str, key: str, rgb: list | None) -> None:
        """A season's tint for the sunlight, ambient light or sky; None (or white) removes it."""
        if self.env is None:
            return
        new = copy.deepcopy(self.env)
        tints = new["settings"].setdefault(season, {})
        if rgb is None or [round(c, 4) for c in rgb] == WHITE:
            tints.pop(key, None)
        else:
            tints[key] = [round(float(c), 4) for c in rgb]
        if not tints:
            new["settings"].pop(season, None)
        self._push(new, f"{SEASONS[season].lower()} {TINTS[key].lower()} tint",
                   f"tint {season} {key}")

    def choose_tint(self, season: str, key: str) -> None:
        if self.env is None:
            return
        chosen = QColorDialog.getColor(_qcolor(environments.tint(self.env, season, key)), self,
                                       f"{TINTS[key]} in {SEASONS[season].lower()}")
        if chosen.isValid():
            self.set_tint(season, key, [chosen.redF(), chosen.greenF(), chosen.blueF()])

    def colder_winter(self) -> None:
        if self.env is None:
            return
        new = copy.deepcopy(self.env)
        new["settings"].update(copy.deepcopy(environments.SEASON_PRESET))
        self._push(new, "season tints", "")

    def no_season_tints(self) -> None:
        if self.env is None:
            return
        new = copy.deepcopy(self.env)
        for season in SEASONS:
            new["settings"].pop(season, None)
        self._push(new, "no season tints", "")

    def _name_edited(self) -> None:
        name = self.name.text().strip()
        if self._refreshing or self.env is None or not name or name == self.env["name"]:
            return
        self._push({**copy.deepcopy(self.env), "name": name}, "name", "name")

    def new_environment(self, name: str | None = None) -> str | None:
        if self.project is None:
            return None
        if name is None:
            name, ok = QInputDialog.getText(self, "New Environment", "Name")
            if not ok or not name.strip():
                return None
        env = {"id": environments.new_id(self.project, name), "name": name.strip(),
               "curves": {}, "settings": {}}
        self.project.undo_stack.push(EnvironmentCommand(self.project, None, env, ""))
        self.select(env["id"])
        return env["id"]

    def duplicate(self) -> str | None:
        if self.project is None or self.env is None:
            return None
        name = f"{self.env['name']} copy"
        env = {**copy.deepcopy(self.env), "id": environments.new_id(self.project, name),
               "name": name}
        self.project.undo_stack.push(EnvironmentCommand(self.project, None, env, ""))
        self.select(env["id"])
        return env["id"]

    def delete_environment(self, confirm: bool = True) -> None:
        if self.project is None or self.env is None:
            return
        users = [e.name for e in self.project.entities()
                 if e.components.get("sky", {}).get("environment") == self.env["id"]]
        if users and confirm:
            answer = QMessageBox.question(
                self, "Delete Environment",
                f"{', '.join(users)} links to {self.env['name']}; its sky will be flat until "
                "the environment comes back (Undo brings it back). Delete it?")
            if answer != QMessageBox.StandardButton.Yes:
                return
        self.project.undo_stack.push(EnvironmentCommand(self.project, self.env, None, ""))

    # ---- the preview -----------------------------------------------------------------------------
    def preview_phase(self, phase: float) -> None:
        label = min(MARKS, key=lambda m: min(abs(phase - m[0]), 1 - abs(phase - m[0])))
        self.preview_label.setText(f"{phase:.2f} ({label[1].lower()})")
        self.view.set_preview(phase)
        self.phase_previewed.emit(phase)

    def preview_season(self, season: float) -> None:
        self.season_label.setText(environments.season_name(season))
        self.season_previewed.emit(season)
