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
    qtbot.waitUntil(lambda: player.kind != "preparing", timeout=30000)
    assert player.kind in ("material", "picture")  # (its material; its picture without the engine)
    assert not player.picture.pixmap().isNull()
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
    qtbot.waitUntil(lambda: picker.player.kind != "preparing", timeout=30000)
    assert picker.use_button.isEnabled() and picker.player.kind in ("material", "picture")
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


# ---- a pack: what it can't use skipped, and split one asset per model (FixMe) --------------------
def test_the_dialog_imports_a_pack_skipping_what_it_cant_use(project, qtbot, tmp_path):
    import shutil

    from test_characters import SAMPLES
    pack = tmp_path / "pack"
    (pack / "hero").mkdir(parents=True)
    (pack / "moves").mkdir()
    shutil.copy(SAMPLES / "superhero_male.glb", pack / "hero")
    shutil.copy(SAMPLES / "animation_library.glb", pack / "moves")
    (pack / "hero" / "hero.fbx").write_bytes(b"FBX")
    (pack / "LICENSE.txt").write_text("CC0", "utf-8")
    asked = []
    dialog = ImportDialog(project, "models")
    qtbot.addWidget(dialog)
    dialog.quiet = True  # (the report, not shown)
    dialog.ask_split = lambda n: asked.append(n) or True
    dialog.add_sources([pack])
    dialog.details.name.setText("Heroes")
    assert dialog.do_import()
    assert asked == [2]  # (it asked: two models)
    assert sorted(a.name for a in dialog.imported.assets) == ["Heroes: Animation library",
                                                              "Heroes: Superhero male"]
    assert "skipped 1 file: 1 .fbx" in dialog.last_report
    assert "hero/hero.fbx: models can't use .fbx" in dialog.last_report


# ---- texture roles and the player's list (FixMe 3-4) -------------------------------------------
def a_rock_pack(tmp_path):
    from test_asset_library import grey_picture
    pack = tmp_path / "rock"
    pack.mkdir()
    for name, value in (("Rock_Color.png", 128), ("Rock_NormalGL.png", 200),
                        ("Rock_Roughness.png", 90), ("Rock_Displacement.png", 60)):
        grey_picture(pack, name, value)
    return pack


def test_the_dialog_asks_each_pictures_role(project, qtbot, tmp_path, monkeypatch):
    said = []
    monkeypatch.setattr(QMessageBox, "warning", lambda *a: said.append(a[2]))
    dialog = ImportDialog(project, "textures")
    qtbot.addWidget(dialog)
    dialog.quiet = True
    dialog.add_sources([a_rock_pack(tmp_path)])
    assert not dialog.pictures.isHidden()
    assert dialog.roles() == {"Rock_Color.png": "color", "Rock_NormalGL.png": "normal",
                              "Rock_Roughness.png": "roughness", "Rock_Displacement.png": ""}
    dialog.set_role("Rock_NormalGL.png", "color")  # two colours: refused, said
    assert not dialog.do_import()
    assert said and "both colour" in said[0]
    dialog.set_role("Rock_NormalGL.png", "normal")
    dialog.details.name.setText("Grey Rock")
    dialog.details.category.setCurrentIndex(dialog.details.category.findData("stone"))
    assert dialog.do_import()
    assert sorted(f["path"] for f in dialog.asset.files) == [
        "grey_rock_color.png", "grey_rock_metallic_roughness.png", "grey_rock_normal.png",
        "grey_rock_roughness.png"]
    assert "Rock_Displacement.png: not used" in dialog.last_report
    # Not textures: no roles asked.
    dialog.details.type.setCurrentIndex(dialog.details.type.findData("images"))
    assert dialog.pictures.isHidden() and dialog.roles() is None


def test_the_player_lists_what_an_asset_holds(project, qtbot, tmp_path):
    from test_asset_library import grey_picture
    from test_characters import SAMPLES

    from midgard_editor.docks.asset_window import AssetPlayer
    pack = a_rock_pack(tmp_path)
    rock = lib.import_asset(project, [pack], "textures", "Rock", category="stone",
                            roles={r: lib.guess_role(r) for r in lib.images_in([pack])})
    player = AssetPlayer()
    qtbot.addWidget(player)
    player.show()
    player.show_asset(rock)
    labels = [player.choose.itemText(i) for i in range(player.choose.count())]
    assert labels[0] == "Material (all maps)"
    assert "Colour: rock_color.png" in labels and "Normal: rock_normal.png" in labels
    qtbot.waitUntil(lambda: player.kind != "preparing", timeout=30000)
    assert player.choose.isVisible() and player.kind in ("material", "picture")
    player.choose.setCurrentIndex(labels.index("Normal: rock_normal.png"))
    assert player.kind == "picture"
    # A model asset of two models: either one.
    two = lib.import_asset(project, [SAMPLES / "superhero_male.glb",
                                     SAMPLES / "animation_library.glb"], "models", "Two")
    player.show_asset(two)
    assert [player.choose.itemText(i) for i in range(player.choose.count())] == [
        "animation_library.glb", "superhero_male.glb"]
    # One thing only: no list.
    lone = lib.import_asset(project, [grey_picture(tmp_path, "lone.png", 5)], "images", "Lone")
    player.show_asset(lone)
    assert not player.choose.isVisible() and player.kind == "picture"
    player.drop()


