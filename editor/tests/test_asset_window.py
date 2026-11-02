# SPDX-License-Identifier: Apache-2.0
"""The Asset Library window (milestone 5.13a step 3): Assets ▸ Asset Library opens it on the
open project; the types with their counts; assets in their folders (kinds, albums) with their
licences and credits; importing with a description and licence (and refusals said); details
changed; an asset removed."""
from pathlib import Path

from PySide6.QtWidgets import QMessageBox

from midgard_editor import asset_library as lib
from midgard_editor.docks.asset_window import AssetWindow, DetailsDialog, ImportDialog

PNG = b"\x89PNG\r\n\x1a\n" + b"\x00" * 32
OGG = b"OggS" + b"\x00" * 32


def a_picture(folder: Path, name: str) -> Path:
    from PySide6.QtGui import QColor, QImage
    image = QImage(8, 8, QImage.Format.Format_RGB32)
    image.fill(QColor(sum(map(ord, name)) % 256, 80, len(name) * 9 % 256))  # (each its own)
    path = folder / name
    image.save(str(path))
    return path


def type_row(window: AssetWindow, key: str) -> int:
    return next(i for i in range(window.types.count())
                if window.types.item(i).data(0x0100) == key)


def test_the_window_shows_the_library_by_type_and_folder(project, qtbot, tmp_path):
    lib.import_asset(project, [a_picture(tmp_path, "oak.png")], "textures", "Oak Bark",
                     category="bark")
    lib.import_asset(project, [a_picture(tmp_path, "zinc.png")], "textures", "Zinc",
                     category="metal", licence="CC-BY-4.0", author="Ana Ruiz", title="Zinc")
    (tmp_path / "dawn.ogg").write_bytes(OGG)
    lib.import_asset(project, [tmp_path / "dawn.ogg"], "music", "Dawn", category="Lights")
    window = AssetWindow()
    qtbot.addWidget(window)
    window.set_project(project)
    assert "3 assets" in window.title.text()
    assert window.types.item(type_row(window, "textures")).text() == "Textures  (2)"
    assert window.types.item(type_row(window, "models")).text() == "Models"
    window.types.setCurrentRow(type_row(window, "textures"))
    folders = [window.tree.topLevelItem(i).text(0) for i in range(window.tree.topLevelItemCount())]
    assert folders == ["Bark", "Metal"]
    zinc = window.tree.topLevelItem(1).child(0)
    assert zinc.text(0) == "Zinc" and zinc.text(1) == "CC BY 4.0"
    assert not zinc.icon(0).isNull()  # (its picture)
    window.select("textures/metal/zinc")
    assert "Credit: \"Zinc\" by Ana Ruiz" in window.info.text()
    window.types.setCurrentRow(type_row(window, "music"))
    assert window.tree.topLevelItem(0).text(0) == "Lights"


def test_importing_through_the_dialog(project, qtbot, tmp_path, monkeypatch):
    said = []
    monkeypatch.setattr(QMessageBox, "warning", lambda *a: said.append(a[2]))
    picture = a_picture(tmp_path, "rusty_plates.png")
    dialog = ImportDialog(project, "textures")
    qtbot.addWidget(dialog)
    dialog.add_sources([picture])
    assert dialog.details.name.text() == "Rusty plates"  # (from the file, to begin with)
    dialog.details.category.setCurrentIndex(dialog.details.category.findData("metal"))
    dialog.details.licence.setCurrentIndex(dialog.details.licence.findData("CC-BY-4.0"))
    assert not dialog.do_import()  # CC BY without an author: refused, said
    assert said and "its author is needed" in said[0]
    dialog.details.author.setText("Ana Ruiz")
    dialog.details.source.setText("https://example.org/rusty")
    dialog.details.description.setPlainText("Old steel.")
    assert dialog.do_import()
    asset = dialog.asset
    assert asset.id == "textures/metal/rusty_plates" and asset.description == "Old steel."
    # Sounds take an album (typed, or none: miscellaneous).
    (tmp_path / "chime.ogg").write_bytes(OGG)
    sound = ImportDialog(project, "effects")
    qtbot.addWidget(sound)
    assert sound.details.category_label.text() == "Album"
    sound.add_sources([tmp_path / "chime.ogg"])
    assert sound.do_import()
    assert sound.asset.id == "effects/miscellaneous/chime"


def test_details_change_and_removing(project, qtbot, tmp_path, monkeypatch):
    asset = lib.import_asset(project, [a_picture(tmp_path, "moss.png")], "images", "Moss")
    details = DetailsDialog(asset)
    qtbot.addWidget(details)
    assert not details.details.type.isEnabled()  # (it stays what it is)
    details.details.name.setText("Green Moss")
    details.details.licence.setCurrentIndex(details.details.licence.findData("CC-BY-3.0"))
    monkeypatch.setattr(QMessageBox, "warning", lambda *a: None)
    assert not details.save()  # (CC BY needs its credit)
    details.details.author.setText("Kim")
    details.details.title.setText("Moss")
    assert details.save()
    again = lib.read(asset.folder)
    assert (again.name, again.licence, again.author) == ("Green Moss", "CC-BY-3.0", "Kim")
    assert "Green Moss" in (asset.folder / "LICENSE.md").read_text("utf-8")
    window = AssetWindow()
    qtbot.addWidget(window)
    window.set_project(project)
    window.select(again.id)
    assert window.remove_selected(confirm=False)
    assert not asset.folder.exists()
    assert "0 assets" in window.title.text()


def test_the_assets_menu_opens_the_library(qtbot, settings, project):
    from midgard_editor.main_window import MainWindow
    window = MainWindow(settings)
    qtbot.addWidget(window)
    menus = [a.text() for a in window.menuBar().actions()]
    assert "&Assets" in menus
    assert not window.library_action.isEnabled()  # (no project)
    window.set_project(project)
    assert window.library_action.isEnabled()
    window.show_asset_library()
    assert window.asset_window.isVisible()
    assert window.asset_window.project is project
    window.set_project(None)
    assert window.asset_window.project is None
    window.asset_window.close()
