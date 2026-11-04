# SPDX-License-Identifier: Apache-2.0
"""The texture generator (milestone 5.17 step 6): presets, saving with the recipe, the window,
and the Asset Library and Master."""
import json

import pytest
from PySide6.QtGui import QImage

from midgard_editor import asset_library as lib
from midgard_editor import engine, textures
from midgard_editor import texture_gen as tg

needs_generator = pytest.mark.skipif(
    engine.module() is None or not hasattr(engine.module(), "generate_texture"),
    reason="needs the engine module with the texture generator")


def test_without_the_engine_the_generator_says_so(monkeypatch):
    monkeypatch.setattr(engine, "module", lambda: None)
    with pytest.raises(tg.GeneratorError, match="engine"):
        tg.generate(tg.DEFAULT)


@needs_generator
@pytest.mark.parametrize("name", list(tg.PRESETS))
def test_every_preset_makes_a_picture(name):
    size, pixels, problems = tg.generate({**tg.PRESETS[name], "size": 64})
    assert problems == []
    assert size == 64 and len(pixels) == 64 * 64 * 4


@needs_generator
def test_every_pattern_has_a_label_and_its_settings():
    patterns = engine.module().texture_patterns()
    assert list(tg.PATTERN_LABELS) == patterns
    assert set(tg.PATTERN_SETTINGS) == set(patterns)


@needs_generator
def test_a_texture_is_saved_with_its_recipe_and_opens_again(project):
    made = tg.save(project.root, "Old Bricks!", tg.PRESETS["Bricks"], bumps_too=True)
    folder = project.root / "textures" / "generated"
    assert made == [folder / "Old Bricks_.png", folder / "Old Bricks__normal.png"]
    colour, bumps = QImage(str(made[0])), QImage(str(made[1]))
    assert colour.width() == 512 and bumps.width() == 512
    # The bump map is mostly blue-purple (normals facing out).
    c = bumps.pixelColor(10, 10)
    assert c.blue() > 180
    assert tg.saved(project.root) == ["Old Bricks_"]
    recipe, has_bumps = tg.load(project.root, "Old Bricks_")
    assert has_bumps and recipe["pattern"] == "bricks"
    assert recipe["colors"] == tg.PRESETS["Bricks"]["colors"]
    # A material can use it as any other picture.
    ref = textures.ref(made[0].relative_to(project.root).as_posix(), "base_color")
    assert textures.texture_data(project.root, ref)[:4] == b"\xabKTX"


@needs_generator
def test_a_bad_recipe_is_refused_with_its_problems(project):
    with pytest.raises(tg.GeneratorError, match="isn't a pattern"):
        tg.save(project.root, "odd", {**tg.DEFAULT, "pattern": "plaid"})


@needs_generator
def test_a_generated_texture_goes_into_the_asset_library(project):
    made = tg.save(project.root, "flagstones", tg.PRESETS["Flagstones"], bumps_too=True)
    asset = tg.add_to_library(project, made, "Flagstones", licence=lib.OWN_WORK,
                              author="Richard")
    assert asset.author == "Richard"
    assert asset.type == "textures" and asset.licence == lib.OWN_WORK
    names = sorted(p.name for p in asset.folder.iterdir() if p.suffix == ".png")
    assert names == ["flagstones_color.png", "flagstones_normal.png"]


@needs_generator
def test_the_window_shows_the_recipe_and_saves_it(qtbot, project, settings):
    from midgard_editor.docks.texture_generator import TextureGenerator
    window = TextureGenerator(project, settings)
    qtbot.addWidget(window)
    window.use_preset("Warning stripes")
    assert window.pattern.currentData() == "stripes"
    assert window.name.text() == "warning_stripes"
    assert window.rows["width"][1].isVisibleTo(window)  # (stripes have a width)
    assert not window.rows["octaves"][1].isVisibleTo(window)
    assert not window.preview.pixmap().isNull()
    window.numbers["scale"].setValue(9)
    assert window.recipe["scale"] == 9
    window.output.setCurrentIndex(window.output.findData("normal"))
    assert window.rows["strength"][1].isVisibleTo(window)
    assert not window.stops_box.isVisibleTo(window)  # (bumps have no colours)
    window.output.setCurrentIndex(window.output.findData("color"))
    window._add_stop()
    assert len(window.recipe["colors"]) == 3
    window.tiled.setChecked(True)
    changed = []
    project.files_changed.connect(changed.append)
    made = window.save_to_project()
    assert [p.name for p in made] == ["warning_stripes.png", "warning_stripes_normal.png"]
    assert changed == [["textures/generated/warning_stripes.png",
                        "textures/generated/warning_stripes_normal.png"]]
    saved = json.loads((made[0].parent / "warning_stripes.texgen.json").read_text("utf-8"))
    assert saved["scale"] == 9 and len(saved["colors"]) == 3

    again = TextureGenerator(project, settings)
    qtbot.addWidget(again)
    again.open_saved("warning_stripes")
    assert again.numbers["scale"].value() == 9 and again.recipe["pattern"] == "stripes"


@needs_generator
def test_send_to_master_puts_it_in_both_libraries(qtbot, project, settings, tmp_path,
                                                  monkeypatch):
    from PySide6.QtWidgets import QMessageBox
    from test_master import make_master

    from midgard_editor import master as mst
    from midgard_editor.docks.texture_generator import TextureGenerator
    found = make_master(tmp_path / "Master")
    settings.set("master_library_dir", str(found.root))
    told = []
    monkeypatch.setattr(QMessageBox, "information", lambda *a: told.append(a[2]))
    monkeypatch.setattr(QMessageBox, "warning", lambda *a: told.append(a[2]))
    window = TextureGenerator(project, settings)
    qtbot.addWidget(window)
    window.use_preset("Rock")
    window.author.setText("Richard")
    given = window.send_to_master()
    assert given is not None, told
    assert told == ["'rock' is in the project's Asset Library and the Master Asset Library."]
    assert any(a.name == "rock" for a in mst.open_master(found.root).scan()[0])
    assert settings.get("texture_author") == "Richard"  # (remembered for next time)
