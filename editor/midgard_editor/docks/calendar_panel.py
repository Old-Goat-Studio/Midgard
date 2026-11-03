# SPDX-License-Identifier: Apache-2.0
"""The Calendar dock (roadmap §6, "Editor: the Calendar dock"; milestone 5.7).

The project's calendar templates: the length of a day and how long it lasts for the player,
daylight, the start date, months (and a leap day), weekdays, seasons, moons, and the ranges body
calendars are rolled from; one is the standard calendar. Below them, a preview of the chosen
date: the year strip (daylight through the year, coloured by season; click to pick a day), the
day bar (night, twilight and day, with sunrise and sunset), and the time-of-day scrubber, which
moves the viewport's sun and the running game's clock (time_scrubbed). A body preview rolls the
calendar a body would get from a seed. The cook checks' complaints show as you edit.

Every change is undoable; a number being dragged is one step in the History. The dates,
daylight and checks come from the engine (_midgard.Calendar), so the dock and the game agree.
"""
import copy
from typing import Any

from PySide6.QtCore import QPointF, QRectF, Qt, Signal
from PySide6.QtGui import QColor, QLinearGradient, QPainter, QPainterPath, QPen
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QHeaderView,
    QInputDialog,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPushButton,
    QSlider,
    QSpinBox,
    QTableWidget,
    QTableWidgetItem,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from .. import calendars
from ..calendars import CalendarCommand
from ..project import Project

SEASON_COLOURS = ["#5b7fa6", "#6fa35a", "#c9a227", "#b9693a", "#8a6bb0", "#4f9c95"]


def clock_text(seconds: float, seconds_per_hour: int) -> str:
    """Seconds into a day as hours and minutes (any length of hour)."""
    seconds = int(seconds)
    return f"{seconds // seconds_per_hour:02d}:{seconds % seconds_per_hour // 60:02d}"


# ---- the pictures --------------------------------------------------------------------------

class YearStrip(QWidget):
    """Daylight through the year (the curve), the seasons (coloured bands) and the chosen day
    (a line). Click or drag to choose a day."""
    day_picked = Signal(int)  # day of the year, 0-based

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.setMinimumHeight(56)
        self.daylight: list[float] = []
        self.seasons: list[int] = []
        self.day = 0
        self.setToolTip("Daylight through the year, coloured by season. Click to choose a day.")

    def set_year(self, daylight: list[float], seasons: list[int], day: int) -> None:
        self.daylight, self.seasons, self.day = daylight, seasons, day
        self.update()

    def _pick(self, x: float) -> None:
        if self.daylight:
            day = int(x / max(self.width(), 1) * len(self.daylight))
            self.day_picked.emit(min(max(day, 0), len(self.daylight) - 1))

    def mousePressEvent(self, e) -> None:
        self._pick(e.position().x())

    def mouseMoveEvent(self, e) -> None:
        if e.buttons() & Qt.MouseButton.LeftButton:
            self._pick(e.position().x())

    def paintEvent(self, _e) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        w, h, n = self.width(), self.height(), len(self.daylight)
        p.fillRect(self.rect(), QColor("#1d2128"))
        if not n:
            return
        step = w / n
        for i, season in enumerate(self.seasons):
            if season >= 0:
                colour = QColor(SEASON_COLOURS[season % len(SEASON_COLOURS)])
                colour.setAlpha(90)
                p.fillRect(QRectF(i * step, 0, step + 1, h), colour)
        path = QPainterPath()
        for i, d in enumerate(self.daylight):
            point = QPointF((i + 0.5) * step, h - d * h)
            path.moveTo(point) if i == 0 else path.lineTo(point)
        p.setPen(QPen(QColor("#f2d16b"), 2))
        p.drawPath(path)
        p.setPen(QPen(QColor("#ffffff"), 1))
        x = (self.day + 0.5) * step
        p.drawLine(QPointF(x, 0), QPointF(x, h))


