# SPDX-License-Identifier: Apache-2.0
"""The Asset Library window (milestone 5.13a; Assets ▸ Asset Library): the project's library
(asset_library.py) in its own window. The asset types on the left; on the right the selected
type's assets in their folders (textures by kind, sounds by album), and below them the selected
asset: what it is, its licence and credit, its files and anything wrong with it. Import brings
files, a folder or a zip in (copied, never linked), with a description and licence; Details
changes those; Remove deletes an asset's folder, after asking (it can't be undone)."""
import contextlib
import logging
import os
import sys
from pathlib import Path

from PySide6.QtCore import QSize, Qt, QTimer, Signal
from PySide6.QtGui import QIcon, QImage, QPixmap
from PySide6.QtWidgets import (
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFileDialog,
    QFormLayout,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QSizePolicy,
    QSlider,
    QSplitter,
    QTableWidget,
    QTableWidgetItem,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

from .. import asset_library as lib
from ..project import Project

log = logging.getLogger(__name__)

THUMB = 48
ASSET_ROLE = Qt.ItemDataRole.UserRole


def size_text(size: int) -> str:
    """'3.4 MB', '820 KB', '96 bytes'."""
    if size >= 1_000_000:
        return f"{size / 1e6:.1f} MB"
    if size >= 1_000:
        return f"{size / 1e3:.0f} KB"
    return f"{size} bytes"


def licence_label(code: str) -> str:
    return lib.LICENCES.get(code, (code or "none", "", False))[0]


class AssetDetailsForm(QWidget):
    """An asset's name, kind, description and licence (Import's and Details' fields)."""
    changed = Signal()

    def __init__(self, type_: str = "models", asset: lib.Asset | None = None, parent=None):
        super().__init__(parent)
        form = QFormLayout(self)
        form.setContentsMargins(0, 0, 0, 0)
        self.type = QComboBox()
        for key, (label, _kinds) in lib.TYPES.items():
            self.type.addItem(label, key)
        self.type.setCurrentIndex(max(0, self.type.findData(asset.type if asset else type_)))
        self.type.setEnabled(asset is None)  # (an asset stays the type it is)
        self.type.currentIndexChanged.connect(lambda _i: self._kinds())
        form.addRow("Type", self.type)
        self.category = QComboBox()
        self.category_label = QLabel("Kind")
        form.addRow(self.category_label, self.category)
        self.name = QLineEdit(asset.name if asset else "")
        self.name.setPlaceholderText("What it is called in the library")
        form.addRow("Name", self.name)
        self.description = QPlainTextEdit(asset.description if asset else "")
        self.description.setMaximumHeight(70)
        form.addRow("Description", self.description)
        self.licence = QComboBox()
        for code, (label, _deed, _credit) in lib.LICENCES.items():
            self.licence.addItem(label, code)
        self.licence.setCurrentIndex(max(0, self.licence.findData(
            asset.licence if asset else "CC0-1.0")))
        self.licence.currentIndexChanged.connect(lambda _i: self._credit_fields())
        form.addRow("Licence", self.licence)
        self.author = QLineEdit(asset.author if asset else "")
        self.title = QLineEdit(asset.title if asset else "")
        self.title.setPlaceholderText("The work's own title (for its credit)")
        self.source = QLineEdit(asset.source if asset else "")
        self.source.setPlaceholderText("Where it came from: a web page")
        form.addRow("Author", self.author)
        form.addRow("Title", self.title)
        form.addRow("Source", self.source)
        self.note = QLabel()
        self.note.setWordWrap(True)
        self.note.setEnabled(False)
        form.addRow("", self.note)
        self._kinds(asset.category if asset else "")
        self.category.setEnabled(asset is None)  # (and in its folder)
        self._credit_fields()

    def _kinds(self, current: str = "") -> None:
        type_ = self.type.currentData()
        kinds = lib.TYPES[type_][1]
        self.category.clear()
        self.category.setEditable(type_ in lib.ALBUMS)
        if kinds:
            for k in kinds:
                self.category.addItem(k.capitalize(), k)
            self.category.setCurrentIndex(max(0, self.category.findData(current or kinds[0])))
        elif type_ in lib.ALBUMS:
            self.category.setEditText(current)
            self.category.lineEdit().setPlaceholderText("Its album (empty: miscellaneous)")
        self.category_label.setText("Album" if type_ in lib.ALBUMS else "Kind")
        visible = bool(kinds) or type_ in lib.ALBUMS
        self.category.setVisible(visible)
        self.category_label.setVisible(visible)

    def _credit_fields(self) -> None:
        credited = lib.LICENCES[self.licence.currentData()][2]
        self.note.setText("CC BY: its author, and its title or source, are needed; the game's "
                          "credits name it." if credited else
                          "CC0: no credit is needed (naming the author is kind all the same).")

    def values(self) -> dict:
        type_ = self.type.currentData()
        if type_ in lib.ALBUMS:
            category = self.category.currentText().strip()
        else:
            category = self.category.currentData() or ""
        return {"type": type_, "category": category, "name": self.name.text().strip(),
                "description": self.description.toPlainText().strip(),
                "licence": self.licence.currentData(), "author": self.author.text().strip(),
                "title": self.title.text().strip(), "source": self.source.text().strip()}


class ImportDialog(QDialog):
    """Files, a folder or a zip into the library, with what they are and their licence."""

    def __init__(self, project: Project, type_: str = "models", parent=None):
        super().__init__(parent)
        self.setWindowTitle("Import Into the Library")
        self.project = project
        self.asset: lib.Asset | None = None
        self.imported: lib.ImportResult | None = None
        self.last_report = ""
        layout = QVBoxLayout(self)
        layout.addWidget(QLabel("Files (copied into the library: the originals stay where "
                                "they are)"))
        self.sources = QListWidget()
        self.sources.setMaximumHeight(110)
        layout.addWidget(self.sources)
        row = QHBoxLayout()
        for label, slot in (("Add Files…", self._add_files), ("Add Folder…", self._add_folder),
                            ("Add Zip…", self._add_zip)):
            button = QPushButton(label)
            button.clicked.connect(slot)
            row.addWidget(button)
        clear = QPushButton("Clear")
        clear.clicked.connect(self.sources.clear)
        clear.clicked.connect(self.refresh_pictures)
        row.addWidget(clear)
        layout.addLayout(row)
        self.details = AssetDetailsForm(type_)
        layout.addWidget(self.details)
        # A texture set: each picture (by its own name) and its role; saved <name>_<role>.
        self.pictures_label = QLabel("Each picture's role (saved as the asset's name and its "
                                     "role; those not used are left out):")
        self.pictures_label.setWordWrap(True)
        self.pictures = QTableWidget(0, 2)
        self.pictures.setHorizontalHeaderLabels(["Picture", "Role"])
        self.pictures.horizontalHeader().setSectionResizeMode(
            0, QHeaderView.ResizeMode.Stretch)
        self.pictures.verticalHeader().setVisible(False)
        layout.addWidget(self.pictures_label)
        layout.addWidget(self.pictures)
        self.details.type.currentIndexChanged.connect(lambda _i: self.refresh_pictures())
        self.refresh_pictures()
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Cancel)
        self.import_button = buttons.addButton("Import", QDialogButtonBox.ButtonRole.AcceptRole)
        buttons.accepted.connect(lambda: self.do_import())
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        self.resize(520, 560)

    def refresh_pictures(self) -> None:
        """The pictures to give roles to (a texture import's), each with its guessed role."""
        texture = self.details.type.currentData() == "textures"
        found = []
        if texture and self.sources.count():
            try:
                found = lib.images_in(self.source_paths())
            except (lib.LibraryError, OSError):
                found = []
        self.pictures.setRowCount(len(found))
        for row, rel in enumerate(found):
            item = QTableWidgetItem(rel)
            item.setFlags(item.flags() & ~Qt.ItemFlag.ItemIsEditable)
            self.pictures.setItem(row, 0, item)
            role = lib.guess_role(rel) or ("color" if len(found) == 1 else "")
            combo = QComboBox()
            for key, label in lib.TEXTURE_ROLES.items():
                combo.addItem(label, key)
            combo.setCurrentIndex(combo.findData(role))
            self.pictures.setCellWidget(row, 1, combo)
        show = texture and bool(found)
        self.pictures.setVisible(show)
        self.pictures_label.setVisible(show)

    def roles(self) -> dict[str, str] | None:
        """Each picture's role, as chosen (None: not a texture import)."""
        if self.details.type.currentData() != "textures" or not self.pictures.rowCount():
            return None
        return {self.pictures.item(r, 0).text(): self.pictures.cellWidget(r, 1).currentData()
                for r in range(self.pictures.rowCount())}

    def set_role(self, picture: str, role: str) -> None:
        for r in range(self.pictures.rowCount()):
            if self.pictures.item(r, 0).text() == picture:
                combo = self.pictures.cellWidget(r, 1)
                combo.setCurrentIndex(combo.findData(role))

    def add_sources(self, paths) -> None:
        for p in paths:
            self.sources.addItem(str(p))
        if not self.details.name.text() and paths:  # (named after the first, to begin with)
            stem = Path(paths[0]).stem.replace("_", " ").replace("-", " ").strip()
            self.details.name.setText(stem[:1].upper() + stem[1:])
        self.refresh_pictures()

    def _add_files(self) -> None:
        files, _ = QFileDialog.getOpenFileNames(self, "Files to import")
        self.add_sources(files)

    def _add_folder(self) -> None:
        folder = QFileDialog.getExistingDirectory(self, "A folder to import")
        if folder:
            self.add_sources([folder])

    def _add_zip(self) -> None:
        file, _ = QFileDialog.getOpenFileName(self, "A zip to import", "", "Zip files (*.zip)")
        if file:
            self.add_sources([file])

    def source_paths(self) -> list[Path]:
        return [Path(self.sources.item(i).text()) for i in range(self.sources.count())]

    def ask_split(self, models: int) -> bool:
        """A pack of several models: one asset, or one for each? (True: one each.)"""
        box = QMessageBox(QMessageBox.Icon.Question, "Import",
                          f"These files hold {models} models. Import them as one asset, or as "
                          f"one asset for each model (each with its own textures, and the "
                          f"pack's licence)?", parent=self)
        each = box.addButton(f"One for Each ({models})", QMessageBox.ButtonRole.YesRole)
        box.addButton("One Asset", QMessageBox.ButtonRole.NoRole)
        box.setDefaultButton(each)
        box.exec()
        return box.clickedButton() is each

    def do_import(self, split: bool | None = None) -> bool:
        """Imports what is chosen (asking, for a pack of models, whether to split it unless
        `split` says), then says what came in and what was left out."""
        v = self.details.values()
        sources = self.source_paths()
        if split is None:
            models = lib.model_count(sources) if v["type"] in ("models", "objects") else 0
            split = models > 1 and self.ask_split(models)
        try:
            self.imported = lib.import_assets(
                self.project, sources, v["type"], v["name"], category=v["category"],
                description=v["description"], licence=v["licence"], author=v["author"],
                source=v["source"], title=v["title"], split=split, roles=self.roles())
        except (lib.LibraryError, OSError) as e:
            QMessageBox.warning(self, "Import", f"Not imported: {e}")
            return False
        result = self.imported
        if not result.assets:
            why = result.refused[0][1] if result.refused else "there is nothing to import"
            if result.skipped:
                why += f" ({lib.skipped_summary(result.skipped)})"
            QMessageBox.warning(self, "Import", f"Not imported: {why}")
            return False
        self.asset = result.assets[0]
        for asset in result.assets:
            log.info("Imported '%s' into the library (%s)", asset.name, asset.id)
        if result.skipped or result.refused or result.warnings:
            log.warning("Import: %s", result.summary())
            for rel, why in result.skipped:
                log.info("Import: skipped %s (%s)", rel, why)
            for warning in result.warnings:
                log.warning("Import: %s", warning)
            self.report(result)
        self.accept()
        return True

    def report(self, result) -> None:
        """What came in, and what was left out (and why)."""
        lines = [result.summary()]
        if result.skipped:
            lines += ["", "Left out:"] + [f"  {rel}: {why}" for rel, why in result.skipped[:30]]
            if len(result.skipped) > 30:
                lines.append(f"  … and {len(result.skipped) - 30} more (in the Log)")
        if result.warnings:
            lines += ["", "To know:"] + [f"  {w}" for w in result.warnings]
        box = QMessageBox(QMessageBox.Icon.Information, "Import", "\n".join(lines),
                          parent=self)
        box.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.last_report = box.text()
        if not getattr(self, "quiet", False):
            box.exec()


