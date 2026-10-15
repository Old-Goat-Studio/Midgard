# SPDX-License-Identifier: Apache-2.0
import struct

import pytest
from PySide6.QtGui import QColor, QImage

from midgard_editor import branding
from midgard_editor.docks.project_page import ProjectPage
from midgard_editor.undo import SetConfigCommand


def make_image(path, width, height, fmt):
    image = QImage(width, height, QImage.Format.Format_RGB32)
    image.fill(QColor("#c64600"))
    assert image.save(str(path), fmt)
    return path


def ico_sizes(data: bytes) -> list[int]:
    reserved, kind, count = struct.unpack_from("<HHH", data)
    assert (reserved, kind) == (0, 1)
    sizes = []
    for i in range(count):
        width, _h, _c, _r, _planes, bits, length, offset = struct.unpack_from(
            "<BBBBHHII", data, 6 + 16 * i)
        assert bits == 32 and data[offset:offset + 8] == b"\x89PNG\r\n\x1a\n"
        sizes.append(width or 256)
    return sizes


# ---- images (no Qt widgets) -------------------------------------------------------------------
def test_an_icon_becomes_a_windows_icon(project, tmp_path, qapp):
    source = make_image(tmp_path / "icon.jpg", 300, 200, "JPEG")
    value, warnings = branding.import_image(project, "icon", source)
    assert any("square" in w for w in warnings)
    png = project.root / value["icon"]
    ico = project.root / value["icon_ico"]
    assert png.parent == project.root / "branding" and png.suffix == ".png"
    assert QImage(str(png)).size().width() == QImage(str(png)).size().height() == 300
    assert ico_sizes(ico.read_bytes()) == [16, 24, 32, 48, 64, 128, 256]
    assert not QImage(str(ico)).isNull()  # Qt (like Windows) reads it


def test_a_logo_from_a_bmp_and_small_icon_warnings(project, tmp_path, qapp):
    value, warnings = branding.import_image(project, "logo",
                                            make_image(tmp_path / "logo.bmp", 640, 200, "BMP"))
    assert warnings == [] and value["logo"].endswith(".png")
    assert QImage(str(project.root / value["logo"])).width() == 640
    _value, warnings = branding.import_image(project, "icon",
                                             make_image(tmp_path / "tiny.png", 32, 32, "PNG"))
    assert any("only 32 pixels" in w for w in warnings)
    with pytest.raises(branding.BrandingError):
        bad = tmp_path / "notes.png"
        bad.write_text("not an image", "utf-8")
        branding.import_image(project, "logo", bad)


def test_undo_finds_the_old_image(project, tmp_path, qapp):
    first, _ = branding.import_image(project, "logo", make_image(tmp_path / "a.png", 64, 64,
                                                                 "PNG"))
    project.undo_stack.push(SetConfigCommand(project, "branding", first, "logo"))
    image = QImage(64, 64, QImage.Format.Format_RGB32)
    image.fill(QColor("#2e8b57"))
    image.save(str(tmp_path / "b.png"))
    second, _ = branding.import_image(project, "logo", tmp_path / "b.png")
    project.undo_stack.push(SetConfigCommand(project, "branding", second, "logo"))
    assert second["logo"] != first["logo"]
    project.undo_stack.undo()
    assert branding.path(project, "logo") == project.root / first["logo"]  # still on disk
    removed = branding.removed(project, "logo")
    assert "logo" not in removed


# ---- the page ---------------------------------------------------------------------------------
@pytest.fixture
def page(qtbot, project):
    widget = ProjectPage()
    qtbot.addWidget(widget)
    widget.set_project(project)
    return widget


def test_the_page_edits_the_project(page, project, qtbot):
    assert page.title.text() == "Hello Cube"
    page.tagline.setText("A cube with places to be")
    page.tagline.editingFinished.emit()
    assert project.game_tagline == "A cube with places to be"
    assert "places to be" in page.heading.text()

    page.description.setPlainText("Roll across the moss.\n\nFind the gate.")
    qtbot.waitUntil(lambda: project.game_description.startswith("Roll"), timeout=3000)

    page.title.setText("")
    page.title.editingFinished.emit()
    assert project.game_title == "Hello Cube"  # a game keeps a title

    page.game_version.setText("1.2.0")
    page.game_version.editingFinished.emit()
    assert project.config["game_version"] == "1.2.0"

    project.undo_stack.undo()  # the version
    project.undo_stack.undo()  # the description
    assert project.game_description == ""
    assert page.description.toPlainText() == ""


def test_choosing_and_removing_the_icon(page, project, tmp_path):
    page.import_image("icon", make_image(tmp_path / "icon.png", 256, 256, "PNG"))
    assert branding.path(project, "icon_ico") is not None
    assert "Windows icon" in page.icon.note.text()
    assert page.icon.remove.isEnabled()
    page.icon.remove.click()
    assert branding.path(project, "icon") is None and branding.path(project, "icon_ico") is None
    project.undo_stack.undo()
    assert branding.path(project, "icon") is not None
