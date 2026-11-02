# SPDX-License-Identifier: Apache-2.0
"""The Asset Library window (milestone 5.13a; Assets ▸ Asset Library): the project's library
(asset_library.py) in its own window. The asset types on the left; on the right the selected
type's assets in their folders (textures by kind, sounds by album), and below them the selected
asset: what it is, its licence and credit, its files and anything wrong with it. Import brings
files, a folder or a zip in (copied, never linked), with a description and licence; Details
changes those; Remove deletes an asset's folder, after asking (it can't be undone)."""
import logging
import os
import sys
from pathlib import Path

from PySide6.QtCore import QSize, Qt, Signal
from PySide6.QtGui import QIcon, QPixmap
from PySide6.QtWidgets import (
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFileDialog,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QSplitter,
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
        row.addWidget(clear)
        layout.addLayout(row)
        self.details = AssetDetailsForm(type_)
        layout.addWidget(self.details)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Cancel)
        self.import_button = buttons.addButton("Import", QDialogButtonBox.ButtonRole.AcceptRole)
        buttons.accepted.connect(self.do_import)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        self.resize(520, 560)

    def add_sources(self, paths) -> None:
        for p in paths:
            self.sources.addItem(str(p))
        if not self.details.name.text() and paths:  # (named after the first, to begin with)
            stem = Path(paths[0]).stem.replace("_", " ").replace("-", " ").strip()
            self.details.name.setText(stem[:1].upper() + stem[1:])

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

    def do_import(self) -> bool:
        v = self.details.values()
        try:
            self.asset = lib.import_asset(
                self.project, self.source_paths(), v["type"], v["name"], category=v["category"],
                description=v["description"], licence=v["licence"], author=v["author"],
                source=v["source"], title=v["title"])
        except (lib.LibraryError, OSError) as e:
            QMessageBox.warning(self, "Import", f"Not imported: {e}")
            return False
        log.info("Imported '%s' into the library (%s)", self.asset.name, self.asset.id)
        self.accept()
        return True


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
        self.player = QWidget()  # (the player: step 4)
        pane = QWidget()
        pane_layout = QHBoxLayout(pane)
        pane_layout.addWidget(self.info, 1)
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
        rows.append(f"Files ({len(asset.files)}, {size / 1e6:.1f} MB): " +
                    ", ".join(f["path"] for f in asset.files[:8]) +
                    (" …" if len(asset.files) > 8 else ""))
        if said := lib.problems(asset):
            rows.append("<span style='color:#d05050'>" + "<br>".join(said) + "</span>")
        self.info.setText("<br>".join(rows))

    # ---- what it does -----------------------------------------------------------------------
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

    def showEvent(self, e) -> None:  # noqa: N802 (Qt's name)
        self.refresh()  # (files may have changed while it was hidden)
        super().showEvent(e)