class DayBar(QWidget):
    """One day from midnight to midnight: night, twilight and day, sunrise and sunset, and the
    time chosen (a line). Click or drag to choose a time."""
    time_picked = Signal(int)  # seconds into the day

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.setMinimumHeight(30)
        self.day_length, self.sunrise, self.sunset, self.twilight, self.time = 86400, 0, 0, 0, 0
        self.setToolTip("The day: night, twilight and daylight. Click to choose a time.")

    def set_day(self, day_length: int, sunrise: float, sunset: float, twilight: float,
                time: int) -> None:
        self.day_length, self.sunrise, self.sunset = day_length, sunrise, sunset
        self.twilight, self.time = twilight, time
        self.update()

    def _pick(self, x: float) -> None:
        t = int(x / max(self.width(), 1) * self.day_length)
        self.time_picked.emit(min(max(t, 0), self.day_length - 1))

    def mousePressEvent(self, e) -> None:
        self._pick(e.position().x())

    def mouseMoveEvent(self, e) -> None:
        if e.buttons() & Qt.MouseButton.LeftButton:
            self._pick(e.position().x())

    def paintEvent(self, _e) -> None:
        p = QPainter(self)
        w, h, length = self.width(), self.height(), max(self.day_length, 1)
        night, day = QColor("#141a2e"), QColor("#9fc7e8")
        p.fillRect(self.rect(), night)
        half = self.twilight * length / 2
        # Daylight (from sunrise to sunset, which may run past midnight), with twilight fading
        # in and out around its ends; drawn once more a day earlier for the part past midnight.
        for shift in (0, -length):
            start, end = self.sunrise + shift, self.sunset + shift
            gradient = QLinearGradient(QPointF((start - half) / length * w, 0),
                                       QPointF((end + half) / length * w, 0))
            span = max(end - start + 2 * half, 1e-9)
            fade = min(2 * half / span, 0.49)
            gradient.setColorAt(0.0, night)
            gradient.setColorAt(fade, day)
            gradient.setColorAt(1.0 - fade, day)
            gradient.setColorAt(1.0, night)
            p.fillRect(QRectF((start - half) / length * w, 0, span / length * w, h), gradient)
        p.setPen(QPen(QColor("#ffffff"), 2))
        x = self.time / length * w
        p.drawLine(QPointF(x, 0), QPointF(x, h))


# ---- the dock ------------------------------------------------------------------------------

def _int_spin(low: int, high: int, suffix: str = "") -> QSpinBox:
    box = QSpinBox()
    box.setRange(low, high)
    box.setSuffix(suffix)
    box.setKeyboardTracking(False)
    return box


def _real_spin(low: float, high: float, decimals: int, suffix: str = "") -> QDoubleSpinBox:
    box = QDoubleSpinBox()
    box.setRange(low, high)
    box.setDecimals(decimals)
    box.setSuffix(suffix)
    box.setKeyboardTracking(False)
    return box


