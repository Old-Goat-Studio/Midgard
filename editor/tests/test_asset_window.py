# SPDX-License-Identifier: Apache-2.0
"""The Asset Library window (milestone 5.13a step 3): Assets ▸ Asset Library opens it on the
open project; the types with their counts; assets in their folders (kinds, albums) with their
licences and credits; importing with a description and licence (and refusals said); details
changed; an asset removed."""
from pathlib import Path

import pytest
from PySide6.QtWidgets import QMessageBox

from midgard_editor import asset_library as lib
from midgard_editor import engine
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


# ---- the player (step 4) -----------------------------------------------------------------------
def test_the_player_shows_pictures_sounds_and_fonts(project, qtbot, tmp_path):
    from pathlib import Path

    import pytest

    from midgard_editor.docks.asset_window import AssetPlayer
    player = AssetPlayer()
    qtbot.addWidget(player)
    picture = lib.import_asset(project, [a_picture(tmp_path, "slate.png")], "textures", "Slate",
                               category="stone")
    player.show_asset(picture)
    assert player.kind == "picture" and not player.picture.pixmap().isNull()
    assert not player.play_button.isVisible()
    (tmp_path / "chime.ogg").write_bytes(OGG)
    sound = lib.import_asset(project, [tmp_path / "chime.ogg"], "effects", "Chime")
    player.show()
    player.show_asset(sound)
    try:  # (Qt Multimedia needs the system's audio libraries: not on every machine)
        from PySide6.QtMultimedia import QMediaPlayer  # noqa: F401
        playable = True
    except ImportError:
        playable = False
    if playable:
        assert player.kind == "sound" and player.play_button.isVisible()
        assert player._audio[0].source().toLocalFile().endswith("chime.ogg")
    else:  # said, not a crash
        assert player.kind == "" and "can't be played here" in player.picture.text()
    fonts = [Path(p) for p in ("C:/Windows/Fonts/arial.ttf",
                               "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")
             if Path(p).is_file()]
    if not fonts:
        pytest.skip("no font file on this system to try")
    font = lib.import_asset(project, [fonts[0]], "fonts", "Sans")
    player.show_asset(font)
    assert player.kind == "font" and "quick brown fox" in player.picture.text()


def test_the_player_turns_a_model_and_plays_its_clips(project, qtbot):
    import pytest
    from test_characters import SAMPLES

    from midgard_editor import engine
    from midgard_editor.docks.asset_window import AssetPlayer
    if engine.module() is None:
        pytest.skip("engine module not built")
    mannequin = lib.import_asset(project, [SAMPLES / "animation_library.glb"], "models",
                                 "Mannequin")
    player = AssetPlayer()
    qtbot.addWidget(player)
    player.show()
    player.show_asset(mannequin)
    if player._view is None:
        pytest.skip(f"no Vulkan here: {player.picture.text()}")
    assert player.kind == "model"
    assert player.clip.isVisible() and player.clip.findData("Walk_Loop") >= 0
    first = player.picture.pixmap().toImage()
    player.clip.setCurrentIndex(player.clip.findData("Walk_Loop"))
    player.play_button.setChecked(True)
    assert player.timer.isActive()
    for _ in range(8):
        player.tick(0.1)
    assert player.picture.pixmap().toImage() != first  # (turned, and walking)
    player.drop()
    assert player._view is None and not player.timer.isActive()


# ---- using library assets (step 5) ---------------------------------------------------------------
def library_hero(project):
    from test_characters import SAMPLES
    hero = lib.import_asset(project, [SAMPLES / "superhero_male.glb"], "models", "Superhero",
                            author="Quaternius")
    moves = lib.import_asset(project, [SAMPLES / "animation_library.glb"], "models", "Moves",
                             author="Quaternius")
    return hero, moves


def test_a_character_uses_library_models_and_the_cook_packs_them(project, qtbot, tmp_path):
    import pytest

    from midgard_editor import engine
    from midgard_editor.cook import cook
    from midgard_editor.docks.entity_form import EntityForm
    from midgard_editor.scene_model import Entity
    if engine.module() is None:
        pytest.skip("engine module not built")
    hero, moves = library_hero(project)
    hero_id, moves_id = ("lib:models/superhero/superhero_male.glb",
                         "lib:models/moves/animation_library.glb")
    assert project.character_assets()[hero_id] == "Library: Superhero (superhero_male.glb)"
    entity = Entity(project.next_entity_id(), "Hero", None, {"transform": {}})
    project.put_entity(entity)
    form = EntityForm(project, entity.id)
    qtbot.addWidget(form)
    form.add("character")
    form = EntityForm(project, entity.id)
    qtbot.addWidget(form)
    assert form.character_model.findData(hero_id) >= 0  # (in its list)
    picker = form.character_picker("model")
    qtbot.addWidget(picker)
    assert [p.name for _a, p in picker.choices] == ["animation_library.glb",
                                                    "superhero_male.glb"]
    assert form.use_library_model("model", hero.folder / "superhero_male.glb") == hero_id
    form.use_library_model("clips", moves.folder / "animation_library.glb")
    character = project.entity(entity.id).components["character"]
    assert (character["model"], character["clips"]) == (hero_id, moves_id)
    assert character["clip"] == "A_TPose"
    assert not (project.objects_dir / "characters").exists()  # (used where it is: no copy)
    result = cook(project, tmp_path / "pkg")
    content = {c["id"]: c["path"] for c in result.manifest["content"]}
    assert content[hero_id] == "meshes/library/models/superhero/superhero_male.glb"
    assert moves_id in content
    assert not [w for w in result.warnings if "Hero" in w], result.warnings


