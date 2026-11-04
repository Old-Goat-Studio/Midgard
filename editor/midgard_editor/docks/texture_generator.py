# SPDX-License-Identifier: Apache-2.0
"""The Texture Generator window (Assets ▸ Texture Generator…, milestone 5.17 step 6): a
pattern, its settings and a colour ramp on the left, the picture on the right (tiled 2 x 2 if
wished, to check its edges meet), and buttons to save it in the project, add it to the
project's Asset Library or send it to the Master Asset Library."""
import copy
import logging
import random

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QColor, QPainter, QPixmap
from PySide6.QtWidgets import (
    QCheckBox,
    QColorDialog,
    QComboBox,
    QDialog,
    QDoubleSpinBox,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMenu,
    QMessageBox,
    QPushButton,
    QSpinBox,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

from .. import asset_library as lib
from .. import texture_gen as tg
from ..project import Project

log = logging.getLogger("midgard")
PREVIEW = 320  # pixels a side (the preview is generated at 256 and scaled)
MAX_STOPS = 6


class TextureGenerator(QDialog):
    def __init__(self, project: Project, settings=None, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWindowTitle("Texture Generator")
        self.project, self.settings = project, settings
        self.recipe: dict = copy.deepcopy(tg.DEFAULT)
        self.saved_pictures: list = []  # what the last save made
        self._loading = False

        form = QFormLayout()
        self.preset = QComboBox()
        self.preset.addItem("(choose a starting point)", "")
        for name in tg.PRESETS:
            self.preset.addItem(name, name)
        self.preset.activated.connect(lambda _i: self.use_preset(self.preset.currentData()))
        self.open_button = QToolButton()
        self.open_button.setText("Open")
        self.open_button.setToolTip("A texture made here before (its recipe is kept)")
        self.open_button.setPopupMode(QToolButton.ToolButtonPopupMode.InstantPopup)
        self.open_menu = QMenu(self.open_button)
        self.open_menu.aboutToShow.connect(self._fill_open_menu)
        self.open_button.setMenu(self.open_menu)
        row = QHBoxLayout()
        row.addWidget(self.preset, 1)
        row.addWidget(self.open_button)
        form.addRow("Start from", row)

        self.pattern = QComboBox()
        for key, label in tg.PATTERN_LABELS.items():
            self.pattern.addItem(label, key)
        self.pattern.currentIndexChanged.connect(lambda _i: self._set("pattern",
                                                                      self.pattern.currentData()))
        form.addRow("Pattern", self.pattern)
        self.size = QComboBox()
        for s in (256, 512, 1024, 2048):
            self.size.addItem(f"{s} × {s}", s)
        self.size.currentIndexChanged.connect(lambda _i: self._set("size", self.size.currentData()))
        form.addRow("Size", self.size)

        self.numbers: dict[str, QWidget] = {}
        self.rows: dict[str, tuple[QLabel, QWidget]] = {}
        specs = [  # key, label, low, high, step, decimals, tooltip
            ("scale", "Scale", 0.5, 64, 1, 1, "How many features across (a whole number tiles)"),
            ("octaves", "Detail", 1, 10, 1, 0, "Layers of finer noise"),
            ("roughness", "Roughness", 0, 1, 0.05, 2, "How strong each finer layer is"),
            ("warp", "Warp", 0, 4, 0.1, 2, "Bends the pattern with noise"),
            ("angle", "Angle", -180, 180, 5, 0, "Which way it runs (degrees)"),
            ("width", "Width", 0, 1, 0.05, 2, "A stripe's width; bricks: the mortar"),
            ("softness", "Softness", 0, 0.5, 0.01, 2, "How soft the edges are"),
            ("jitter", "Irregular", 0, 1, 0.05, 2, "How irregular the cells are"),
            ("variation", "Variation", 0, 1, 0.05, 2, "How much bricks differ"),
            ("strength", "Bump strength", 0, 20, 0.5, 1, "How steep the bumps look"),
        ]
        for key, label, low, high, step, decimals, tip in specs:
            box = QSpinBox() if decimals == 0 else QDoubleSpinBox()
            box.setRange(low, high)
            box.setSingleStep(step)
            if decimals:
                box.setDecimals(decimals)
            box.setToolTip(tip)
            box.valueChanged.connect(lambda v, k=key: self._set(k, v))
            name = QLabel(label)
            form.addRow(name, box)
            self.numbers[key] = box
            self.rows[key] = (name, box)
        self.seed = QSpinBox()
        self.seed.setRange(0, 999999)
        self.seed.valueChanged.connect(lambda v: self._set("seed", v))
        reseed = QPushButton("New")
        reseed.setToolTip("Another random pattern of the same kind")
        reseed.clicked.connect(lambda: self.seed.setValue(random.randint(0, 999999)))
        row = QHBoxLayout()
        row.addWidget(self.seed, 1)
        row.addWidget(reseed)
        form.addRow("Seed", row)
        self.invert = QCheckBox("Invert")
        self.invert.toggled.connect(lambda on: self._set("invert", on))
        form.addRow("", self.invert)

        self.output = QComboBox()
        self.output.addItem("Colours", "color")
        self.output.addItem("Bumps (a normal map)", "normal")
        self.output.currentIndexChanged.connect(lambda _i: self._set("output",
                                                                     self.output.currentData()))
        form.addRow("Makes", self.output)
        self.bumps_too = QCheckBox("Its bumps too (a normal map beside it)")
        self.bumps_too.setChecked(True)
        form.addRow("", self.bumps_too)

        self.stops_box = QWidget()
        self.stops_layout = QVBoxLayout(self.stops_box)
        self.stops_layout.setContentsMargins(0, 0, 0, 0)
        self.colours_label = QLabel("Colours")
        form.addRow(self.colours_label, self.stops_box)

        self.preview = QLabel()
        self.preview.setFixedSize(PREVIEW, PREVIEW)
        self.preview.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.tiled = QCheckBox("Show it tiled (2 × 2)")
        self.tiled.toggled.connect(lambda _on: self._show_preview())
        self.problems = QLabel()
        self.problems.setWordWrap(True)
        self.problems.setStyleSheet("color: #d08030;")
        side = QVBoxLayout()
        side.addWidget(self.preview)
        side.addWidget(self.tiled)
        side.addWidget(self.problems)
        side.addStretch(1)

        self.name = QLineEdit("texture")
        self.name.setToolTip("Its file name, in the project's textures/generated folder")
        self.licence = QComboBox()
        for key in (lib.OWN_WORK, "CC0-1.0"):
            self.licence.addItem(lib.LICENCES[key][0], key)
        self.licence.setToolTip("The licence it has in the Asset Library")
        self.author = QLineEdit(settings.get("texture_author", "") if settings else "")
        self.author.setPlaceholderText("Your name")
        self.author.setToolTip("Who made it (the Asset Library needs it for your own work)")
        save = QPushButton("Save to Project")
        save.clicked.connect(self.save_to_project)
        add = QPushButton("Add to Library")
        add.clicked.connect(self.add_to_library)
        send = QPushButton("Send to Master")
        send.clicked.connect(self.send_to_master)
        close = QPushButton("Close")
        close.clicked.connect(self.close)
        buttons = QHBoxLayout()
        buttons.addWidget(QLabel("Name"))
        buttons.addWidget(self.name, 1)
        buttons.addWidget(self.licence)
        buttons.addWidget(self.author)
        buttons.addWidget(save)
        buttons.addWidget(add)
        buttons.addWidget(send)
        buttons.addWidget(close)

        top = QHBoxLayout()
        left = QWidget()
        left.setLayout(form)
        top.addWidget(left, 1)
        top.addLayout(side)
        layout = QVBoxLayout(self)
        layout.addLayout(top, 1)
        layout.addLayout(buttons)

        self._timer = QTimer(self, singleShot=True, interval=150)
        self._timer.timeout.connect(self._show_preview)
        self.show_recipe()

    # ---- the recipe -------------------------------------------------------------------------
    def _set(self, key: str, value) -> None:
        if self._loading:
            return
        self.recipe[key] = value
        if key in ("pattern", "output"):
            self._show_rows()
        self._timer.start()

    def use_preset(self, name: str) -> None:
        if name:
            self.recipe = copy.deepcopy(tg.PRESETS[name])
            self.name.setText(tg.safe_name(name.split(" (")[0].lower().replace(" ", "_")))
            self.show_recipe()

    def open_saved(self, name: str) -> None:
        try:
            self.recipe, bumps = tg.load(self.project.root, name)
        except tg.GeneratorError as e:
            QMessageBox.warning(self, "Texture Generator", str(e))
            return
        self.bumps_too.setChecked(bumps)
        self.name.setText(name)
        self.show_recipe()

    def _fill_open_menu(self) -> None:
        self.open_menu.clear()
        names = tg.saved(self.project.root)
        for name in names:
            self.open_menu.addAction(name, lambda n=name: self.open_saved(n))
        if not names:
            self.open_menu.addAction("(none made yet)").setEnabled(False)

    def show_recipe(self) -> None:
        """The controls show the recipe (without each change making the picture again)."""
        self._loading = True
        r = self.recipe
        self.pattern.setCurrentIndex(max(0, self.pattern.findData(r["pattern"])))
        self.size.setCurrentIndex(max(0, self.size.findData(int(r["size"]))))
        for key, box in self.numbers.items():
            box.setValue(int(r[key]) if isinstance(box, QSpinBox) else float(r[key]))
        self.seed.setValue(int(r["seed"]))
        self.invert.setChecked(bool(r["invert"]))
        self.output.setCurrentIndex(max(0, self.output.findData(r["output"])))
        self._loading = False
        self._show_stops()
        self._show_rows()
        self._show_preview()

    def _show_rows(self) -> None:
        """Only the settings the pattern (and the output) use."""
        used = set(tg.PATTERN_SETTINGS.get(self.recipe["pattern"], ()))
        normal = self.recipe["output"] == "normal"
        for key, (label, box) in self.rows.items():
            show = key in used or key in ("scale", "warp") or (key == "strength" and normal)
            label.setVisible(show)
            box.setVisible(show)
        self.colours_label.setVisible(not normal)
        self.stops_box.setVisible(not normal)
        self.bumps_too.setVisible(not normal)

    # ---- the colour ramp ----------------------------------------------------------------------
    def _show_stops(self) -> None:
        while self.stops_layout.count():
            item = self.stops_layout.takeAt(0)
            if item.widget():
                item.widget().hide()  # (now: it is deleted a moment later)
                item.widget().deleteLater()
        stops = self.recipe["colors"]
        for i, (at, rgb) in enumerate(stops):
            row = QWidget()
            h = QHBoxLayout(row)
            h.setContentsMargins(0, 0, 0, 0)
            where = QDoubleSpinBox()
            where.setRange(0, 1)
            where.setSingleStep(0.05)
            where.setValue(at)
            where.setToolTip("Where on the ramp (0: the lowest, 1: the highest)")
            where.valueChanged.connect(lambda v, k=i: self._set_stop(k, at=v))
            colour = QPushButton()
            c = QColor.fromRgbF(*rgb)
            colour.setStyleSheet(f"background-color: {c.name()};")
            colour.setToolTip(c.name())
            colour.clicked.connect(lambda _c=False, k=i: self._pick_stop(k))
            remove = QToolButton()
            remove.setText("✕")
            remove.setEnabled(len(stops) > 2)
            remove.clicked.connect(lambda _c=False, k=i: self._remove_stop(k))
            h.addWidget(where)
            h.addWidget(colour, 1)
            h.addWidget(remove)
            self.stops_layout.addWidget(row)
        add = QPushButton("Add a colour")
        add.setEnabled(len(stops) < MAX_STOPS)
        add.clicked.connect(self._add_stop)
        self.stops_layout.addWidget(add)

    def _set_stop(self, index: int, at: float | None = None, rgb: list | None = None) -> None:
        stop = self.recipe["colors"][index]
        if at is not None:
            stop[0] = round(at, 3)
        if rgb is not None:
            stop[1] = rgb
        self._timer.start()

    def _pick_stop(self, index: int) -> None:
        current = QColor.fromRgbF(*self.recipe["colors"][index][1])
        chosen = QColorDialog.getColor(current, self, "Colour")
        if chosen.isValid():
            self._set_stop(index, rgb=[round(chosen.redF(), 4), round(chosen.greenF(), 4),
                                       round(chosen.blueF(), 4)])
            self._show_stops()

    def _add_stop(self) -> None:
        stops = self.recipe["colors"]
        stops.append([1.0, list(stops[-1][1])])
        stops[-2][0] = min(stops[-2][0], 0.5) if len(stops) == 2 else stops[-2][0]
        self._show_stops()
        self._timer.start()

    def _remove_stop(self, index: int) -> None:
        if len(self.recipe["colors"]) > 2:
            del self.recipe["colors"][index]
            self._show_stops()
            self._timer.start()

    # ---- the picture --------------------------------------------------------------------------
    def _show_preview(self) -> None:
        try:
            size, pixels, problems = tg.generate({**self.recipe, "size": 256})
        except tg.GeneratorError as e:
            self.preview.setText(str(e))
            return
        self.problems.setText("; ".join(problems))
        image = tg.to_image(size, pixels)
        pixmap = QPixmap(PREVIEW, PREVIEW)
        painter = QPainter(pixmap)
        if self.tiled.isChecked():
            half = PREVIEW // 2
            for x in (0, half):
                for y in (0, half):
                    painter.drawImage(x, y, image.scaled(half, half))
        else:
            painter.drawImage(0, 0, image.scaled(PREVIEW, PREVIEW,
                                                 Qt.AspectRatioMode.IgnoreAspectRatio,
                                                 Qt.TransformationMode.SmoothTransformation))
        painter.end()
        self.preview.setPixmap(pixmap)

    # ---- saving -------------------------------------------------------------------------------
    def save_to_project(self) -> list:
        try:
            made = tg.save(self.project.root, self.name.text(), self.recipe,
                           bumps_too=self.bumps_too.isChecked())
        except (tg.GeneratorError, OSError) as e:
            QMessageBox.warning(self, "Texture Generator", f"Not saved: {e}")
            return []
        self.saved_pictures = made
        self.project.files_changed.emit([f"{tg.FOLDER}/{p.name}" for p in made])
        log.info("Saved the generated texture %s", ", ".join(p.name for p in made))
        return made

    def add_to_library(self, tell: bool = True):
        made = self.save_to_project()
        if not made:
            return None
        try:
            author = self.author.text().strip()
            if self.settings is not None and author:
                self.settings.set("texture_author", author)
            asset = tg.add_to_library(self.project, made, self.name.text(),
                                      licence=self.licence.currentData(), author=author)
        except (lib.LibraryError, OSError) as e:
            QMessageBox.warning(self, "Add to Library", f"Not added: {e}")
            return None
        log.info("Added '%s' to the Asset Library", asset.name)
        if tell:
            QMessageBox.information(self, "Add to Library",
                                    f"'{asset.name}' is in the project's Asset Library.")
        return asset

    def send_to_master(self):
        from .. import master as mst
        try:
            found = mst.find_master(self.settings)
        except mst.MasterError as e:
            QMessageBox.warning(self, "Send to Master", str(e))
            return None
        asset = self.add_to_library(tell=False)
        if asset is None:
            return None
        try:
            given = mst.send_to_master(asset, found)
        except (mst.MasterError, lib.LibraryError, OSError) as e:
            QMessageBox.warning(self, "Send to Master", f"Not sent: {e}")
            return None
        log.info("Sent '%s' to the Master", asset.name)
        QMessageBox.information(self, "Send to Master",
                                f"'{asset.name}' is in the project's Asset Library and the "
                                "Master Asset Library.")
        return given