class DetailsDialog(QDialog):
    """An asset's name, description and licence, changed."""

    def __init__(self, asset: lib.Asset, parent=None):
        super().__init__(parent)
        self.setWindowTitle(f"Details - {asset.name}")
        self.asset = asset
        layout = QVBoxLayout(self)
        self.details = AssetDetailsForm(asset=asset)
        layout.addWidget(self.details)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Save |
                                   QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self.save)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def save(self) -> bool:
        v = self.details.values()
        changed = lib.Asset(**{**self.asset.__dict__, **{k: v[k] for k in (
            "name", "description", "licence", "author", "title", "source")}})
        said = [p for p in lib.problems(changed) if "licence" in p or "credit" in p]
        if not changed.name:
            said.append("it needs a name")
        if said:
            QMessageBox.warning(self, "Details", "Not saved: " + "; ".join(said))
            return False
        lib.write(changed)
        self.asset = changed
        self.accept()
        return True


# ---- the player (step 4) -------------------------------------------------------------------------
PLAYER_SIZE = (320, 240)
MODEL_KEY = "library-model"


def model_bounds(glb: bytes) -> tuple[list[float], list[float]] | None:
    """A glTF binary's extent, from its meshes' positions' min and max (its nodes' moves left
    out: enough to frame it)."""
    import json
    import struct
    try:
        length = struct.unpack_from("<I", glb, 12)[0]
        doc = json.loads(glb[20:20 + length])
    except (struct.error, ValueError):
        return None
    lo, hi = [float("inf")] * 3, [float("-inf")] * 3
    for mesh in doc.get("meshes", []):
        for prim in mesh.get("primitives", []):
            index = prim.get("attributes", {}).get("POSITION")
            if index is None or index >= len(doc.get("accessors", [])):
                continue
            acc = doc["accessors"][index]
            if "min" in acc and "max" in acc:
                lo = [min(a, b) for a, b in zip(lo, acc["min"], strict=False)]
                hi = [max(a, b) for a, b in zip(hi, acc["max"], strict=False)]
    return (lo, hi) if lo[0] != float("inf") else None


