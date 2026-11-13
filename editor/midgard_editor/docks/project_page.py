# SPDX-License-Identifier: Apache-2.0
"""The Project page: the game's title, tagline, description, version and player count, and its
logo and application icon. Every change is undoable. The text is kept as localisation strings
(game.title, game.tagline, game.description) in {project}.db; the rest is in {project}.config,
with the images in the project's branding/ folder (see branding.py)."""
from pathlib import Path

from PySide6.QtCore import QRegularExpression, Qt, QTimer
from PySide6.QtGui import QPixmap, QRegularExpressionValidator
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QFileDialog,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QScrollArea,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from .. import branding, menus, music
from ..project import (
    GAME_DESCRIPTION_KEY,
    GAME_TAGLINE_KEY,
    GAME_TITLE_KEY,
    WORLD_MODES,
    Project,
)
from ..undo import SetConfigCommand, SetStringCommand
from .fresh_combo import FreshComboBox, decorate_pictures

PREVIEW = {"logo": 160, "icon": 96}


class ImagePicker(QWidget):
    """A preview of one branding image, with Choose… and Remove."""

    def __init__(self, page: "ProjectPage", kind: str):
        super().__init__()
        self.page, self.kind = page, kind
        self.preview = QLabel()
        self.preview.setFixedSize(PREVIEW[kind], PREVIEW[kind])
        self.preview.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.preview.setStyleSheet("border: 1px dashed palette(mid);")
        self.small = QLabel()  # the icon as the taskbar shows it
        self.note = QLabel()
        self.note.setWordWrap(True)
        self.note.setEnabled(False)
        choose = QPushButton("Choose…")
        choose.clicked.connect(self.choose)
        self.remove = QPushButton("Remove")
        self.remove.clicked.connect(lambda: page.set_branding(branding.removed(page.project,
                                                                               kind), kind))
        buttons = QVBoxLayout()
        buttons.addWidget(choose)
        buttons.addWidget(self.remove)
        buttons.addWidget(self.small)
        buttons.addStretch()
        row = QHBoxLayout(self)
        row.setContentsMargins(0, 0, 0, 0)
        row.addWidget(self.preview)
        row.addLayout(buttons)
        row.addWidget(self.note, 1)

    def choose(self) -> None:
        source, _ = QFileDialog.getOpenFileName(self, f"Choose the {branding.KINDS[self.kind]}",
                                                str(Path.home()), branding.IMAGE_FILTER)
        if source:
            self.page.import_image(self.kind, Path(source))

    def refresh(self) -> None:
        file = branding.path(self.page.project, self.kind)
        self.remove.setEnabled(file is not None)
        if file is None:
            self.preview.setPixmap(QPixmap())
            self.preview.setText("No image")
            self.small.clear()
            self.note.setText("PNG, JPEG or BMP. Square, 256 × 256 or larger, works best."
                              if self.kind == "icon" else "PNG, JPEG or BMP.")
            return
        pixmap = QPixmap(str(file))
        side = PREVIEW[self.kind] - 4
        self.preview.setPixmap(pixmap.scaled(side, side, Qt.AspectRatioMode.KeepAspectRatio,
                                             Qt.TransformationMode.SmoothTransformation))
        saved_as = file.relative_to(self.page.project.root).as_posix()
        note = f"{pixmap.width()} × {pixmap.height()}, saved as {saved_as}"
        if self.kind == "icon":
            ico = branding.path(self.page.project, "icon_ico")
            self.small.setPixmap(pixmap.scaled(16, 16, Qt.AspectRatioMode.KeepAspectRatio,
                                               Qt.TransformationMode.SmoothTransformation))
            self.small.setToolTip("At 16 × 16, as in a window's title bar")
            if ico is not None:
                note += (f"\nWindows icon: {ico.relative_to(self.page.project.root).as_posix()} "
                         "(16 to 256 pixels)")
        self.note.setText(note)