class CalendarPanel(QWidget):
    time_scrubbed = Signal(int, float)  # the clock (game seconds) and its solar phase
    season_scrubbed = Signal(float)  # how far into summer that day is (1 summer, -1 winter)

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.project: Project | None = None
        self.cal: dict[str, Any] | None = None
        self._refreshing = False
        # The previewed moment: a date and seconds into its day.
        self.year, self.month, self.day, self.seconds = 1, 1, 1, 0

        layout = QVBoxLayout(self)
        top = QHBoxLayout()
        self.chooser = QComboBox()
        self.chooser.currentIndexChanged.connect(lambda _i: self._chosen())
        top.addWidget(self.chooser, 1)
        for text, tip, slot in (("New…", "A new calendar, starting as a copy of the standard one",
                                 self.new_calendar),
                                ("Duplicate", "A copy of this calendar", self.duplicate),
                                ("Delete", "Delete this calendar", self.delete_calendar)):
            button = QPushButton(text)
            button.setToolTip(tip)
            button.clicked.connect(slot)
            top.addWidget(button)
        layout.addLayout(top)
        self.standard = QCheckBox("Standard calendar")
        self.standard.setToolTip("The project's main calendar: the game's dates, wages, deadlines "
                                 "and anything not tied to a place follow it. There is one.")
        self.standard.toggled.connect(self._standard_toggled)
        layout.addWidget(self.standard)

        self.tabs = QTabWidget()
        self.tabs.addTab(self._time_tab(), "Time")
        self.tabs.addTab(self._months_tab(), "Months")
        self.tabs.addTab(self._weekdays_tab(), "Weekdays")
        self.tabs.addTab(self._seasons_tab(), "Seasons")
        self.tabs.addTab(self._moons_tab(), "Moons")
        self.tabs.addTab(self._bodies_tab(), "Bodies")
        from .weather_tab import WeatherTab
        self.weather_tab = WeatherTab()  # (the scene's weather: not the calendar's)
        self.tabs.addTab(self.weather_tab, "Weather")
        layout.addWidget(self.tabs)
        layout.addWidget(self._preview())
        self.problems = QLabel()
        self.problems.setWordWrap(True)
        self.problems.setStyleSheet("color: #d08030;")
        layout.addWidget(self.problems)
        layout.addStretch(1)
        self.set_project(None)

    # ---- building the tabs -----------------------------------------------------------------
    def _time_tab(self) -> QWidget:
        page = QWidget()
        form = QFormLayout(page)
        self.name = QLineEdit()
        self.name.editingFinished.connect(
            lambda: self._set("name", self.name.text().strip() or self.cal["id"], "name"))
        form.addRow("Name", self.name)
        self.hours_per_day = _int_spin(1, 1000, " hours")
        self.hours_per_day.valueChanged.connect(
            lambda v: self._set("hours_per_day", v, "hours per day"))
        form.addRow("Day", self.hours_per_day)
        self.minutes_per_hour = _int_spin(1, 1000, " minutes")
        self.minutes_per_hour.valueChanged.connect(
            lambda v: self._set("minutes_per_hour", v, "minutes per hour"))
        form.addRow("Hour", self.minutes_per_hour)
        self.real_minutes = _real_spin(0.1, 100000.0, 1, " real minutes")
        self.real_minutes.setToolTip("How long a day lasts for the player")
        self.real_minutes.valueChanged.connect(
            lambda v: self._set("real_minutes_per_day", v, "day length (real minutes)"))
        self.scale_label = QLabel()
        form.addRow("A day lasts", self.real_minutes)
        form.addRow("", self.scale_label)
        self.daylight = _real_spin(1.0, 99.0, 1, " %")
        self.daylight.setToolTip("The day's share of daylight, for a calendar without seasons "
                                 "(each season has its own)")
        self.daylight.valueChanged.connect(lambda v: self._set("daylight", v / 100, "daylight"))
        form.addRow("Daylight", self.daylight)
        self.noon = _real_spin(0.0, 99.9, 1, " % of the day")
        self.noon.setToolTip("Solar noon, the middle of daylight (50%: midday)")
        self.noon.valueChanged.connect(lambda v: self._set("solar_noon", v / 100, "solar noon"))
        form.addRow("Solar noon", self.noon)
        self.twilight = _real_spin(0.0, 25.0, 1, " % of the day")
        self.twilight.valueChanged.connect(lambda v: self._set("twilight", v / 100, "twilight"))
        form.addRow("Twilight", self.twilight)
        self.era = QLineEdit()
        self.era.setPlaceholderText("Year {n}")
        self.era.setToolTip("How a year is shown to players; {n} is its number")
        self.era.editingFinished.connect(lambda: self._set("era", self.era.text().strip(), "era"))
        form.addRow("Years shown as", self.era)
        start = QWidget()
        row = QHBoxLayout(start)
        row.setContentsMargins(0, 0, 0, 0)
        self.start_year = _int_spin(1, 1_000_000)
        self.start_year.setPrefix("Year ")
        self.start_month = QComboBox()
        self.start_day = _int_spin(1, 10000)
        self.start_hour = _int_spin(0, 999, " h")
        self.start_minute = _int_spin(0, 999, " min")
        for w in (self.start_year, self.start_month, self.start_day, self.start_hour,
                  self.start_minute):
            row.addWidget(w)
        self.start_year.valueChanged.connect(lambda v: self._set("start_year", v, "start year"))
        self.start_month.currentIndexChanged.connect(
            lambda i: i >= 0 and self._set("start_month", i + 1, "start month"))
        self.start_day.valueChanged.connect(lambda v: self._set("start_day", v, "start day"))
        self.start_hour.valueChanged.connect(lambda _v: self._set_start_time())
        self.start_minute.valueChanged.connect(lambda _v: self._set_start_time())
        form.addRow("A new game starts", start)
        return page

    def _table(self, headers: list[str], field: str, read) -> tuple[QWidget, QTableWidget]:
        page = QWidget()
        box = QVBoxLayout(page)
        table = QTableWidget(0, len(headers))
        table.setHorizontalHeaderLabels(headers)
        table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        table.verticalHeader().setVisible(False)
        table.itemChanged.connect(lambda item: self._table_edited(field, read, item))
        box.addWidget(table, 1)
        buttons = QHBoxLayout()
        add, remove = QPushButton("Add"), QPushButton("Remove")
        add.clicked.connect(lambda: self._add_row(field))
        remove.clicked.connect(lambda: self._remove_row(field, table))
        buttons.addWidget(add)
        buttons.addWidget(remove)
        buttons.addStretch(1)
        box.addLayout(buttons)
        return page, table

    def _months_tab(self) -> QWidget:
        page, self.months = self._table(
            ["Name", "Days"], "months",
            lambda r: {"name": self._text(self.months, r, 0),
                       "days": self._number(self.months, r, 1, int)})
        leap = QWidget()
        row = QHBoxLayout(leap)
        row.setContentsMargins(0, 0, 0, 0)
        self.leap_every = _int_spin(0, 1000, " years")
        self.leap_every.setSpecialValueText("No leap years")
        self.leap_every.valueChanged.connect(lambda v: self._set("leap_every", v, "leap years"))
        self.leap_month = QComboBox()
        self.leap_month.setToolTip("The month that gains a day in a leap year")
        self.leap_month.currentIndexChanged.connect(
            lambda i: i >= 0 and self._set("leap_month", i + 1, "leap month"))
        row.addWidget(QLabel("A leap day every"))
        row.addWidget(self.leap_every)
        row.addWidget(QLabel("in"))
        row.addWidget(self.leap_month, 1)
        page.layout().addWidget(leap)
        return page

    def _weekdays_tab(self) -> QWidget:
        page, self.weekdays = self._table(["Name"], "weekdays",
                                          lambda r: self._text(self.weekdays, r, 0))
        return page

    def _seasons_tab(self) -> QWidget:
        page, self.seasons = self._table(
            ["Name", "Share %", "Daylight %", "Min °C", "Max °C"], "seasons",
            lambda r: {"name": self._text(self.seasons, r, 0),
                       "share": self._number(self.seasons, r, 1, float) / 100,
                       "daylight": self._number(self.seasons, r, 2, float) / 100,
                       "temp_min": self._number(self.seasons, r, 3, float),
                       "temp_max": self._number(self.seasons, r, 4, float)})
        self.seasons.setToolTip("Each season's share of the year and daylight at its middle; "
                                "daylight changes smoothly between them. The first season "
                                "starts with the year.")
        return page

    def _moons_tab(self) -> QWidget:
        page, self.moons = self._table(
            ["Name", "Period (days)", "Phase at the start"], "moons",
            lambda r: {"name": self._text(self.moons, r, 0),
                       "period": self._number(self.moons, r, 1, float),
                       "offset": self._number(self.moons, r, 2, float)})
        return page

    def _bodies_tab(self) -> QWidget:
        page = QWidget()
        form = QFormLayout(page)
        form.addRow(QLabel("Each body's calendar is rolled from these ranges and the body's "
                           "seed (its months scaled to its year)."))
        self.range_boxes: dict[str, QDoubleSpinBox | QSpinBox] = {}
        for label, low_key, high_key, maker in (
                ("Day length", "day_hours_min", "day_hours_max",
                 lambda: _real_spin(1.0, 1000.0, 1, " h")),
                ("Year length", "year_days_min", "year_days_max",
                 lambda: _int_spin(0, 100000, " days")),
                ("Axial tilt", "tilt_min", "tilt_max", lambda: _real_spin(0.0, 90.0, 1, "°"))):
            row = QWidget()
            line = QHBoxLayout(row)
            line.setContentsMargins(0, 0, 0, 0)
            for key in (low_key, high_key):
                box = maker()
                box.valueChanged.connect(lambda v, k=key: self._set(k, v, k.replace("_", " ")))
                line.addWidget(box)
                self.range_boxes[key] = box
                if key == low_key:
                    line.addWidget(QLabel("to"))
            form.addRow(label, row)
        self.range_boxes["year_days_min"].setToolTip("0 to 0: the months as they are")
        self.seed = QSpinBox()
        self.seed.setRange(0, 2**31 - 1)
        self.seed.valueChanged.connect(lambda _v: self._roll())
        self.rolled = QLabel()
        form.addRow("Body preview: seed", self.seed)
        form.addRow("", self.rolled)
        return page

    def _preview(self) -> QWidget:
        group = QGroupBox("Preview")
        box = QVBoxLayout(group)
        row = QHBoxLayout()
        self.p_year = _int_spin(1, 1_000_000)
        self.p_year.setPrefix("Year ")
        self.p_month = QComboBox()
        self.p_day = _int_spin(1, 10000)
        for w in (self.p_year, self.p_month, self.p_day):
            row.addWidget(w)
        self.p_year.valueChanged.connect(lambda v: self._preview_date(v, self.month, self.day))
        self.p_month.currentIndexChanged.connect(
            lambda i: i >= 0 and self._preview_date(self.year, i + 1, self.day))
        self.p_day.valueChanged.connect(lambda v: self._preview_date(self.year, self.month, v))
        start = QPushButton("Start Date")
        start.setToolTip("Preview the date and time a new game starts at")
        start.clicked.connect(self.preview_start)
        row.addWidget(start)
        box.addLayout(row)
        self.date_label = QLabel()
        box.addWidget(self.date_label)
        self.year_strip = YearStrip()
        self.year_strip.day_picked.connect(self._day_of_year_picked)
        box.addWidget(self.year_strip)
        self.day_bar = DayBar()
        self.day_bar.time_picked.connect(self.scrub)
        box.addWidget(self.day_bar)
        self.scrubber = QSlider(Qt.Orientation.Horizontal)
        self.scrubber.setToolTip("The time of day: moves the viewport's sun (suns that follow the "
                                 "clock) and, while playing, the game's clock")
        self.scrubber.valueChanged.connect(lambda v: None if self._refreshing else self.scrub(v))
        box.addWidget(self.scrubber)
        self.time_label = QLabel()
        box.addWidget(self.time_label)
        return group

    # ---- the project ---------------------------------------------------------------------
    def set_project(self, project: Project | None) -> None:
        if self.project is not None:
            self.project.calendars_changed.disconnect(self._calendars_changed)
        self.project = project
        if project is not None:
            project.calendars_changed.connect(self._calendars_changed)
        self.setEnabled(project is not None)
        self.weather_tab.set_project(project)
        self.cal = None
        self._reload(keep="")

    def _calendars_changed(self, _calendar_id: str) -> None:
        self._reload(keep=self.cal["id"] if self.cal else "")

    def _reload(self, keep: str) -> None:
        self._refreshing = True
        self.chooser.clear()
        found = calendars.load_all(self.project) if self.project is not None else []
        for cal in found:
            self.chooser.addItem(cal["name"] + (" (standard)" if cal["standard"] else ""),
                                 cal["id"])
        index = max(self.chooser.findData(keep), 0) if found else -1
        self.chooser.setCurrentIndex(index)
        self._refreshing = False
        previous = self.cal["id"] if self.cal else None
        self.cal = next((c for c in found if c["id"] == self.chooser.currentData()), None)
        if self.cal is not None and self.cal["id"] != previous:  # a new one: from its start
            self._clamp_preview(self.cal["start_year"], self.cal["start_month"],
                                self.cal["start_day"])
            self.seconds = self.cal["start_minute"] * 60
        self.problems.setText("\n".join(calendars.problems(found)))
        self.refresh()

    def _chosen(self) -> None:
        if self._refreshing or self.project is None:
            return
        self.cal = calendars.load(self.project, self.chooser.currentData())
        if self.cal is not None:
            self._clamp_preview(self.cal["start_year"], self.cal["start_month"],
                                self.cal["start_day"])
            self.seconds = self.cal["start_minute"] * 60
        self.refresh()

    def select(self, calendar_id: str) -> None:
        self.chooser.setCurrentIndex(max(self.chooser.findData(calendar_id), 0))

    # ---- showing the calendar --------------------------------------------------------------
    def _fill_table(self, table: QTableWidget, rows: list[list[Any]]) -> None:
        table.setRowCount(len(rows))
        for r, values in enumerate(rows):
            for c, v in enumerate(values):
                if isinstance(v, float):
                    v = f"{v:g}"
                table.setItem(r, c, QTableWidgetItem(str(v)))

    def refresh(self) -> None:
        cal = self.cal
        self._refreshing = True
        try:
            for w in (self.tabs, self.standard):
                w.setEnabled(cal is not None)
            if cal is None:
                return
            self.standard.setChecked(cal["standard"])
            self.standard.setEnabled(not cal["standard"])  # (another one is made standard)
            self.name.setText(cal["name"])
            self.era.setText(cal["era"])
            self.hours_per_day.setValue(cal["hours_per_day"])
            self.minutes_per_hour.setValue(cal["minutes_per_hour"])
            self.real_minutes.setValue(cal["real_minutes_per_day"])
            day_seconds = cal["hours_per_day"] * cal["minutes_per_hour"] * 60
            scale = day_seconds / max(cal["real_minutes_per_day"] * 60, 1e-9)
            self.scale_label.setText(f"Time runs {scale:g} times as fast as real time")
            self.daylight.setValue(cal["daylight"] * 100)
            self.noon.setValue(cal["solar_noon"] * 100)
            self.twilight.setValue(cal["twilight"] * 100)
            month_names = [m["name"] or f"Month {i + 1}" for i, m in enumerate(cal["months"])]
            for combo, value in ((self.start_month, cal["start_month"]),
                                 (self.leap_month, cal["leap_month"]),
                                 (self.p_month, self.month)):
                combo.clear()
                combo.addItems(month_names)
                combo.setCurrentIndex(value - 1)
            self.start_year.setValue(cal["start_year"])
            self.start_day.setValue(cal["start_day"])
            self.start_hour.setValue(cal["start_minute"] // cal["minutes_per_hour"])
            self.start_minute.setValue(cal["start_minute"] % cal["minutes_per_hour"])
            self.leap_every.setValue(cal["leap_every"])
            self._fill_table(self.months, [[m["name"], m["days"]] for m in cal["months"]])
            self._fill_table(self.weekdays, [[w] for w in cal["weekdays"]])
            self._fill_table(self.seasons, [[s["name"], s["share"] * 100, s["daylight"] * 100,
                                             s["temp_min"], s["temp_max"]]
                                            for s in cal["seasons"]])
            self._fill_table(self.moons, [[m["name"], m["period"], m["offset"]]
                                          for m in cal["moons"]])
            for key, box in self.range_boxes.items():
                box.setValue(cal[key])
        finally:
            self._refreshing = False
        self._roll()
        self.refresh_preview()

    def _engine(self):
        """The engine's view of the calendar, or None if it can't be used as it stands."""
        if self.cal is None:
            return None
        try:
            engine_cal = calendars.engine_calendar(self.cal)
        except (RuntimeError, ValueError):
            return None
        if engine_cal is None or engine_cal.problems():
            return None
        return engine_cal

    def _clamp_preview(self, year: int, month: int, day: int) -> None:
        cal = self.cal
        if cal is None or not cal["months"]:
            return
        engine_cal = self._engine()
        self.year = max(year, 1)
        self.month = min(max(month, 1), len(cal["months"]))
        days = cal["months"][self.month - 1]["days"]
        if engine_cal is not None:  # (a leap year's month may have a day more)
            start = engine_cal.time_of(self.year, self.month, 1)
            days = max(days, 1)
            while days < 1000 and engine_cal.date(start + days * engine_cal.seconds_per_day)[
                    "month"] == self.month:
                days += 1
        self.day = min(max(day, 1), max(days, 1))

    def refresh_preview(self) -> None:
        engine_cal = self._engine()
        self._refreshing = True
        try:
            if engine_cal is None:
                self.date_label.setText("No preview until the calendar can be used (see below).")
                self.year_strip.set_year([], [], 0)
                return
            spd, sph = engine_cal.seconds_per_day, engine_cal.seconds_per_hour
            self.seconds = min(self.seconds, spd - 1)
            self.p_year.setValue(self.year)
            self.p_month.setCurrentIndex(self.month - 1)
            self.p_day.setValue(self.day)
            t = engine_cal.time_of(self.year, self.month, self.day, self.seconds)
            date = engine_cal.date(t)
            days = engine_cal.year_days(self.year)
            self.year_strip.set_year([engine_cal.daylight(self.year, d) for d in range(days)],
                                     [engine_cal.season_on(self.year, d) for d in range(days)],
                                     date["day_of_year"])
            sunrise, sunset = engine_cal.sun_times(t)
            self.day_bar.set_day(spd, sunrise, sunset, self.cal["twilight"], self.seconds)
            self.scrubber.setRange(0, spd - 1)
            self.scrubber.setSingleStep(60)
            self.scrubber.setPageStep(sph)
            self.scrubber.setValue(self.seconds)
            cal = self.cal
            season = cal["seasons"][date["season"]]["name"] if date["season"] >= 0 else ""
            weekday = cal["weekdays"][date["weekday"]] if cal["weekdays"] else ""
            daylight = engine_cal.daylight(self.year, date["day_of_year"])
            era = (cal["era"] or "Year {n}").replace("{n}", str(self.year))
            self.date_label.setText(
                f"{weekday}, {cal['months'][self.month - 1]['name']} {self.day}, {era}"
                + (f" · {season}" if season else "")
                + f" · daylight {daylight * 100:.0f}%, sunrise {clock_text(sunrise, sph)},"
                  f" sunset {clock_text(sunset % spd, sph)}")
            phase = engine_cal.solar_phase(self.year, date["day_of_year"], self.seconds)
            part = "day" if phase < 0.5 else "night"
            self.time_label.setText(f"{clock_text(self.seconds, sph)} · solar phase "
                                    f"{phase:.3f} ({part})")
        finally:
            self._refreshing = False

    def _roll(self) -> None:
        engine_cal = self._engine()
        if engine_cal is None:
            self.rolled.setText("")
            return
        body = engine_cal.roll(self.seed.value())
        self.rolled.setText(f"A {body.hours_per_day}-hour day, a {sum(body.months)}-day year, "
                            f"tilted {body.tilt:.1f}°")

    # ---- the preview's date and time -----------------------------------------------------
    def _preview_date(self, year: int, month: int, day: int) -> None:
        if self._refreshing:
            return
        self._clamp_preview(year, month, day)
        self.refresh_preview()

    def _day_of_year_picked(self, day_of_year: int) -> None:
        engine_cal = self._engine()
        if engine_cal is None:
            return
        start = engine_cal.time_of(self.year, 1, 1)
        date = engine_cal.date(start + day_of_year * engine_cal.seconds_per_day)
        self._clamp_preview(date["year"], date["month"], date["day"])
        self.scrub(self.seconds)

    def preview_start(self) -> None:
        if self.cal is None:
            return
        self._clamp_preview(self.cal["start_year"], self.cal["start_month"],
                            self.cal["start_day"])
        self.scrub(self.cal["start_minute"] * 60)

    def scrub(self, seconds_into_day: int) -> None:
        """The scrubber (or the day bar) moved: the preview, the viewport's sun and, while
        playing, the game's clock follow (time_scrubbed)."""
        engine_cal = self._engine()
        if engine_cal is None:
            return
        self.seconds = min(max(int(seconds_into_day), 0), engine_cal.seconds_per_day - 1)
        self.refresh_preview()
        t = engine_cal.time_of(self.year, self.month, self.day, self.seconds)
        date = engine_cal.date(t)
        self.time_scrubbed.emit(t, engine_cal.solar_phase(self.year, date["day_of_year"],
                                                          self.seconds))
        if hasattr(engine_cal, "summer"):  # the environment's season tints follow
            self.season_scrubbed.emit(engine_cal.summer(self.year, date["day_of_year"]))

    # ---- changes (each one undoable) -------------------------------------------------------
    def _push(self, new: dict[str, Any], label: str, field: str) -> None:
        if self.project is None or self.cal is None or new == self.cal:
            return
        self.project.undo_stack.push(CalendarCommand(self.project, self.cal, new, label, field))

    def _set(self, key: str, value: Any, label: str) -> None:
        if self._refreshing or self.cal is None:
            return
        self._push({**copy.deepcopy(self.cal), key: value}, label, key)

    def _set_start_time(self) -> None:
        if self._refreshing or self.cal is None:
            return
        minute = self.start_hour.value() * self.cal["minutes_per_hour"] + self.start_minute.value()
        self._set("start_minute", minute, "start time")

    @staticmethod
    def _text(table: QTableWidget, row: int, col: int) -> str:
        item = table.item(row, col)
        return item.text().strip() if item else ""

    @staticmethod
    def _number(table: QTableWidget, row: int, col: int, kind):
        try:
            return kind(float(CalendarPanel._text(table, row, col).replace(",", ".")))
        except ValueError:
            return kind(0)

    def _table_edited(self, field: str, read, item: QTableWidgetItem) -> None:
        if self._refreshing or self.cal is None:
            return
        table = item.tableWidget()
        rows = [read(r) for r in range(table.rowCount())]
        self._push({**copy.deepcopy(self.cal), field: rows}, field,
                   f"{field}.{item.row()}.{item.column()}")

    def _add_row(self, field: str) -> None:
        if self.cal is None:
            return
        rows = copy.deepcopy(self.cal[field])
        n = len(rows) + 1
        rows.append({"months": {"name": f"Month {n}", "days": 30},
                     "weekdays": f"Day {n}",
                     "seasons": {"name": f"Season {n}", "share": 0.0, "daylight": 0.5,
                                 "temp_min": 0.0, "temp_max": 20.0},
                     "moons": {"name": f"Moon {n}", "period": 29.5, "offset": 0.0}}[field])
        self._push({**copy.deepcopy(self.cal), field: rows}, f"{field} (added)", "")

    def _remove_row(self, field: str, table: QTableWidget) -> None:
        if self.cal is None or not self.cal[field]:
            return
        row = table.currentRow() if table.currentRow() >= 0 else len(self.cal[field]) - 1
        rows = copy.deepcopy(self.cal[field])
        del rows[row]
        new = {**copy.deepcopy(self.cal), field: rows}
        if field == "months":  # keep the start and leap months among them
            new["start_month"] = min(new["start_month"], max(len(rows), 1))
            new["leap_month"] = min(new["leap_month"], max(len(rows), 1))
        self._push(new, f"{field} (removed)", "")

    def _standard_toggled(self, on: bool) -> None:
        if self._refreshing or self.project is None or self.cal is None or not on:
            return
        calendars.make_standard(self.project, self.cal["id"])

    def new_calendar(self, name: str | None = None) -> str | None:
        """A new calendar, starting as a copy of the standard one. Returns its id."""
        if self.project is None:
            return None
        if name is None:
            name, ok = QInputDialog.getText(self, "New Calendar", "Name:")
            if not ok or not name.strip():
                return None
        found = calendars.load_all(self.project)
        base = next((c for c in found if c["standard"]), found[0] if found else None)
        if base is None:
            return None
        cal = {**copy.deepcopy(base), "id": calendars.new_id(self.project, name.strip()),
               "name": name.strip(), "standard": False}
        self.project.undo_stack.push(CalendarCommand(self.project, None, cal, ""))
        self.select(cal["id"])
        return cal["id"]

    def duplicate(self) -> str | None:
        if self.project is None or self.cal is None:
            return None
        name = f"{self.cal['name']} copy"
        cal = {**copy.deepcopy(self.cal), "id": calendars.new_id(self.project, name),
               "name": name, "standard": False}
        self.project.undo_stack.push(CalendarCommand(self.project, None, cal, ""))
        self.select(cal["id"])
        return cal["id"]

    def delete_calendar(self, confirm: bool = True) -> None:
        if self.project is None or self.cal is None:
            return
        if self.cal["standard"]:
            QMessageBox.information(self, "Delete Calendar", "The standard calendar can't be "
                                    "deleted. Make another calendar the standard one first.")
            return
        if confirm and QMessageBox.question(
                self, "Delete Calendar", f"Delete the calendar {self.cal['name']}?") != \
                QMessageBox.StandardButton.Yes:
            return
        self.project.undo_stack.push(CalendarCommand(self.project, self.cal, None, ""))