class AssetPlayer(QWidget):
    """The selected asset, shown or played: a picture; a model turning (a skinned one playing
    its clips) through the engine; a sound; a font's sample."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMinimumWidth(PLAYER_SIZE[0])
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        self.picture = QLabel()
        self.picture.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.picture.setMinimumSize(*PLAYER_SIZE)
        # What to show, when the asset holds more than one thing (and how: a material's shape).
        top = QHBoxLayout()
        self.choose = QComboBox()
        self.choose.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Fixed)
        self.choose.setToolTip("Which of the asset's files to show or play")
        self.choose.currentIndexChanged.connect(lambda _i: self.show_choice())
        top.addWidget(self.choose, 1)
        self.shape = QComboBox()
        self.shape.addItem("Sphere", "prim_sphere")
        self.shape.addItem("Tile", "prim_plane")
        self.shape.currentIndexChanged.connect(
            lambda _i: self.kind == "material" and self._show_material())
        top.addWidget(self.shape)
        layout.addLayout(top)
        layout.addWidget(self.picture, 1)
        controls = QHBoxLayout()
        self.play_button = QPushButton("Play")
        self.play_button.setCheckable(True)
        self.play_button.toggled.connect(self._play)
        controls.addWidget(self.play_button)
        self.clip = QComboBox()
        self.clip.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Fixed)
        self.clip.currentIndexChanged.connect(lambda _i: self._show_model_clip())
        controls.addWidget(self.clip, 1)
        self.position = QSlider(Qt.Orientation.Horizontal)
        self.position.sliderMoved.connect(self._seek)
        controls.addWidget(self.position, 1)
        self.time = QLabel()
        controls.addWidget(self.time)
        layout.addLayout(controls)
        self.kind = ""  # "", "picture", "model", "sound", "font"
        self.asset: lib.Asset | None = None
        self._view = None
        self._angle = 0.0
        self._frame = None  # (centre, size) of the model shown
        self._skinned = False
        self._audio = None
        self.timer = QTimer(self)
        self.timer.setInterval(33)
        self.timer.timeout.connect(lambda: self.tick(0.033))
        self.show_asset(None)

    # ---- what it shows ----------------------------------------------------------------------
    def show_asset(self, asset: lib.Asset | None) -> None:
        """An asset: the list of what it holds that can be shown (when more than one), and
        the first of them shown."""
        self.stop()
        self.asset = asset
        self.choose.blockSignals(True)
        self.choose.clear()
        for label, kind, path in self.choices(asset):
            self.choose.addItem(label, (kind, path))
        self.choose.blockSignals(False)
        # (Sounds are chosen from the window's list of files, with their sizes.)
        self.choose.setVisible(self.choose.count() > 1 and
                               (asset is None or asset.type not in lib.ALBUMS))
        self.show_choice()

    def choose_file(self, path: Path) -> None:
        """One of the asset's files, shown (or ready to play)."""
        for i in range(self.choose.count()):
            data = self.choose.itemData(i)
            if data and data[1] is not None and Path(data[1]) == Path(path):
                if i == self.choose.currentIndex():
                    return
                self.choose.setCurrentIndex(i)  # (shows it)
                return

    def choices(self, asset: lib.Asset | None) -> list[tuple[str, str, Path | None]]:
        """What can be shown of an asset: (label, kind, file). A texture set: its whole
        material first, then each picture by its role."""
        if asset is None:
            return []
        files = [p for p in asset.file_paths() if p.is_file()]
        out: list[tuple[str, str, Path | None]] = []
        if asset.type == "textures":
            roles = {p: r for r, p in lib.texture_set(asset).items()}
            if roles:
                out.append(("Material (all maps)", "material", None))
            for p in files:
                if p.suffix.lower() in (".png", ".jpg", ".jpeg"):
                    role = lib.TEXTURE_ROLES.get(roles.get(p, ""), "")
                    label = f"{role}: {p.name}" if p in roles else p.name
                    out.append((label, "picture", p))
            return out
        for p in files:
            ext = p.suffix.lower()
            if asset.type in ("models", "objects") and ext in lib.MODEL_SUFFIXES:
                out.append((p.name, "model", p))
            elif ext in lib.AUDIO:
                out.append((p.name, "sound", p))
            elif asset.type == "fonts" and ext in (".ttf", ".otf"):
                out.append((p.name, "font", p))
        if not out:  # (pictures, when that is all it has)
            out = [(p.name, "picture", p) for p in files
                   if p.suffix.lower() in (".png", ".jpg", ".jpeg")]
        return out

    def show_choice(self) -> None:
        """What is chosen in the list, shown (or played)."""
        self.stop()
        self.kind = ""
        self.picture.clear()
        self.clip.clear()
        choice = self.choose.currentData()
        if self.asset is None:
            self.picture.setText("")
        elif choice is None:
            self.picture.setText("(nothing to show)")
        else:
            kind, path = choice
            {"model": self._show_model, "sound": self._show_sound, "font": self._show_font,
             "picture": self._show_picture}.get(kind, lambda _p: self._show_material())(path)
        self.play_button.setVisible(self.kind in ("model", "sound"))
        self.clip.setVisible(self.kind == "model" and self.clip.count() > 0)
        self.position.setVisible(self.kind == "sound")
        self.time.setVisible(self.kind == "sound")
        self.shape.setVisible(self.kind == "material")

    def _show_material(self) -> None:
        """A texture set as a material on a sphere (or a tile), every picture in its role."""
        from types import SimpleNamespace

        from .. import meshes, objects, textures
        library = next((p for p in self.asset.folder.parents if p.name == lib.FOLDER), None)
        if library is None:
            self.picture.setText("(not in a library)")
            return
        root = library.parent
        slots = lib.material_textures(SimpleNamespace(root=root), self.asset)
        renderer = objects.shared_renderer()
        shape = self.shape.currentData()
        material = {"color": [1.0, 1.0, 1.0, 1.0], "roughness": 1.0, "textures": slots}
        try:
            renderer.send(shape, meshes.to_glb(meshes.primitive(shape)))
            for texture in slots.values():
                data = textures.texture_data(root, texture)
                if data is not None:
                    renderer.send(texture, data)
        except RuntimeError as e:
            self.picture.setText(f"Not shown: {e}")
            return
        flat = shape == "prim_plane"
        image = renderer.render("library-material",
                                [{"name": "preview", "components": {"mesh": {"asset": shape},
                                                                    "material": material}}],
                                [-0.5, -0.5, -0.5, 0.5, 0.5, 0.5], *PLAYER_SIZE,
                                yaw=0.0 if flat else 35.0, pitch=60.0 if flat else 22.0,
                                zoom=0.8 if flat else 1.0)
        if image is None:  # (no engine here: its colour picture, as it is)
            colour = lib.texture_set(self.asset).get("color")
            if colour is not None:
                self._show_picture(colour)
            else:
                self.picture.setText(f"Not shown: {renderer.error or 'nothing to draw'}")
            return
        self.kind = "material"
        self.picture.setPixmap(QPixmap.fromImage(image))

    def _show_picture(self, path: Path) -> None:
        pixmap = QPixmap(str(path))
        if pixmap.isNull():
            self.picture.setText(f"{path.name} can't be shown")
            return
        self.kind = "picture"
        self.picture.setPixmap(pixmap.scaled(*PLAYER_SIZE, Qt.AspectRatioMode.KeepAspectRatio,
                                             Qt.TransformationMode.SmoothTransformation))

    def _show_font(self, path: Path) -> None:
        from PySide6.QtGui import QFont, QFontDatabase
        family_id = QFontDatabase.addApplicationFont(str(path))
        families = QFontDatabase.applicationFontFamilies(family_id) if family_id >= 0 else []
        if not families:
            self.picture.setText(f"{path.name} can't be read as a font")
            return
        self.kind = "font"
        self.picture.setFont(QFont(families[0], 20))
        self.picture.setWordWrap(True)
        self.picture.setText(f"{families[0]}\nThe quick brown fox jumps over the lazy dog.\n"
                             "0123456789")

    def _show_sound(self, path: Path) -> None:
        from PySide6.QtCore import QUrl
        try:
            from PySide6.QtMultimedia import QAudioOutput, QMediaPlayer
        except ImportError:
            self.picture.setText("(sounds can't be played here)")
            return
        self.kind = "sound"
        if self._audio is None:
            player, output = QMediaPlayer(self), QAudioOutput(self)
            player.setAudioOutput(output)
            player.durationChanged.connect(lambda ms: self.position.setRange(0, int(ms)))
            player.positionChanged.connect(self._audio_moved)
            self._audio = (player, output)
        self._audio[0].setSource(QUrl.fromLocalFile(str(path)))
        self.picture.setText(f"♪  {path.name}")
        self._audio_moved(0)

    def _audio_moved(self, ms: int) -> None:
        if not self.position.isSliderDown():
            self.position.setValue(int(ms))
        total = self._audio[0].duration() if self._audio else 0
        self.time.setText(f"{ms / 1000:.1f} / {total / 1000:.1f} s")

    def _seek(self, ms: int) -> None:
        if self._audio is not None:
            self._audio[0].setPosition(ms)

    def _show_model(self, path: Path) -> None:
        import json

        from .. import engine
        from ..gltf_pack import PackError, pack
        midgard = engine.module()
        if midgard is None:
            self.picture.setText("(models are shown by the engine, not built here)")
            return
        try:
            glb = pack(path) if path.suffix.lower() == ".gltf" else path.read_bytes()
        except (OSError, PackError) as e:
            self.picture.setText(f"{path.name} can't be read: {e}")
            return
        try:
            if self._view is None:
                self._view = midgard.Viewport()
                self._view.attach_offscreen(*PLAYER_SIZE)
                self._view.set_settings(json.dumps({"sky": [0.16, 0.18, 0.22]}))
                self._view.put_entity(json.dumps({"id": "sun", "name": "Sun", "components": {
                    "transform": {"rotation": [-40, 30, 0]}, "light": {"type": "directional"}}}))
            self._view.remove_entity("model")
            self._view.set_mesh(MODEL_KEY, glb)
        except RuntimeError as e:  # (no Vulkan, or not a model)
            self._view = None if "Vulkan" in str(e) else self._view
            self.picture.setText(f"Not shown: {e}")
            return
        self.kind = "model"
        clips = []
        try:
            clips = [c["name"] for c in midgard.skinned_info(glb)["clips"]]
            self._skinned = True
        except ValueError:
            self._skinned = False
        for name in clips:
            self.clip.addItem(name, name)
        bounds = model_bounds(glb) or ([-0.5, 0, -0.5], [0.5, 1, 0.5])
        lo, hi = bounds
        centre = [(a + b) / 2 for a, b in zip(lo, hi, strict=True)]
        size = max(max(b - a for a, b in zip(lo, hi, strict=True)), 0.01)
        self._frame = (centre, size)
        self._angle = 0.0
        self._show_model_clip()

    def _show_model_clip(self) -> None:
        import json
        if self.kind != "model" or self._view is None:
            return
        components = {"transform": {}, "material": {"color": [0.85, 0.85, 0.85, 1]}}
        if self._skinned:
            components["character"] = {"model": MODEL_KEY, "clip": self.clip.currentData() or ""}
        else:
            components["mesh"] = {"asset": MODEL_KEY}
        self._view.put_entity(json.dumps({"id": "model", "name": "Model",
                                          "components": components}))
        self.tick(0.0)

    # ---- playing ----------------------------------------------------------------------------
    def _play(self, on: bool) -> None:
        self.play_button.setText("Stop" if on else "Play")
        if self.kind == "sound" and self._audio is not None:
            (self._audio[0].play if on else self._audio[0].stop)()
        elif self.kind == "model":
            (self.timer.start if on else self.timer.stop)()

    def stop(self) -> None:
        self.timer.stop()
        if self._audio is not None:
            self._audio[0].stop()
        self.play_button.blockSignals(True)
        self.play_button.setChecked(False)
        self.play_button.setText("Play")
        self.play_button.blockSignals(False)

    def tick(self, dt: float) -> None:
        """The model moved on by dt (its clip, and the camera round it) and drawn."""
        import math
        if self.kind != "model" or self._view is None or self._frame is None:
            return
        (cx, cy, cz), size = self._frame
        self._angle += dt * 0.6
        distance = size * 1.9
        eye = [cx + math.sin(self._angle) * distance, cy + size * 0.35,
               cz + math.cos(self._angle) * distance]
        try:
            self._view.set_camera(eye, [cx, cy, cz], 40.0)
            self._view.animate(dt)
            self._view.render()
            pixels = self._view.read_pixels()
        except RuntimeError as e:
            self.picture.setText(f"Not shown: {e}")
            return
        image = QImage(pixels, *PLAYER_SIZE, QImage.Format.Format_RGBA8888).copy()
        self.picture.setPixmap(QPixmap.fromImage(image))

    def drop(self) -> None:
        """Its engine view let go (the window closing)."""
        self.stop()
        if self._view is not None:
            with contextlib.suppress(RuntimeError):
                self._view.detach()
            self._view = None