def test_the_material_editor_takes_a_whole_texture_set(project, qtbot, tmp_path):
    from test_asset_library import grey_picture

    from midgard_editor.docks.material_editor import EntityMaterial, MaterialEditor
    from midgard_editor.scene_model import Entity
    pack = a_rock_pack(tmp_path)
    rock = lib.import_asset(project, [pack], "textures", "Rock", category="stone",
                            roles={r: lib.guess_role(r) for r in lib.images_in([pack])})
    entity = Entity(project.next_entity_id(), "Wall", None, {"transform": {},
                                                            "mesh": {"asset": "prim_cube"}})
    project.put_entity(entity)
    editor = MaterialEditor(project, EntityMaterial(project, entity.id))
    qtbot.addWidget(editor)
    editor.use_library_picture("normal", rock.folder / "rock_normal.png")
    slots = project.entity(entity.id).components["material"]["textures"]
    assert set(slots) == {"base_color", "normal", "metallic_roughness"}
    assert slots["base_color"].endswith("rock_color.png")
    project.undo_stack.undo()  # (one step)
    assert "textures" not in project.entity(entity.id).components.get("material", {})
    lone = lib.import_asset(project, [grey_picture(tmp_path, "plain.png", 7)], "textures",
                            "Plain", category="wood")
    editor.use_library_picture("emissive", lone.folder / "plain.png")
    assert set(project.entity(entity.id).components["material"]["textures"]) == {"emissive"}


def test_a_sound_assets_files_are_listed_with_their_sizes_to_play(project, qtbot, tmp_path):
    from midgard_editor.docks.asset_window import size_text
    album = tmp_path / "album"
    album.mkdir()
    (album / "dawn.ogg").write_bytes(OGG + b"\x00" * 2000)
    (album / "dusk.ogg").write_bytes(OGG + b"\x01" * 50)
    (album / "LICENSE.txt").write_text("CC0", "utf-8")
    lib.import_asset(project, [album], "music", "Day", category="Lights")
    window = AssetWindow()
    qtbot.addWidget(window)
    window.set_project(project)
    window.show()
    window.select("music/lights/day")
    rows = [(window.files_list.topLevelItem(i).text(0), window.files_list.topLevelItem(i).text(1))
            for i in range(window.files_list.topLevelItemCount())]
    assert rows == [("dawn.ogg", "2 KB"), ("dusk.ogg", "86 bytes"), ("LICENSE.txt", "3 bytes")]
    assert window.files_list.isVisible()
    assert "dawn.ogg" not in window.info.text()  # (listed below, not in the details)
    assert not window.player.choose.isVisible()  # (the list chooses)
    licence = window.files_list.topLevelItem(2)
    assert not licence.flags() & licence.flags().ItemIsSelectable  # (not a sound)
    # The first sound is ready; choosing the other is what the player plays.
    assert window.player.choose.currentData()[1].name == "dawn.ogg"
    window.files_list.topLevelItem(0).setSelected(False)
    window.files_list.topLevelItem(1).setSelected(True)
    assert window.player.choose.currentData()[1].name == "dusk.ogg"
    # Not a sound asset: no list.
    lib.import_asset(project, [a_picture(tmp_path, "tile.png")], "images", "Tile")
    window.refresh()
    window.select("images/tile")
    assert not window.files_list.isVisible()
    assert size_text(3_400_000) == "3.4 MB" and size_text(820_000) == "820 KB"