class ProjectPage(QScrollArea):
    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.project: Project | None = None
        self.setWidgetResizable(True)
        body = QWidget()
        self.setWidget(body)
        self.heading = QLabel()
        self.title = QLineEdit()
        self.title.setPlaceholderText("The game's name, as players see it")
        self.tagline = QLineEdit()
        self.tagline.setPlaceholderText("One line that sums the game up")
        self.description = QPlainTextEdit()
        self.description.setPlaceholderText("What the game is about, for a store page or the "
                                            "about screen")
        self.description.setMinimumHeight(90)
        self.description.setMaximumHeight(150)
        self.game_version = QLineEdit()
        self.game_version.setValidator(QRegularExpressionValidator(
            QRegularExpression(r"\d{1,3}\.\d{1,3}\.\d{1,3}"), self.game_version))
        self.game_version.setMaximumWidth(120)
        self.max_players = QSpinBox(minimum=2, maximum=16)
        self.max_players.setMaximumWidth(120)
        self.world_mode = QLabel()
        self.folder = QLabel()
        self.folder.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.logo = ImagePicker(self, "logo")
        self.icon = ImagePicker(self, "icon")
        form = QFormLayout()
        form.addRow("Game title", self.title)
        form.addRow("Tagline", self.tagline)
        form.addRow("Description", self.description)
        form.addRow("Game version", self.game_version)
        form.addRow("Max players", self.max_players)
        form.addRow("World mode", self.world_mode)
        form.addRow("Logo", self.logo)
        form.addRow("Application icon", self.icon)
        form.addRow("Folder", self.folder)
        self.credits = QLabel()
        self.credits.setWordWrap(True)
        self.credits.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.credits.setToolTip("The Asset Library's CC BY assets the game uses: the cook "
                                "writes them into CREDITS.txt")
        form.addRow("Credits", self.credits)
        self.error_log = QCheckBox("Keep an error log for this project (error.log in its "
                                   "folder)")
        self.error_log.setToolTip("The last 100 keys and clicks, and errors, so a crash can be "
                                  "understood. Turn it off for speed. (Edit ▸ Preferences can "
                                  "turn it off for every project.)")
        self.error_log.toggled.connect(self._error_log_toggled)
        form.addRow("Error log", self.error_log)
        # The game's screens (6.0): the splash, the crosshair, the menu pointer, menu music.
        self.crosshair = FreshComboBox(self.show_game_settings)
        self.crosshair.setToolTip("The crosshair's picture, in the middle of the screen (from "
                                  "the GUI section). Without one, a small white dot")
        self.crosshair_views = QComboBox()
        for key, label in menus.CROSSHAIR_VIEWS.items():
            self.crosshair_views.addItem(label, key)
        self.crosshair_views.setToolTip("When the crosshair shows (a script can still show or "
                                        "hide it: midgard.ui.crosshair)")
        self.crosshair_size = QSpinBox(minimum=4, maximum=256, suffix=" dp")
        self.crosshair_size.setMaximumWidth(120)
        self.crosshair_size.setToolTip("How big the crosshair picture is, across")
        self.pointer = FreshComboBox(self.show_game_settings)
        self.pointer.setToolTip("The mouse pointer over menus (from the GUI section; its tip is "
                                "the picture's top left corner, best 32 x 32). Without one, the "
                                "system's")
        self.window_mode = QComboBox()
        self.window_mode.addItem("In a window", "windowed")
        self.window_mode.addItem("Full screen", "fullscreen")
        self.window_mode.setToolTip("How the game starts on a computer (phones are always full "
                                    "screen). Players switch with F11 or Alt+Enter")
        self.window_mode.activated.connect(
            lambda _i: self._set_game("window", self.window_mode.currentData()))
        self.menu_music = FreshComboBox(self.show_game_settings)
        self.menu_music.setToolTip("A playlist (the Audio panel's) that plays while a menu shows, "
                                   "over the scene's music; a screen can keep it off")
        for key, box in (("crosshair", self.crosshair),
                         ("pointer", self.pointer), ("menu_music", self.menu_music),
                         ("crosshair_views", self.crosshair_views)):
            box.activated.connect(lambda _i, k=key, b=box: self._set_game(k, b.currentData()))
        self.crosshair_size.editingFinished.connect(
            lambda: self._set_game("crosshair_size", self.crosshair_size.value()))
        screens = QFormLayout()
        screens.addRow("Crosshair", self.crosshair)
        screens.addRow("Crosshair shows", self.crosshair_views)
        screens.addRow("Crosshair size", self.crosshair_size)
        screens.addRow("Menu pointer", self.pointer)
        screens.addRow("Menu music", self.menu_music)
        screens.addRow("Starts", self.window_mode)
        layout = QVBoxLayout(body)
        layout.addWidget(self.heading)
        layout.addLayout(form)
        layout.addWidget(QLabel("<h3>Game screens</h3>"))
        layout.addLayout(screens)
        layout.addStretch()

        self.title.editingFinished.connect(lambda: self._text_edited(
            GAME_TITLE_KEY, self.title.text(), "game title", required=True))
        self.tagline.editingFinished.connect(lambda: self._text_edited(
            GAME_TAGLINE_KEY, self.tagline.text(), "tagline"))
        # The description is saved when typing pauses (one undo step per pause).
        self._description_timer = QTimer(self, singleShot=True, interval=800)
        self._description_timer.timeout.connect(lambda: self._text_edited(
            GAME_DESCRIPTION_KEY, self.description.toPlainText(), "description"))
        self.description.textChanged.connect(self._description_typed)
        self.game_version.editingFinished.connect(self._version_edited)
        self.max_players.editingFinished.connect(self._players_edited)

    def set_project(self, project: Project | None) -> None:
        self._description_timer.stop()
        if self.project is not None:
            self.project.config_changed.disconnect(self.refresh)
            self.project.string_changed.disconnect(self.refresh)
            self.project.game_settings_changed.disconnect(self.refresh)
        self.project = project
        if project is not None:
            project.config_changed.connect(self.refresh)
            project.string_changed.connect(self.refresh)
            project.game_settings_changed.connect(self.refresh)
            self.refresh()

    def refresh(self, *_args) -> None:
        p = self.project
        if p is None:
            return
        self.heading.setText(f"<h2>{p.game_title}</h2>" +
                             (f"<p><i>{p.game_tagline}</i></p>" if p.game_tagline else ""))
        for field, value in ((self.title, p.game_title), (self.tagline, p.game_tagline)):
            if not field.hasFocus():
                field.setText(value)
        if not self.description.hasFocus() and \
                self.description.toPlainText() != p.game_description:
            self.description.blockSignals(True)
            self.description.setPlainText(p.game_description)
            self.description.blockSignals(False)
        self.game_version.setText(p.config.get("game_version", "0.1.0"))
        self.max_players.setValue(int(p.config.get("max_players", 4)))
        self.world_mode.setText(WORLD_MODES.get(p.config.get("world_mode"), "?") +
                                "  (chosen when the project was created)")
        self.folder.setText(str(p.root))
        self.error_log.blockSignals(True)
        self.error_log.setChecked(bool(p.config.get("error_log", True)))
        self.error_log.blockSignals(False)
        self.show_credits()
        self.logo.refresh()
        self.icon.refresh()
        self.show_game_settings()

    def show_game_settings(self) -> None:
        """The game's screen settings, with the GUI pictures and playlists to choose from."""
        p = self.project
        if p is None:
            return
        settings = menus.game_settings(p)
        pictures = sorted(menus.gui_images(p).items(), key=lambda kv: kv[1].lower())
        for key, box in (("crosshair", self.crosshair), ("pointer", self.pointer)):
            box.clear()
            box.addItem("None", "")
            for ref, label in pictures:
                box.addItem(label, ref)
            chosen = settings.get(key, "")
            if chosen and box.findData(chosen) < 0:
                box.addItem(f"(missing) {chosen[3:]}", chosen)
            box.setCurrentIndex(max(0, box.findData(chosen)))
            decorate_pictures(box, lambda ref: menus.image_file(p, ref))
        self.menu_music.clear()
        self.menu_music.addItem("None", "")
        for playlist in music.load_all(p):
            self.menu_music.addItem(playlist.get("name", playlist["id"]), playlist["id"])
        self.menu_music.setCurrentIndex(max(0, self.menu_music.findData(
            settings.get("menu_music", ""))))
        self.crosshair_views.setCurrentIndex(max(0, self.crosshair_views.findData(
            settings.get("crosshair_views", "first"))))
        self.crosshair_size.setValue(int(settings.get("crosshair_size", 32)))
        self.window_mode.setCurrentIndex(max(0, self.window_mode.findData(
            settings.get("window", "windowed"))))

    def _set_game(self, key: str, value) -> None:
        """One of the game's screen settings: one undo step."""
        if self.project is None:
            return
        settings = menus.game_settings(self.project)
        defaults = {"crosshair_views": "first", "crosshair_size": 32, "window": "windowed"}
        if value in ("", None) or defaults.get(key) == value:
            if key not in settings:
                return
            settings.pop(key)
        elif settings.get(key) == value:
            return
        else:
            settings[key] = value
        names = {"crosshair": "crosshair", "pointer": "menu pointer",
                 "menu_music": "menu music", "crosshair_views": "when the crosshair shows",
                 "crosshair_size": "crosshair size", "window": "window"}
        self.project.undo_stack.push(menus.SetGameSettingsCommand(
            self.project, settings, f"Set the {names[key]}"))

    def _error_log_toggled(self, on: bool) -> None:
        if self.project is not None and bool(self.project.config.get("error_log", True)) != on:
            self.project.set_config("error_log", on)

    def showEvent(self, e) -> None:  # noqa: N802 (Qt's name)
        if self.project is not None:  # (the scenes may use other assets by now)
            self.show_credits()
            self.show_game_settings()  # (and the library hold other pictures)
        super().showEvent(e)

    def show_credits(self) -> None:
        """The credits the game needs (the library's CC BY assets its scenes use)."""
        from .. import asset_library
        from ..cook import used_mesh_assets
        # (Not the models' own textures: they come with the models, and can take a minute to
        # make the first time.)
        used = asset_library.used_by(self.project, used_mesh_assets(self.project, False))
        needed = asset_library.credits(used)
        if needed:
            self.credits.setText("<br>".join(needed))
        elif used:
            self.credits.setText(f"None needed: the {len(used)} library asset"
                                 f"{'' if len(used) == 1 else 's'} the game uses "
                                 f"{'is' if len(used) == 1 else 'are'} CC0.")
        else:
            self.credits.setText("None: the game uses no CC BY assets.")

    # ---- edits ----------------------------------------------------------------------------
    def _text_edited(self, key: str, text: str, label: str, required: bool = False) -> None:
        if self.project is None:
            return
        text = text.strip() if key != GAME_DESCRIPTION_KEY else text.rstrip()
        current = self.project.string(key) or ""
        if (required and not text) or text == current:
            if required and not text:
                self.refresh()  # a game must have a title
            return
        self.project.undo_stack.push(SetStringCommand(self.project, key, text, label))

    def _description_typed(self) -> None:
        self._description_timer.start()

    def _version_edited(self) -> None:
        text = ".".join(str(int(p)) for p in self.game_version.text().split("."))
        if self.project and text != self.project.config.get("game_version"):
            self.project.undo_stack.push(
                SetConfigCommand(self.project, "game_version", text, "game version"))

    def _players_edited(self) -> None:
        value = self.max_players.value()
        if self.project and value != self.project.config.get("max_players"):
            self.project.undo_stack.push(
                SetConfigCommand(self.project, "max_players", value, "max players"))

    def import_image(self, kind: str, source: Path) -> None:
        try:
            value, warnings = branding.import_image(self.project, kind, source)
        except branding.BrandingError as e:
            QMessageBox.warning(self, branding.KINDS[kind], str(e))
            return
        self.set_branding(value, kind)
        if warnings:
            QMessageBox.information(self, branding.KINDS[kind], "\n\n".join(warnings))

    def set_branding(self, value: dict, kind: str) -> None:
        if self.project is not None and value != (self.project.config.get("branding") or {}):
            self.project.undo_stack.push(SetConfigCommand(
                self.project, "branding", value, branding.KINDS[kind].lower()))