class LibraryPicker(QDialog):
    """Choose a file from the Asset Library: the assets of some types with files of some
    endings, each shown in the player as it is picked."""

    def __init__(self, project: Project, title: str, types: tuple[str, ...],
                 suffixes: tuple[str, ...], parent=None):
        super().__init__(parent)
        self.setWindowTitle(title)
        self.project = project
        self.chosen: Path | None = None
        self.choices = lib.files_of(project, types, suffixes)
        layout = QVBoxLayout(self)
        row = QHBoxLayout()
        self.list = QListWidget()
        self.list.setIconSize(QSize(THUMB, THUMB))
        for asset, path in self.choices:
            item = QListWidgetItem(f"{asset.name}  ({path.name})\n{licence_label(asset.licence)}")
            if path.suffix.lower() in (".png", ".jpg", ".jpeg"):
                pixmap = QPixmap(str(path))
                if not pixmap.isNull():
                    item.setIcon(QIcon(pixmap.scaled(THUMB, THUMB,
                                                     Qt.AspectRatioMode.KeepAspectRatio)))
            self.list.addItem(item)
        self.list.currentRowChanged.connect(self._picked)
        self.list.itemDoubleClicked.connect(lambda _i: self.accept_current())
        row.addWidget(self.list, 1)
        self.player = AssetPlayer()
        row.addWidget(self.player)
        layout.addLayout(row)
        if not self.choices:
            empty = QLabel("The library has none yet: Assets ▸ Import Into Library brings them "
                           "in.")
            empty.setWordWrap(True)
            layout.addWidget(empty)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Cancel)
        self.use_button = buttons.addButton("Use", QDialogButtonBox.ButtonRole.AcceptRole)
        self.use_button.setEnabled(False)
        buttons.accepted.connect(self.accept_current)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        self.resize(760, 420)

    def _picked(self, row: int) -> None:
        self.use_button.setEnabled(row >= 0)
        self.player.show_asset(self.choices[row][0] if row >= 0 else None)

    def pick(self, row: int) -> None:
        self.list.setCurrentRow(row)

    def accept_current(self) -> None:
        row = self.list.currentRow()
        if row < 0:
            return
        self.chosen = self.choices[row][1]
        self.player.drop()
        self.accept()

    def done(self, result: int) -> None:
        self.player.drop()
        super().done(result)