def test_sounds_in_a_packs_folders_are_listed_by_their_own_names(project, qtbot, tmp_path):
    """A pack keeps its folders: the list names each file, not the folder they share."""
    pack = tmp_path / "pick"
    (pack / "Sci-Fi Sound Pack" / "Lasers").mkdir(parents=True)
    (pack / "Sci-Fi Sound Pack" / "Doors").mkdir()
    (pack / "Sci-Fi Sound Pack" / "Lasers" / "laser_01.ogg").write_bytes(OGG + b"\x01" * 10)
    (pack / "Sci-Fi Sound Pack" / "Lasers" / "laser_02.ogg").write_bytes(OGG + b"\x02" * 20)
    (pack / "Sci-Fi Sound Pack" / "Doors" / "door_open.ogg").write_bytes(OGG + b"\x03" * 30)
    (pack / "Sci-Fi Sound Pack" / "readme.txt").write_text("CC0", "utf-8")
    lib.import_asset(project, [pack], "effects", "Sci-Fi")
    window = AssetWindow()
    qtbot.addWidget(window)
    window.set_project(project)
    window.show()
    window.select("effects/miscellaneous/sci_fi")
    rows = [tuple(window.files_list.topLevelItem(i).text(c) for c in range(3))
            for i in range(window.files_list.topLevelItemCount())]
    assert rows == [("door_open.ogg", "66 bytes", "Sci-Fi Sound Pack/Doors"),
                    ("laser_01.ogg", "46 bytes", "Sci-Fi Sound Pack/Lasers"),
                    ("laser_02.ogg", "56 bytes", "Sci-Fi Sound Pack/Lasers"),
                    ("readme.txt", "3 bytes", "Sci-Fi Sound Pack")]
    assert window.files_list.topLevelItem(1).toolTip(0) == \
        "Sci-Fi Sound Pack/Lasers/laser_01.ogg"
    window.files_list.topLevelItem(0).setSelected(False)
    window.files_list.topLevelItem(2).setSelected(True)
    assert window.player.choose.currentData()[1].name == "laser_02.ogg"


def test_a_materials_textures_are_prepared_without_holding_up_the_window(project, qtbot,
                                                                         tmp_path, monkeypatch):
    """Encoding a picture for the engine takes seconds: the player says so and carries on, then
    draws the material; choosing something else meanwhile forgets it."""
    import threading

    from midgard_editor import textures
    from midgard_editor.docks.asset_window import AssetPlayer
    if engine.module() is None:
        pytest.skip("needs the engine module")
    monkeypatch.setattr(textures, "CACHE", textures.TextureCache())
    pack = a_rock_pack(tmp_path)
    rock = lib.import_asset(project, [pack], "textures", "Rock", category="stone",
                            roles={r: lib.guess_role(r) for r in lib.images_in([pack])})
    gate = threading.Event()
    encode = textures.TextureCache.data

    def slow(cache, root, texture):  # (held until the test lets it go)
        gate.wait(10)
        return encode(cache, root, texture)

    monkeypatch.setattr(textures.TextureCache, "data", slow)
    player = AssetPlayer()
    qtbot.addWidget(player)
    player.show_asset(rock)
    assert player.kind == "preparing" and "Preparing the textures" in player.picture.text()
    # Something else chosen meanwhile: the preparation's end draws nothing.
    player.choose.setCurrentIndex(1)
    assert player.kind == "picture"
    gate.set()
    qtbot.wait(300)
    assert player.kind == "picture"
    # Back to the material: ready now (cached), drawn at once.
    player.choose.setCurrentIndex(0)
    qtbot.waitUntil(lambda: player.kind == "material", timeout=30000)
    assert not player.picture.pixmap().isNull()


def test_a_material_is_shown_on_a_cube_sphere_or_plane_tiled_1_1_to_1_4(project, qtbot,
                                                                       tmp_path):
    from midgard_editor.docks.asset_window import TILINGS, AssetPlayer
    if engine.module() is None:
        pytest.skip("needs the engine module")
    from PySide6.QtGui import QColor, QImage
    checks = QImage(64, 64, QImage.Format.Format_RGB32)  # (a pattern: tiling shows)
    for y in range(64):
        for x in range(64):
            checks.setPixelColor(x, y, QColor(230, 230, 230) if (x // 16 + y // 16) % 2
                                 else QColor(30, 60, 160))
    (tmp_path / "checks").mkdir()
    checks.save(str(tmp_path / "checks" / "checks_color.png"))
    checks.save(str(tmp_path / "checks" / "checks_normal.png"))
    tiles = lib.import_asset(project, [tmp_path / "checks"], "textures", "Checks",
                             category="floor", roles={"checks_color.png": "color",
                                                     "checks_normal.png": "normal"})
    player = AssetPlayer()
    qtbot.addWidget(player)
    player.show()
    player.show_asset(tiles)
    qtbot.waitUntil(lambda: player.kind == "material", timeout=30000)
    assert player.shape.isVisible() and player.tiling.isVisible()
    assert [player.shape.itemText(i) for i in range(player.shape.count())] == \
        ["Cube", "Sphere", "Plane"]
    assert [player.tiling.itemText(i) for i in range(player.tiling.count())] == \
        [f"1:{n}" for n in TILINGS] == ["1:1", "1:2", "1:4"]
    pictures = {}
    for shape in range(player.shape.count()):
        player.shape.setCurrentIndex(shape)
        for tiling in range(player.tiling.count()):
            player.tiling.setCurrentIndex(tiling)
            assert player.kind == "material"
            pictures[shape, tiling] = player.picture.pixmap().toImage()
    # Each shape and each tiling draws something of its own.
    assert len({bytes(p.constBits()) for p in pictures.values()}) == len(pictures)
    # A single map shown as a picture: neither choice applies.
    player.choose.setCurrentIndex(1)
    assert not player.shape.isVisible() and not player.tiling.isVisible()