def test_the_library_picker_and_a_library_texture(project, qtbot, tmp_path):
    from midgard_editor.docks.asset_window import LibraryPicker
    from midgard_editor.docks.material_editor import EntityMaterial, MaterialEditor
    from midgard_editor.scene_model import Entity
    texture = lib.import_asset(project, [a_picture(tmp_path, "slate.png")], "textures", "Slate",
                               category="stone")
    picker = LibraryPicker(project, "Pick", ("textures",), (".png",))
    qtbot.addWidget(picker)
    assert not picker.use_button.isEnabled()
    picker.pick(0)
    assert picker.use_button.isEnabled() and picker.player.kind == "picture"
    picker.accept_current()
    assert picker.chosen == texture.folder / "slate.png"
    entity = Entity(project.next_entity_id(), "Floor", None, {"transform": {},
                                                             "mesh": {"asset": "prim_cube"}})
    project.put_entity(entity)
    editor = MaterialEditor(project, EntityMaterial(project, entity.id))
    qtbot.addWidget(editor)
    editor.use_image("base_color", picker.chosen)
    material = project.entity(entity.id).components["material"]
    assert material["textures"]["base_color"] == "tex:library/textures/stone/slate/slate.png"
    assert not (project.textures_dir / "slate.png").exists()  # (used where it is)
    empty = LibraryPicker(project, "Pick", ("music",), (".ogg",))
    qtbot.addWidget(empty)
    assert empty.list.count() == 0


def test_an_entity_uses_a_library_model_as_its_mesh(project, tmp_path):
    from test_characters import SAMPLES

    from midgard_editor.cook import cook, mesh_asset_data
    from midgard_editor.scene_model import Entity
    lib.import_asset(project, [SAMPLES / "superhero_male.glb"], "models", "Statue")
    asset = "lib:models/statue/superhero_male.glb"
    assert project.mesh_assets()[asset] == "Library: Statue (superhero_male.glb)"
    assert mesh_asset_data(project, asset)[:4] == b"glTF"
    assert mesh_asset_data(project, "lib:../Lib.db") is None  # (never outside the library)
    project.put_entity(Entity(project.next_entity_id(), "Statue", None,
                              {"transform": {}, "mesh": {"asset": asset}}))
    result = cook(project, tmp_path / "pkg")
    assert asset in {c["id"] for c in result.manifest["content"]}


# ---- credits (step 6) ---------------------------------------------------------------------------
@pytest.mark.skipif(engine.module() is None,  # (the cook encodes the texture with it)
                    reason="engine module not built")
def test_the_game_credits_the_cc_by_assets_it_uses(project, qtbot, tmp_path):
    from test_characters import SAMPLES

    from midgard_editor.cook import cook
    from midgard_editor.docks.project_page import ProjectPage
    from midgard_editor.scene_model import Entity
    lib.import_asset(project, [SAMPLES / "superhero_male.glb"], "models", "Superhero",
                     author="Quaternius")  # CC0
    plates = lib.import_asset(project, [a_picture(tmp_path, "plates.png")], "textures",
                              "Rusty Plates", category="metal", licence="CC-BY-4.0",
                              author="Ana Ruiz", source="https://example.org/rusty",
                              title="Rusty Plates")
    lib.import_asset(project, [a_picture(tmp_path, "unused.png")], "textures", "Unused",
                     category="wood", licence="CC-BY-4.0", author="Bo", title="Unused")
    page = ProjectPage()
    qtbot.addWidget(page)
    page.set_project(project)
    assert page.credits.text() == "None: the game uses no CC BY assets."
    project.put_entity(Entity(project.next_entity_id(), "Statue", None, {
        "transform": {}, "mesh": {"asset": "lib:models/superhero/superhero_male.glb"},
        "material": {"textures": {"base_color": "tex:library/textures/metal/rusty_plates/"
                                                "plates.png"}}}))
    page.show_credits()
    assert page.credits.text() == lib.credit(plates)  # (only what the game uses)
    result = cook(project, tmp_path / "pkg")
    assert result.credits == [lib.credit(plates)]
    text = (result.out_dir / "CREDITS.txt").read_text("utf-8")
    assert lib.credit(plates) in text
    assert "Superhero by Quaternius" in text  # (CC0: thanked)
    assert "Unused" not in text
    # A used asset whose file changed since it was imported: the cook says so.
    (plates.folder / "plates.png").write_bytes((plates.folder / "plates.png").read_bytes() + b"x")
    warnings = cook(project, tmp_path / "pkg2").warnings
    assert any("Rusty Plates" in w and "has changed since it was imported" in w for w in warnings)


def test_no_library_assets_no_credits_file(project, tmp_path):
    from midgard_editor.cook import cook
    result = cook(project, tmp_path / "pkg")
    assert result.credits == [] and not (result.out_dir / "CREDITS.txt").exists()