class AssetWindow(QWidget):
    """The library's window (one for the editor; it follows the open project)."""

    def __init__(self, parent=None):
        super().__init__(parent, Qt.WindowType.Window)
        self.setWindowTitle("Asset Library")
        self.resize(1000, 680)
        self.project: Project | None = None
        self.assets: list[lib.Asset] = []
        self.unreadable: list[str] = []
        outer = QVBoxLayout(self)
        bar = QHBoxLayout()
        self.title = QLabel()
        bar.addWidget(self.title, 1)
        self.import_button = QPushButton("Import…")
        self.import_button.clicked.connect(self.import_dialog)
        self.details_button = QPushButton("Details…")
        self.details_button.clicked.connect(self.edit_details)
        self.remove_button = QPushButton("Remove")
        self.remove_button.clicked.connect(self.remove_selected)
        self.show_button = QPushButton("Show in Folder")
        self.show_button.clicked.connect(self.show_in_folder)
        for b in (self.import_button, self.details_button, self.remove_button, self.show_button):
            bar.addWidget(b)
        outer.addLayout(bar)
        split = QSplitter(Qt.Orientation.Horizontal)
        self.types = QListWidget()
        self.types.setMaximumWidth(200)
        for key, (label, _kinds) in lib.TYPES.items():
            item = QListWidgetItem(label)
            item.setData(ASSET_ROLE, key)
            self.types.addItem(item)
        self.types.currentRowChanged.connect(lambda _r: self.show_type())
        split.addWidget(self.types)
        right = QSplitter(Qt.Orientation.Vertical)
        self.tree = QTreeWidget()
        self.tree.setHeaderLabels(["Name", "Licence", "Files", "Description"])
        self.tree.setIconSize(QSize(THUMB, THUMB))
        self.tree.setColumnWidth(0, 260)
        self.tree.itemSelectionChanged.connect(self.show_selected)
        self.tree.itemDoubleClicked.connect(lambda *_: self.edit_details())
        right.addWidget(self.tree)
        self.info = QLabel()
        self.info.setWordWrap(True)
        self.info.setAlignment(Qt.AlignmentFlag.AlignTop | Qt.AlignmentFlag.AlignLeft)
        self.info.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.player = AssetPlayer()
        # A sound asset's files with their sizes: choosing one is what the player plays.
        self.files_list = QTreeWidget()
        self.files_list.setHeaderLabels(["File", "Size"])
        self.files_list.setRootIsDecorated(False)
        self.files_list.setAlternatingRowColors(True)
        self.files_list.header().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        self.files_list.header().setSectionResizeMode(1,
                                                      QHeaderView.ResizeMode.ResizeToContents)
        self.files_list.setToolTip("Choose a sound to play it (double-click: play at once)")
        self.files_list.itemSelectionChanged.connect(self._file_chosen)
        self.files_list.itemDoubleClicked.connect(lambda *_: self._play_chosen())
        details = QWidget()
        details_layout = QVBoxLayout(details)
        details_layout.setContentsMargins(0, 0, 0, 0)
        details_layout.addWidget(self.info)
        details_layout.addWidget(self.files_list, 1)
        pane = QWidget()
        pane_layout = QHBoxLayout(pane)
        pane_layout.addWidget(details, 1)
        pane_layout.addWidget(self.player)
        right.addWidget(pane)
        right.setSizes([440, 220])
        split.addWidget(right)
        split.setSizes([180, 820])
        outer.addWidget(split, 1)
        self.types.setCurrentRow(0)
        self.set_project(None)

    # ---- what it shows ----------------------------------------------------------------------
    def set_project(self, project: Project | None) -> None:
        self.project = project
        self.player.show_asset(None)
        if project is None:
            self.player.drop()
        self.refresh()

    def refresh(self) -> None:
        if self.project is None:
            self.assets, self.unreadable = [], []
            self.title.setText("No project open")
        else:
            self.assets, self.unreadable = lib.scan(self.project)
            self.title.setText(f"{self.project.name}'s library: {len(self.assets)} asset"
                               f"{'' if len(self.assets) == 1 else 's'}")
        for b in (self.import_button, self.show_button):
            b.setEnabled(self.project is not None)
        for i in range(self.types.count()):
            item = self.types.item(i)
            key = item.data(ASSET_ROLE)
            count = sum(1 for a in self.assets if a.type == key)
            item.setText(lib.TYPES[key][0] + (f"  ({count})" if count else ""))
        self.show_type()

    def current_type(self) -> str:
        item = self.types.currentItem()
        return item.data(ASSET_ROLE) if item else "models"

    def show_type(self) -> None:
        """The selected type's assets, in their folders (kinds, albums)."""
        selected = self.selected()
        self.tree.clear()
        type_ = self.current_type()
        folders: dict[str, QTreeWidgetItem] = {}
        for asset in (a for a in self.assets if a.type == type_):
            parent = None
            if asset.category:
                if asset.category not in folders:
                    folders[asset.category] = QTreeWidgetItem(
                        self.tree, [asset.category.replace("_", " ").capitalize()])
                    folders[asset.category].setExpanded(True)
                parent = folders[asset.category]
            item = QTreeWidgetItem(parent or self.tree, [
                asset.name, licence_label(asset.licence), str(len(asset.files)),
                asset.description.splitlines()[0] if asset.description else ""])
            item.setData(0, ASSET_ROLE, asset.id)
            if (icon := self.thumbnail(asset)) is not None:
                item.setIcon(0, icon)
            if lib.problems(asset):
                item.setForeground(0, Qt.GlobalColor.red)
            if selected and asset.id == selected.id:
                item.setSelected(True)
        self.show_selected()

    def thumbnail(self, asset: lib.Asset) -> QIcon | None:
        """A picture's own image (models' come with the player)."""
        for path in asset.file_paths():
            if path.suffix.lower() in (".png", ".jpg", ".jpeg") and path.is_file():
                pixmap = QPixmap(str(path))
                if not pixmap.isNull():
                    return QIcon(pixmap.scaled(THUMB, THUMB,
                                               Qt.AspectRatioMode.KeepAspectRatio,
                                               Qt.TransformationMode.SmoothTransformation))
        return None

    def selected(self) -> lib.Asset | None:
        items = self.tree.selectedItems()
        if not items:
            return None
        asset_id = items[0].data(0, ASSET_ROLE)
        return next((a for a in self.assets if a.id == asset_id), None)

    def select(self, asset_id: str) -> None:
        """Shows an asset: its type, and it selected."""
        asset = next((a for a in self.assets if a.id == asset_id), None)
        if asset is None:
            return
        for i in range(self.types.count()):
            if self.types.item(i).data(ASSET_ROLE) == asset.type:
                self.types.setCurrentRow(i)
        for item in self.tree.findItems("*", Qt.MatchFlag.MatchWildcard |
                                        Qt.MatchFlag.MatchRecursive):
            item.setSelected(item.data(0, ASSET_ROLE) == asset_id)

    def show_selected(self) -> None:
        asset = self.selected()
        shown = self.player.asset
        if (asset.id if asset else None) != (shown.id if shown else None):
            self.player.show_asset(asset)
            self.show_files(asset)
        for b in (self.details_button, self.remove_button):
            b.setEnabled(asset is not None)
        if asset is None:
            text = "Select an asset to see it." if self.project else ""
            if self.unreadable:
                text += "<br><br><b>Not read:</b><br>" + "<br>".join(self.unreadable)
            self.info.setText(text)
            return
        rows = [f"<b>{asset.name}</b> &nbsp; <i>{asset.id}</i>"]
        if asset.description:
            rows.append(asset.description.replace("\n", "<br>"))
        rows.append(f"Licence: {licence_label(asset.licence)}")
        for label, value in (("Author", asset.author), ("Title", asset.title),
                             ("Source", asset.source), ("Imported", asset.imported)):
            if value:
                rows.append(f"{label}: {value}")
        if lib.needs_credit(asset):
            rows.append(f"Credit: {lib.credit(asset)}")
        size = sum(f.get("bytes", 0) for f in asset.files)
        if asset.type in lib.ALBUMS:  # (listed below, with their sizes)
            rows.append(f"Files: {len(asset.files)}, {size_text(size)}")
        else:
            rows.append(f"Files ({len(asset.files)}, {size / 1e6:.1f} MB): " +
                        ", ".join(f["path"] for f in asset.files[:8]) +
                        (" …" if len(asset.files) > 8 else ""))
        if said := lib.problems(asset):
            rows.append("<span style='color:#d05050'>" + "<br>".join(said) + "</span>")
        self.info.setText("<br>".join(rows))

    # ---- what it does -----------------------------------------------------------------------
    def show_files(self, asset: lib.Asset | None) -> None:
        """A sound asset's files, each with its size (the sounds can be chosen to play)."""
        self.files_list.blockSignals(True)
        self.files_list.clear()
        sounds = asset is not None and asset.type in lib.ALBUMS
        self.files_list.setVisible(sounds)
        if sounds:
            first = None
            # The sounds first (by name), then what else it holds (a licence, a read-me).
            ordered = sorted(asset.files, key=lambda f: (
                Path(f["path"]).suffix.lower() not in lib.AUDIO, f["path"].lower()))
            for f in ordered:
                path = asset.folder / f["path"]
                item = QTreeWidgetItem([f["path"], size_text(f.get("bytes", 0))])
                item.setData(0, ASSET_ROLE, str(path))
                item.setTextAlignment(1, Qt.AlignmentFlag.AlignRight |
                                      Qt.AlignmentFlag.AlignVCenter)
                if path.suffix.lower() not in lib.AUDIO:  # (a licence, a read-me)
                    item.setFlags(item.flags() & ~Qt.ItemFlag.ItemIsSelectable)
                    item.setForeground(0, Qt.GlobalColor.gray)
                    item.setForeground(1, Qt.GlobalColor.gray)
                elif first is None:
                    first = item
                self.files_list.addTopLevelItem(item)
            if first is not None:
                first.setSelected(True)
        self.files_list.blockSignals(False)

    def _file_chosen(self) -> None:
        items = self.files_list.selectedItems()
        if items:
            self.player.choose_file(Path(items[0].data(0, ASSET_ROLE)))

    def _play_chosen(self) -> None:
        self._file_chosen()
        if self.player.kind == "sound":
            self.player.play_button.setChecked(True)

    def import_dialog(self) -> ImportDialog | None:
        if self.project is None:
            return None
        dialog = ImportDialog(self.project, self.current_type(), self)
        if dialog.exec() and dialog.asset is not None:
            self.refresh()
            self.select(dialog.asset.id)
        return dialog

    def edit_details(self) -> None:
        asset = self.selected()
        if asset is not None and DetailsDialog(asset, self).exec():
            self.refresh()
            self.select(asset.id)

    def remove_selected(self, confirm: bool = True) -> bool:
        asset = self.selected()
        if asset is None:
            return False
        if confirm and QMessageBox.question(
                self, "Remove", f"Remove '{asset.name}' from the library? Its folder and files "
                "are deleted (this can't be undone).") != QMessageBox.StandardButton.Yes:
            return False
        lib.remove(asset)
        log.info("Removed '%s' from the library", asset.name)
        self.refresh()
        return True

    def show_in_folder(self) -> None:
        if self.project is None:
            return
        asset = self.selected()
        folder = asset.folder if asset else lib.library_dir(self.project)
        folder.mkdir(parents=True, exist_ok=True)
        if sys.platform == "win32":
            os.startfile(folder)  # noqa: S606 (the user's own folder, in Explorer)
        else:
            import subprocess
            subprocess.Popen(["xdg-open", str(folder)])

    def closeEvent(self, e) -> None:  # noqa: N802 (Qt's name)
        self.player.drop()
        super().closeEvent(e)

    def showEvent(self, e) -> None:  # noqa: N802 (Qt's name)
        self.refresh()  # (files may have changed while it was hidden)
        super().showEvent(e)

