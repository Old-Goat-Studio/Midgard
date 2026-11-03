# SPDX-License-Identifier: Apache-2.0
"""The Calendar dock's Weather tab (milestone 5.15): the project's weather types, and the
scene's weather, in four parts.

- Types: each weather type's look (clouds, darkness, light colour, how far you can see, what
  falls and how much, lightning), feel (wind, gusts, wetness, temperature) and sound (looping
  sounds, thunder).
- Tables: what weather comes where (biomes) and when (seasons), how much of the time, what
  follows what, and how long each lasts; or a controlled timetable, for inside worlds.
- Fronts: storms that travel across the world: how often, how fast, how big, how long.
- Forecast: the coming days' weather where the player starts, made by the engine as the game
  will make it, and how much of a year each weather is there.

The scene's weather is its own ("weather" in its settings: the engine uses it before its world
type's); the first change starts it as a copy of its world type's. Every change is a step on
the project's undo stack, and reaches a game playing under the editor at once (weather.put).
"""
import copy
from typing import Any

from PySide6.QtCore import QRectF, Qt, QTimer
from PySide6.QtGui import QColor, QPainter
from PySide6.QtWidgets import (
    QCheckBox,
    QColorDialog,
    QComboBox,
    QDoubleSpinBox,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QScrollArea,
    QSpinBox,
    QSplitter,
    QTableWidget,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from .. import calendars, sound_events, weather, world_types
from ..project import Project

ID_ROLE = Qt.ItemDataRole.UserRole
FALLS = {"none": "Nothing", "rain": "Rain", "snow": "Snow", "ash": "Ash", "sand": "Sand"}


def _spin(low: float, high: float, step: float, decimals: int = 1,
          suffix: str = "") -> QDoubleSpinBox:
    box = QDoubleSpinBox()
    box.setRange(low, high)
    box.setSingleStep(step)
    box.setDecimals(decimals)
    box.setKeyboardTracking(False)
    if suffix:
        box.setSuffix(suffix)
    return box


def _row(*widgets: QWidget) -> QWidget:
    w = QWidget()
    layout = QHBoxLayout(w)
    layout.setContentsMargins(0, 0, 0, 0)
    for x in widgets:
        layout.addWidget(x)
    return w


def _pair(value: Any, default: list[float]) -> list[float]:
    if isinstance(value, (list, tuple)) and len(value) == 2:
        return [float(value[0]), float(value[1])]
    if isinstance(value, (int, float)):
        return [float(value), float(value)]
    return list(default)


def weather_color(settings: dict[str, Any]) -> QColor:
    """A colour for a weather type, from what it does (for the forecast's strip)."""
    falls = settings.get("falls", "none")
    if falls == "rain":
        base = (70, 110, 190)
    elif falls == "snow":
        base = (235, 240, 250)
    elif falls == "sand":
        base = (210, 170, 110)
    elif falls == "ash":
        base = (90, 85, 80)
    else:
        clouds = float(settings.get("clouds", 0.0))
        sky, grey = (120, 180, 240), (170, 175, 185)
        base = tuple(int(s + (g - s) * clouds) for s, g in zip(sky, grey, strict=True))
    dark = 1.0 - 0.6 * float(settings.get("darkness", 0.0))
    if float(settings.get("lightning", 0.0)) > 0.0:
        dark *= 0.7
    return QColor(*(int(c * dark) for c in base))


class ForecastStrip(QWidget):
    """The coming days' spells, side by side in their colours, a tick each midnight."""

    def __init__(self):
        super().__init__()
        self.spells: list[dict[str, Any]] = []
        self.start = 0
        self.hours = 72.0
        self.hour = 3600
        self.hours_per_day = 24
        self.colors: dict[str, QColor] = {}
        self.names: dict[str, str] = {}
        self.setMinimumHeight(48)

    def show_spells(self, spells, start: int, hours: float, hour: int, hours_per_day: int,
                    types: list[dict[str, Any]]) -> None:
        self.spells, self.start, self.hours = spells, start, hours
        self.hour, self.hours_per_day = hour, hours_per_day
        self.colors = {t["id"]: weather_color(t["weather"]) for t in types}
        self.names = {t["id"]: t["name"] for t in types}
        self.update()

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        w, h = self.width(), self.height()
        p.fillRect(0, 0, w, h, QColor(40, 40, 44))
        span = max(self.hours * self.hour, 1.0)
        for s in self.spells:
            x0 = (s["start"] - self.start) / span * w
            x1 = (s["end"] - self.start) / span * w
            color = self.colors.get(s["weather"], QColor(60, 60, 60))
            p.fillRect(QRectF(x0, 0, max(x1 - x0, 1.0), h - 14), color)
            if s.get("front", -1) >= 0:  # a front: a band along the bottom
                p.fillRect(QRectF(x0, h - 20, max(x1 - x0, 1.0), 4), QColor(220, 60, 60))
            if x1 - x0 > 40:
                p.setPen(Qt.GlobalColor.black if color.lightness() > 130 else Qt.GlobalColor.white)
                p.drawText(QRectF(x0 + 3, 0, x1 - x0 - 4, h - 14),
                           Qt.AlignmentFlag.AlignVCenter,
                           self.names.get(s["weather"], s["weather"] or "none"))
        p.setPen(QColor(200, 200, 200))
        day = self.hours_per_day * self.hour
        for d in range(int(self.hours // self.hours_per_day) + 1):
            x = d * day / span * w
            p.drawLine(int(x), h - 14, int(x), h)
            p.drawText(int(x) + 3, h - 2, f"day {d + 1}")
        p.end()


class WeatherTab(QWidget):
    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.project: Project | None = None
        self.tabs = QTabWidget()
        self.tabs.addTab(self._types_page(), "Types")
        self.tabs.addTab(self._tables_page(), "Tables")
        self.tabs.addTab(self._fronts_page(), "Fronts")
        self.tabs.addTab(self._forecast_page(), "Forecast")
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.tabs)
        self._forecast_timer = QTimer(self, singleShot=True, interval=250)
        self._forecast_timer.timeout.connect(self.refresh_forecast)
        self.set_project(None)

    # ---- the project -------------------------------------------------------------------------
    def set_project(self, project: Project | None) -> None:
        if self.project is not None:
            self.project.weather_changed.disconnect(self._changed)
            self.project.scene_opened.disconnect(self._changed)
        self.project = project
        if project is not None:
            project.weather_changed.connect(self._changed)
            project.scene_opened.connect(self._changed)
        self.setEnabled(project is not None)
        self.refresh()

    def _changed(self, *_args) -> None:
        self.refresh()

    def refresh(self) -> None:
        self._refresh_types()
        self._refresh_tables()
        self._refresh_fronts()
        self._forecast_timer.start()

    def types(self) -> list[dict[str, Any]]:
        return weather.load_all(self.project) if self.project is not None else []

    def _events(self) -> list[dict[str, Any]]:
        return sound_events.load_all(self.project) if self.project is not None else []

    # ---- types -------------------------------------------------------------------------------
    def _types_page(self) -> QWidget:
        page = QWidget()
        self.type_list = QListWidget()
        self.type_list.currentItemChanged.connect(lambda *_: self._show_type())
        new = QPushButton("New")
        new.setToolTip("A new weather type (acid rain, sleet, a heatwave)")
        new.clicked.connect(self.new_type)
        self.delete_type_button = QPushButton("Delete")
        self.delete_type_button.clicked.connect(self.delete_type)
        left = QWidget()
        left_layout = QVBoxLayout(left)
        left_layout.setContentsMargins(0, 0, 0, 0)
        left_layout.addWidget(self.type_list, 1)
        left_layout.addWidget(_row(new, self.delete_type_button))
        self.type_scroll = QScrollArea()
        self.type_scroll.setWidgetResizable(True)
        self.type_form: QWidget | None = None
        split = QSplitter()
        split.addWidget(left)
        split.addWidget(self.type_scroll)
        split.setStretchFactor(1, 1)
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(split)
        return page

    def chosen_type(self) -> dict[str, Any] | None:
        current = self.type_list.currentItem()
        tid = current.data(ID_ROLE) if current is not None else None
        return next((t for t in self.types() if t["id"] == tid), None)

    def choose_type(self, type_id: str) -> None:
        for i in range(self.type_list.count()):
            if self.type_list.item(i).data(ID_ROLE) == type_id:
                self.type_list.setCurrentRow(i)

    def _refresh_types(self) -> None:
        keep = self.type_list.currentItem().data(ID_ROLE) if self.type_list.currentItem() else None
        self.type_list.blockSignals(True)
        self.type_list.clear()
        for t in self.types():
            item = QListWidgetItem(t["name"])
            item.setData(ID_ROLE, t["id"])
            self.type_list.addItem(item)
            if t["id"] == keep:
                self.type_list.setCurrentItem(item)
        if self.type_list.currentItem() is None and self.type_list.count():
            self.type_list.setCurrentRow(0)
        self.type_list.blockSignals(False)
        self._show_type()

    def new_type(self) -> None:
        name = "New weather"
        new = {"id": weather.new_id(self.project, name), "name": name,
               "weather": {"clouds": 0.5}}
        self.project.undo_stack.push(weather.WeatherCommand(self.project, None, new, ""))
        self.choose_type(new["id"])

    def delete_type(self) -> None:
        chosen = self.chosen_type()
        if chosen is not None:
            self.project.undo_stack.push(weather.WeatherCommand(self.project, chosen, None, ""))

    def change_type(self, item: dict[str, Any], key: str, value: Any, label: str) -> None:
        """One setting of a weather type (or its "name"), as a step; None removes it."""
        new = copy.deepcopy(item)
        if key == "name":
            new["name"] = value
        elif value is None:
            new["weather"].pop(key, None)
        else:
            new["weather"][key] = value
        if new != item:
            self.project.undo_stack.push(
                weather.WeatherCommand(self.project, item, new, label, key))

    def _show_type(self) -> None:
        if self.type_form is not None:  # (taken out first: setWidget would delete it at once,
            self.type_scroll.takeWidget()  # while a box on it may still be telling its change)
            self.type_form.deleteLater()
            self.type_form = None
        item = self.chosen_type() if self.project is not None else None
        self.delete_type_button.setEnabled(item is not None)
        if item is None:
            return
        self.type_form = self._type_form(item)
        self.type_scroll.setWidget(self.type_form)

    def _type_form(self, item: dict[str, Any]) -> QWidget:
        w = item["weather"]
        page = QWidget()
        form = QFormLayout(page)
        name = QLineEdit(item["name"])
        name.editingFinished.connect(
            lambda: self.change_type(item, "name", name.text().strip() or item["id"], "name"))
        form.addRow("Name", name)

        def percent(key: str, label: str, tip: str, default: float = 0.0) -> QDoubleSpinBox:
            box = _spin(0.0, 100.0, 5.0, 0, " %")
            box.setValue(float(w.get(key, default)) * 100.0)
            box.setToolTip(tip)
            box.valueChanged.connect(lambda v: self.change_type(item, key, round(v / 100.0, 4),
                                                                label.lower()))
            form.addRow(label, box)
            return box

        def number(key: str, label: str, low: float, high: float, step: float, suffix: str,
                   tip: str, decimals: int = 1) -> QDoubleSpinBox:
            box = _spin(low, high, step, decimals, suffix)
            box.setValue(float(w.get(key, 0.0)))
            box.setToolTip(tip)
            box.valueChanged.connect(lambda v: self.change_type(item, key, v, label.lower()))
            form.addRow(label, box)
            return box

        form.addRow(QLabel("<b>How it looks</b>"))
        self.clouds = percent("clouds", "Clouds", "How much of the sky is cloud (100: overcast)")
        self.darkness = percent("darkness", "Darkness",
                                "How much of the daylight it takes (a storm: 75)")
        self.tint = QPushButton()
        self.tint.setToolTip("The light's colour under it (white: as it is)")
        tint = w.get("tint", [1.0, 1.0, 1.0])
        r, g, b = (int(max(0.0, min(1.0, c)) * 255) for c in tint)
        self.tint.setStyleSheet(f"background: rgb({r}, {g}, {b});")
        self.tint.clicked.connect(lambda: self._pick_tint(item))
        form.addRow("Light colour", self.tint)
        self.visibility = number("visibility", "Can see", 0.0, 100000.0, 50.0, " m",
                                 "How far you can see through its fog, dust or snow (0: no limit)",
                                 0)
        self.falls = QComboBox()
        for key, label in FALLS.items():
            self.falls.addItem(label, key)
        self.falls.setCurrentIndex(max(self.falls.findData(w.get("falls", "none")), 0))
        self.falls.currentIndexChanged.connect(
            lambda _i: self.change_type(item, "falls", self.falls.currentData(), "falls"))
        form.addRow("Falls", self.falls)
        self.amount = percent("amount", "How much",
                              "How much falls (100: a downpour)", 0.5 if w.get("falls") else 0.0)
        self.amount.setEnabled(w.get("falls", "none") != "none")
        self.lightning = number("lightning", "Lightning", 0.0, 60.0, 1.0, " a minute",
                                "Flashes of lightning a minute (0: none)")
        form.addRow(QLabel("<b>How it feels</b>"))
        self.wind = number("wind", "Wind", 0.0, 100.0, 1.0, " m/s",
                           "Wind speed (5: a breeze, 15: a gale, 30: a hurricane)")
        self.gusts = percent("gusts", "Gusts", "How gusty the wind is (0: steady)")
        self.wetness = percent("wetness", "Wetness", "How wet things out in it get")
        self.temperature = number("temperature", "Temperature", -100.0, 100.0, 1.0, " °C",
                                  "How much warmer (or colder: below 0) than usual")
        form.addRow(QLabel("<b>How it sounds</b>"))
        events = self._events()
        sounds = w.get("sounds", [])
        self.sounds_table = QTableWidget(len(sounds), 2)
        self.sounds_table.setHorizontalHeaderLabels(["Sound", "Volume"])
        self.sounds_table.horizontalHeader().setStretchLastSection(True)
        for row, sound in enumerate(sounds):
            event = sound if isinstance(sound, str) else sound.get("event", "")
            volume = 1.0 if isinstance(sound, str) else float(sound.get("volume", 1.0))
            combo = self._event_combo(events, event)
            combo.currentIndexChanged.connect(
                lambda _i, r=row, c=combo: self._set_sound(item, r, "event", c.currentData()))
            self.sounds_table.setCellWidget(row, 0, combo)
            vol = _spin(0.0, 4.0, 0.1, 2)
            vol.setValue(volume)
            vol.valueChanged.connect(lambda v, r=row: self._set_sound(item, r, "volume", v))
            self.sounds_table.setCellWidget(row, 1, vol)
        self.sounds_table.setMinimumHeight(60 + 28 * len(sounds))
        form.addRow("Sounds", self.sounds_table)
        add_sound = QPushButton("Add Sound")
        add_sound.setToolTip("A sound event that loops while this weather lasts")
        add_sound.setEnabled(bool(events))
        add_sound.clicked.connect(lambda: self.change_type(
            item, "sounds", [*self._sound_rows(item), {"event": events[0]["id"], "volume": 1.0}],
            "sounds"))
        remove_sound = QPushButton("Remove")
        remove_sound.clicked.connect(lambda: self._remove_sound(item))
        form.addRow("", _row(add_sound, remove_sound))
        self.thunder = self._event_combo(events, w.get("thunder", ""), "(none)")
        self.thunder.setToolTip("Played a moment after each flash of lightning")
        self.thunder.currentIndexChanged.connect(lambda _i: self.change_type(
            item, "thunder", self.thunder.currentData() or None, "thunder"))
        form.addRow("Thunder", self.thunder)
        problems = weather.problems_of(w, events)
        if problems:
            note = QLabel("\n".join(f"• {p[0].upper()}{p[1:]}." for p in problems))
            note.setWordWrap(True)
            note.setStyleSheet("color: #d08030;")
            form.addRow(note)
        return page

    def _event_combo(self, events, current: str, empty: str | None = None) -> QComboBox:
        combo = QComboBox()
        if empty is not None:
            combo.addItem(empty, "")
        for e in events:
            combo.addItem(e["name"], e["id"])
        if current and combo.findData(current) < 0:
            combo.addItem(f"{current} (missing)", current)
        combo.setCurrentIndex(max(combo.findData(current), 0))
        return combo

    def _sound_rows(self, item) -> list[dict[str, Any]]:
        return [{"event": s, "volume": 1.0} if isinstance(s, str) else dict(s)
                for s in item["weather"].get("sounds", [])]

    def _set_sound(self, item, row: int, key: str, value: Any) -> None:
        rows = self._sound_rows(item)
        rows[row][key] = value
        self.change_type(item, "sounds", rows, "sounds")

    def _remove_sound(self, item) -> None:
        rows = self._sound_rows(item)
        row = self.sounds_table.currentRow()
        if rows:
            rows.pop(row if 0 <= row < len(rows) else -1)
            self.change_type(item, "sounds", rows or None, "sounds")

    def _pick_tint(self, item) -> None:
        tint = item["weather"].get("tint", [1.0, 1.0, 1.0])
        color = QColorDialog.getColor(QColor.fromRgbF(*tint), self, "The light's colour")
        if color.isValid():
            value = [round(color.redF(), 3), round(color.greenF(), 3), round(color.blueF(), 3)]
            self.change_type(item, "tint", None if value == [1.0, 1.0, 1.0] else value, "colour")

    # ---- the scene's weather settings --------------------------------------------------------
    def settings(self) -> dict[str, Any] | None:
        return weather.scene_settings(self.project) if self.project is not None else None

    def change_settings(self, new: dict[str, Any] | None, label: str, field: str = "") -> None:
        old = weather.own_settings(self.project)
        if new != old:
            self.project.undo_stack.push(
                weather.SceneWeatherCommand(self.project, old, new, label, field))

    def edit_settings(self, edit, label: str, field: str = "") -> None:
        """Change a copy of the scene's settings (starting them if it has none) with `edit`."""
        new = copy.deepcopy(self.settings() or weather.STARTING_SETTINGS)
        edit(new)
        self.change_settings(new, label, field)

    def _biomes(self) -> list[str]:
        settings = world_types.resolved_settings(self.project, self.project.scene_settings())
        terrain = settings.get("terrain")
        return [b.get("name", "") for b in (terrain or {}).get("biomes", []) if b.get("name")]

    def _seasons(self) -> list[str]:
        for cal in calendars.load_all(self.project):
            if cal.get("standard"):
                return [s.get("name") or f"Season {i + 1}"
                        for i, s in enumerate(cal.get("seasons", []))]
        return []

    def _tables_page(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 0, 0, 0)
        self.where = QLabel()
        self.where.setWordWrap(True)
        layout.addWidget(self.where)
        self.has_weather = QCheckBox("This scene has weather")
        self.has_weather.setToolTip("Untick for no weather of its own (scripts can still force "
                                    "some)")
        self.has_weather.toggled.connect(self._weather_toggled)
        layout.addWidget(self.has_weather)
        self.tables_scroll = QScrollArea()
        self.tables_scroll.setWidgetResizable(True)
        layout.addWidget(self.tables_scroll, 1)
        self.tables_form: QWidget | None = None
        self.table_index = 0
        return page

    def _weather_toggled(self, on: bool) -> None:
        if on:
            self.change_settings(copy.deepcopy(self.settings() or weather.STARTING_SETTINGS),
                                 "on")
        else:
            self.change_settings({}, "off")

    def _refresh_tables(self) -> None:
        if self.tables_form is not None:
            self.tables_scroll.takeWidget()
            self.tables_form.deleteLater()
            self.tables_form = None
        if self.project is None:
            return
        own = weather.own_settings(self.project)
        from_world = weather.world_type_settings(self.project)
        settings = self.settings()
        if own is not None:
            self.where.setText("This scene's weather.")
        elif from_world is not None:
            self.where.setText("This scene's weather comes from its world type. A change here "
                               "starts the scene's own, as a copy.")
        else:
            self.where.setText("This scene has no weather yet. Tick below to start some.")
        active = bool(settings) and bool(settings.get("tables") or settings.get("controlled")
                                         or (settings.get("fronts") or {}).get("weather"))
        self.has_weather.blockSignals(True)
        self.has_weather.setChecked(active)
        self.has_weather.blockSignals(False)
        if not active:
            return
        self.tables_form = self._settings_form(settings)
        self.tables_scroll.setWidget(self.tables_form)

    def _settings_form(self, s: dict[str, Any]) -> QWidget:
        page = QWidget()
        form = QFormLayout(page)
        types = self.types()
        cell = _spin(0.1, 10000.0, 0.5, 1, " km")
        cell.setValue(float(s.get("cell", 4000.0)) / 1000.0)
        cell.setToolTip("How big a patch of weather is: each has its own")
        cell.valueChanged.connect(lambda v: self.edit_settings(
            lambda n: n.__setitem__("cell", round(v * 1000.0, 1)), "cell size", "cell"))
        form.addRow("Weather cells", cell)

        controlled = s.get("controlled") or []
        self.controlled = QCheckBox("Controlled: a timetable that repeats (inside worlds)")
        self.controlled.setChecked(bool(controlled))
        self.controlled.toggled.connect(lambda on: self.edit_settings(
            lambda n: n.__setitem__("controlled", [{"weather": types[0]["id"], "hours": 24.0}]
                                    if on and types else []), "controlled"))
        form.addRow(self.controlled)
        if controlled:
            self.timetable = QTableWidget(len(controlled), 2)
            self.timetable.setHorizontalHeaderLabels(["Weather", "Hours"])
            self.timetable.horizontalHeader().setStretchLastSection(True)
            for row, step in enumerate(controlled):
                combo = self._type_combo(types, step.get("weather", ""))
                combo.currentIndexChanged.connect(
                    lambda _i, r=row, c=combo: self._set_step(r, "weather", c.currentData()))
                self.timetable.setCellWidget(row, 0, combo)
                hours = _spin(0.25, 10000.0, 1.0, 2, " h")
                hours.setValue(float(step.get("hours", 1.0)))
                hours.valueChanged.connect(lambda v, r=row: self._set_step(r, "hours", v))
                self.timetable.setCellWidget(row, 1, hours)
            self.timetable.setMinimumHeight(60 + 28 * len(controlled))
            form.addRow("Timetable", self.timetable)
            add = QPushButton("Add")
            add.clicked.connect(lambda: self.edit_settings(lambda n: n["controlled"].append(
                {"weather": types[0]["id"], "hours": 4.0}), "timetable"))
            remove = QPushButton("Remove")
            remove.clicked.connect(lambda: self.edit_settings(
                lambda n: len(n["controlled"]) > 1 and n["controlled"].pop(), "timetable"))
            form.addRow("", _row(add, remove))
            return page

        # Tables: where and when, how much of the time, what follows what.
        tables = s.get("tables") or []
        self.table_index = min(self.table_index, max(len(tables) - 1, 0))
        self.table_chooser = QComboBox()
        for t in tables:
            self.table_chooser.addItem(t.get("name") or "(unnamed)")
        self.table_chooser.setCurrentIndex(self.table_index)
        self.table_chooser.currentIndexChanged.connect(self._table_chosen)
        add_table = QPushButton("New Table")
        add_table.clicked.connect(self.add_table)
        remove_table = QPushButton("Delete")
        remove_table.setEnabled(bool(tables))
        remove_table.clicked.connect(self.delete_table)
        form.addRow("Table", _row(self.table_chooser, add_table, remove_table))
        if tables:
            self._table_rows(form, tables[self.table_index], types)

        # How long each lasts.
        lasts = s.get("lasts") or {}
        form.addRow(QLabel("<b>How long each lasts</b> (hours, from least to most)"))
        for key, label in [("default", "Any other"), *[(t["id"], t["name"]) for t in types
                                                       if t["id"] in lasts]]:
            least, most = _pair(lasts.get(key), [2.0, 8.0])
            a, b = _spin(0.25, 10000.0, 0.5, 2, " h"), _spin(0.25, 10000.0, 0.5, 2, " h")
            a.setValue(least)
            b.setValue(most)
            a.valueChanged.connect(lambda v, k=key, o=b: self._set_lasts(k, [v, o.value()]))
            b.valueChanged.connect(lambda v, k=key, o=a: self._set_lasts(k, [o.value(), v]))
            form.addRow(label, _row(a, QLabel("to"), b))
        add_lasts = QComboBox()
        add_lasts.addItem("Set how long one lasts…", "")
        for t in types:
            if t["id"] not in lasts:
                add_lasts.addItem(t["name"], t["id"])
        add_lasts.currentIndexChanged.connect(
            lambda _i: add_lasts.currentData() and self._set_lasts(
                add_lasts.currentData(), _pair(lasts.get("default"), [2.0, 8.0])))
        form.addRow("", add_lasts)
        return page

    def _table_rows(self, form: QFormLayout, table: dict[str, Any], types) -> None:
        index = self.table_index
        name = QLineEdit(table.get("name", ""))
        name.editingFinished.connect(lambda: self._set_table(index, "name", name.text().strip(),
                                                             "table name"))
        form.addRow("Name", name)
        for key, label, choices, tip in (
                ("biomes", "In biomes", self._biomes(),
                 "Where it applies (none ticked: everywhere)"),
                ("seasons", "In seasons", self._seasons(),
                 "When it applies (none ticked: all year)")):
            chosen = table.get(key) or []
            box = QListWidget()
            box.setToolTip(tip)
            values = choices if key == "biomes" else list(range(len(choices)))
            for value, text in zip(values, choices, strict=True):
                item = QListWidgetItem(text)
                item.setData(ID_ROLE, value)
                item.setFlags(item.flags() | Qt.ItemFlag.ItemIsUserCheckable)
                item.setCheckState(Qt.CheckState.Checked if value in chosen
                                   else Qt.CheckState.Unchecked)
                box.addItem(item)
            box.setMaximumHeight(22 * max(len(choices), 1) + 8)
            box.itemChanged.connect(lambda _i, k=key, b=box: self._set_table(
                index, k, [b.item(r).data(ID_ROLE) for r in range(b.count())
                           if b.item(r).checkState() == Qt.CheckState.Checked], k))
            form.addRow(label, box if choices else QLabel("(the world has none)"))
        form.addRow(QLabel("<b>How much of the time</b> (by weight: 3 is three times 1)"))
        weights = table.get("weather") or {}
        for t in types:
            box = _spin(0.0, 1000.0, 1.0, 1)
            box.setValue(float(weights.get(t["id"], 0.0)))
            box.valueChanged.connect(lambda v, tid=t["id"]: self._set_weight(index, tid, v))
            form.addRow(t["name"], box)
        after = table.get("after") or {}
        rows = [(a, b, w) for a, nexts in after.items() for b, w in (nexts or {}).items()]
        form.addRow(QLabel("<b>What comes after</b> (instead of the weights above)"))
        self.after_table = QTableWidget(len(rows), 3)
        self.after_table.setHorizontalHeaderLabels(["After", "Then", "How often"])
        self.after_table.horizontalHeader().setStretchLastSection(True)
        for r, (a, b, w) in enumerate(rows):
            for col, value in ((0, a), (1, b)):
                combo = self._type_combo(types, value)
                combo.currentIndexChanged.connect(lambda _i: self._save_after(index))
                self.after_table.setCellWidget(r, col, combo)
            weight = _spin(0.0, 1000.0, 1.0, 1)
            weight.setValue(float(w))
            weight.valueChanged.connect(lambda _v: self._save_after(index))
            self.after_table.setCellWidget(r, 2, weight)
        self.after_table.setMinimumHeight(60 + 28 * len(rows))
        form.addRow(self.after_table)
        add = QPushButton("Add")
        add.setToolTip("After one weather, what may follow (a thunderstorm clearing through rain)")
        add.setEnabled(bool(types))
        add.clicked.connect(lambda: self._add_after(index, types))
        remove = QPushButton("Remove")
        remove.clicked.connect(lambda: self._remove_after(index))
        form.addRow("", _row(add, remove))

    def _type_combo(self, types, current: str) -> QComboBox:
        combo = QComboBox()
        for t in types:
            combo.addItem(t["name"], t["id"])
        if current and combo.findData(current) < 0:
            combo.addItem(f"{current} (missing)", current)
        combo.setCurrentIndex(max(combo.findData(current), 0))
        return combo

    def _table_chosen(self, index: int) -> None:
        self.table_index = index
        self._refresh_tables()

    def add_table(self) -> None:
        def edit(n):
            n.setdefault("tables", []).append(
                {"name": f"Table {len(n['tables']) + 1}", "biomes": [], "seasons": [],
                 "weather": {"clear": 1}})
            self.table_index = len(n["tables"]) - 1
        self.edit_settings(edit, "add table")

    def delete_table(self) -> None:
        index = self.table_index
        self.edit_settings(lambda n: n["tables"].pop(index), "delete table")

    def _set_table(self, index: int, key: str, value: Any, label: str) -> None:
        self.edit_settings(lambda n: n["tables"][index].__setitem__(key, value), label,
                           f"table{index}.{key}")

    def _set_weight(self, index: int, type_id: str, value: float) -> None:
        def edit(n):
            weights = n["tables"][index].setdefault("weather", {})
            if value > 0.0:
                weights[type_id] = value
            else:
                weights.pop(type_id, None)
        self.edit_settings(edit, "how much of the time", f"table{index}.{type_id}")

    def _after_rows(self) -> dict[str, dict[str, float]]:
        out: dict[str, dict[str, float]] = {}
        for r in range(self.after_table.rowCount()):
            a = self.after_table.cellWidget(r, 0).currentData()
            b = self.after_table.cellWidget(r, 1).currentData()
            out.setdefault(a, {})[b] = self.after_table.cellWidget(r, 2).value()
        return out

    def _save_after(self, index: int) -> None:
        rows = self._after_rows()
        self.edit_settings(lambda n: n["tables"][index].__setitem__("after", rows),
                           "what comes after")

    def _add_after(self, index: int, types) -> None:
        rows = self._after_rows()
        first = types[0]["id"]
        rows.setdefault(first, {}).setdefault(types[-1]["id"], 1.0)
        self.edit_settings(lambda n: n["tables"][index].__setitem__("after", rows),
                           "what comes after")

    def _remove_after(self, index: int) -> None:
        row = self.after_table.currentRow()
        if self.after_table.rowCount() == 0:
            return
        self.after_table.removeRow(row if row >= 0 else self.after_table.rowCount() - 1)
        self._save_after(index)

    def _set_lasts(self, key: str, value: list[float]) -> None:
        least, most = sorted(value)
        self.edit_settings(lambda n: n.setdefault("lasts", {}).__setitem__(key, [least, most]),
                           "how long it lasts", f"lasts.{key}")

    def _set_step(self, row: int, key: str, value: Any) -> None:
        self.edit_settings(lambda n: n["controlled"][row].__setitem__(key, value), "timetable",
                           f"controlled{row}.{key}")

    # ---- fronts ------------------------------------------------------------------------------
    def _fronts_page(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 0, 0, 0)
        self.fronts_on = QCheckBox("Fronts: weather that travels across the world")
        self.fronts_on.toggled.connect(self._fronts_toggled)
        layout.addWidget(self.fronts_on)
        self.fronts_scroll = QScrollArea()
        self.fronts_scroll.setWidgetResizable(True)
        layout.addWidget(self.fronts_scroll, 1)
        self.fronts_form: QWidget | None = None
        return page

    def _fronts_toggled(self, on: bool) -> None:
        def edit(n):
            if on:
                n["fronts"] = {"every": [24, 72], "speed": [5, 15], "radius": [3, 8],
                               "lasts": [12, 48], "weather": {"thunderstorm": 1, "rain": 2}}
            else:
                n.pop("fronts", None)
        self.edit_settings(edit, "fronts")

    def _refresh_fronts(self) -> None:
        if self.fronts_form is not None:
            self.fronts_scroll.takeWidget()
            self.fronts_form.deleteLater()
            self.fronts_form = None
        s = self.settings() if self.project is not None else None
        fronts = (s or {}).get("fronts") or {}
        on = bool(fronts.get("weather")) and not (s or {}).get("controlled")
        self.fronts_on.blockSignals(True)
        self.fronts_on.setChecked(on)
        self.fronts_on.setEnabled(self.project is not None and not (s or {}).get("controlled"))
        self.fronts_on.blockSignals(False)
        if not on:
            return
        page = QWidget()
        form = QFormLayout(page)
        for key, label, default, low, high, step, suffix, tip in (
                ("every", "A new one every", [24, 72], 1.0, 10000.0, 1.0, " h",
                 "Hours from one front starting to the next"),
                ("speed", "Moving at", [5, 15], 0.0, 300.0, 1.0, " m/s",
                 "How fast fronts travel"),
                ("radius", "Size", [3, 8], 0.5, 1000.0, 0.5, " cells",
                 "How far a front reaches from its middle, in weather cells"),
                ("lasts", "Lasting", [12, 48], 0.25, 10000.0, 1.0, " h",
                 "How long a front lasts")):
            least, most = _pair(fronts.get(key), default)
            a, b = _spin(low, high, step, 1, suffix), _spin(low, high, step, 1, suffix)
            a.setValue(least)
            b.setValue(most)
            a.setToolTip(tip)
            b.setToolTip(tip)
            a.valueChanged.connect(lambda v, k=key, o=b: self._set_front(k, [v, o.value()]))
            b.valueChanged.connect(lambda v, k=key, o=a: self._set_front(k, [o.value(), v]))
            form.addRow(label, _row(a, QLabel("to"), b))
        form.addRow(QLabel("<b>Their weather</b> (by weight)"))
        weights = fronts.get("weather") or {}
        for t in self.types():
            box = _spin(0.0, 1000.0, 1.0, 1)
            box.setValue(float(weights.get(t["id"], 0.0)))
            box.valueChanged.connect(lambda v, tid=t["id"]: self._set_front_weight(tid, v))
            form.addRow(t["name"], box)
        self.fronts_form = page
        self.fronts_scroll.setWidget(page)

    def _set_front(self, key: str, value: list[float]) -> None:
        least, most = sorted(value)
        self.edit_settings(lambda n: n["fronts"].__setitem__(key, [least, most]), "fronts",
                           f"fronts.{key}")

    def _set_front_weight(self, type_id: str, value: float) -> None:
        def edit(n):
            weights = n["fronts"].setdefault("weather", {})
            if value > 0.0:
                weights[type_id] = value
            else:
                weights.pop(type_id, None)
        self.edit_settings(edit, "fronts' weather", f"fronts.{type_id}")

    # ---- the forecast ------------------------------------------------------------------------
    def _forecast_page(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        top = QHBoxLayout()
        self.forecast_days = QSpinBox()
        self.forecast_days.setRange(1, 30)
        self.forecast_days.setValue(3)
        self.forecast_days.setSuffix(" days")
        self.forecast_days.valueChanged.connect(lambda _v: self.refresh_forecast())
        self.forecast_from = QSpinBox()
        self.forecast_from.setRange(0, 100000)
        self.forecast_from.setPrefix("from day ")
        self.forecast_from.setToolTip("Days after the game starts")
        self.forecast_from.valueChanged.connect(lambda _v: self.refresh_forecast())
        top.addWidget(QLabel("The weather where the player starts:"))
        top.addWidget(self.forecast_days)
        top.addWidget(self.forecast_from)
        top.addStretch(1)
        layout.addLayout(top)
        self.strip = ForecastStrip()
        layout.addWidget(self.strip)
        self.forecast_text = QLabel()
        self.forecast_text.setWordWrap(True)
        self.forecast_text.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        layout.addWidget(self.forecast_text)
        layout.addStretch(1)
        return page

    def _player_spot(self) -> tuple[float, float, float]:
        for e in self.project.entities():
            if "player" in e.components:
                p = e.components.get("transform", {}).get("position", [0.0, 0.0, 0.0])
                return float(p[0]), float(p[1]), float(p[2])
        return 0.0, 0.0, 0.0

    def refresh_forecast(self) -> None:
        if self.project is None:
            self.forecast_text.setText("")
            return
        types = self.types()
        days = self.forecast_days.value()
        try:
            first = weather.forecast(self.project, hours=1.0, at=self._player_spot())
            if first is None:
                self.forecast_text.setText("The forecast needs the engine (it isn't built here).")
                return
            hour = first["seconds_per_hour"]
            hours_per_day = self._hours_per_day()
            start = first["start"] + self.forecast_from.value() * hours_per_day * hour
            hours = float(days * hours_per_day)
            f = weather.forecast(self.project, hours=hours, at=self._player_spot(), start=start)
        except (RuntimeError, ValueError) as e:
            self.forecast_text.setText(f"The weather settings can't be read: {e}")
            return
        if not f["spells"]:
            self.strip.show_spells([], 0, 1.0, 3600, 24, types)
            self.forecast_text.setText("No weather here: the scene has no weather settings.")
            return
        self.strip.show_spells(f["spells"], f["start"], hours, f["seconds_per_hour"],
                               hours_per_day, types)
        names = {t["id"]: t["name"] for t in types}
        lines = []
        for s in f["spells"][:12]:
            when = (s["start"] - f["start"]) / f["seconds_per_hour"]
            length = (s["end"] - s["start"]) / f["seconds_per_hour"]
            front = " (a front)" if s["front"] >= 0 else ""
            lines.append(f"{names.get(s['weather'], s['weather'] or 'none')}{front}: "
                         f"from hour {when:.1f}, for {length:.1f} h")
        shares = sorted(f["shares"].items(), key=lambda kv: -kv[1])
        year = ", ".join(f"{names.get(k, k or 'none')} {v * 100:.0f}%" for k, v in shares)
        biome = f" ({f['biome']})" if f["biome"] else ""
        self.forecast_text.setText(
            f"Weather cell {f['cell']}{biome}.\n" + "\n".join(lines) +
            f"\n\nOver a year here: {year}.")

    def _hours_per_day(self) -> int:
        for cal in calendars.load_all(self.project):
            if cal.get("standard"):
                return int(cal.get("hours_per_day", 24))
        return 24
