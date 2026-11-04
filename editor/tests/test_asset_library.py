# SPDX-License-Identifier: Apache-2.0
"""The Asset Library (milestone 5.13a): assets in the project's library/ folder, each with its
asset.json and LICENSE.md; only CC0 and CC BY (credited) allowed; files checked against what was
imported; credits; folders by type, kind and album."""
import json

import pytest

from midgard_editor import asset_library as lib


def make(project, name="Rusty Plates", type_="textures", category="metal", **meta):
    folder = lib.type_folder(project, type_, category) / lib.slug(name)
    folder.mkdir(parents=True)
    (folder / "rusty_basecolor.png").write_bytes(b"\x89PNG not really")
    asset = lib.Asset(folder=folder, name=name, type=type_, category=category, **meta)
    lib.record_files(asset)
    lib.write(asset)
    return asset


def test_a_new_project_has_a_library(project):
    assert lib.library_dir(project).is_dir()


def test_assets_are_written_and_read_back(project):
    asset = make(project, description="Old steel plates.", author="Ana Ruiz",
                 source="https://example.org/rusty", licence="CC-BY-4.0", title="Rusty Plates",
                 imported="2026-11-02")
    assert asset.id == "textures/metal/rusty_plates"
    doc = json.loads((asset.folder / "asset.json").read_text("utf-8"))
    assert doc["format"] == 2 and doc["files"][0]["path"] == "rusty_basecolor.png"
    assert len(doc["id"]) == 36 and doc["version"] == 1 and len(doc["fingerprint"]) == 64
    assert len(doc["files"][0]["sha256"]) == 64
    again = lib.read(asset.folder)
    assert again == asset
    text = (asset.folder / "LICENSE.md").read_text("utf-8")
    assert "# Rusty Plates" in text and "CC BY 4.0" in text and "Ana Ruiz" in text
    assert "rusty_basecolor.png" in text
    assert lib.problems(again) == []


def test_only_known_licences_and_credited_ones_are_allowed(project):
    assert lib.problems(make(project, "Free", licence="CC0-1.0")) == []
    bare = make(project, "Bare", licence="CC-BY-4.0")
    said = lib.problems(bare)
    assert any("its author is needed" in p for p in said)
    assert any("its title or source is needed" in p for p in said)
    other = make(project, "Other", licence="CC-BY-NC-4.0", author="Someone")
    assert any("isn't allowed (only CC0-1.0, CC-BY-4.0, CC-BY-3.0, CC-BY-SA-4.0, "
               "CC-BY-SA-3.0, Purchased)" in p for p in lib.problems(other))


def test_changed_and_missing_files_are_noticed(project):
    asset = make(project)
    (asset.folder / "rusty_basecolor.png").write_bytes(b"something else")
    assert lib.problems(asset) == ["rusty_basecolor.png has changed since it was imported"]
    (asset.folder / "rusty_basecolor.png").unlink()
    assert lib.problems(asset) == ["rusty_basecolor.png is missing"]


def test_credits_are_made_for_cc_by_only(project):
    by = make(project, "Rusty Plates", licence="CC-BY-4.0", author="Ana Ruiz",
              source="https://example.org/rusty", title="Rusty Plates")
    free = make(project, "Moss", licence="CC0-1.0")
    assert lib.credit(by) == ('"Rusty Plates" by Ana Ruiz (https://example.org/rusty), CC BY 4.0 '
                              "(https://creativecommons.org/licenses/by/4.0/)")
    assert lib.credits([by, free, by]) == [lib.credit(by)]  # (each once; CC0 needs none)


def test_the_library_is_scanned_by_type_and_name(project):
    make(project, "Zinc", category="metal")
    make(project, "Oak", category="wood")
    make(project, "Hero", type_="models", category="")
    song = make(project, "Dawn", type_="music", category="Northern Lights")
    assert song.id == "music/northern_lights/dawn"
    broken = lib.library_dir(project) / "images" / "broken"
    broken.mkdir(parents=True)
    (broken / "asset.json").write_text("{not json", "utf-8")
    assets, problems = lib.scan(project)
    assert [a.name for a in assets] == ["Hero", "Dawn", "Zinc", "Oak"]
    assert len(problems) == 1 and "broken" in problems[0]


def test_folders_by_type_kind_and_album(project):
    root = lib.library_dir(project)
    assert lib.type_folder(project, "textures", "wood") == root / "textures" / "wood"
    assert lib.type_folder(project, "effects") == root / "effects" / "miscellaneous"
    assert lib.type_folder(project, "models") == root / "models"
    with pytest.raises(lib.LibraryError):
        lib.type_folder(project, "textures", "cheese")
    with pytest.raises(lib.LibraryError):
        lib.type_folder(project, "sounds")


def test_removing_an_asset_tidies_its_folders(project):
    asset = make(project, "Dawn", type_="music", category="Northern Lights")
    lib.remove(asset)
    assert not (lib.library_dir(project) / "music").exists()
    assert lib.library_dir(project).is_dir()  # (the library itself stays)


# ---- importing (step 2) --------------------------------------------------------------------------
PNG = b"\x89PNG\r\n\x1a\n" + b"\x00" * 32
OGG = b"OggS" + b"\x00" * 32


def test_files_a_folder_and_a_zip_are_copied_in(project, tmp_path):
    import zipfile

    from test_characters import SAMPLES
    # Loose files: a model and its licence.
    (tmp_path / "LICENSE.txt").write_text("CC0", "utf-8")
    hero = lib.import_asset(project, [SAMPLES / "superhero_male.glb", tmp_path / "LICENSE.txt"],
                            "models", "Superhero", author="Quaternius",
                            source="https://quaternius.com", today="2026-11-02")
    assert hero.id == "models/superhero"
    assert sorted(f["path"] for f in hero.files) == ["LICENSE.txt", "superhero_male.glb"]
    assert (hero.folder / "superhero_male.glb").read_bytes() == \
        (SAMPLES / "superhero_male.glb").read_bytes()  # (a copy)
    assert lib.problems(hero) == []
    # A folder: its layout kept.
    textures = tmp_path / "rusty"
    (textures / "maps").mkdir(parents=True)
    (textures / "maps" / "base.png").write_bytes(PNG)
    (textures / "maps" / "normal.png").write_bytes(PNG + b"n")
    rusty = lib.import_asset(project, [textures], "textures", "Rusty Plates", category="metal",
                             licence="CC-BY-4.0", author="Ana Ruiz", title="Rusty Plates")
    assert sorted(f["path"] for f in rusty.files) == ["maps/base.png", "maps/normal.png"]
    # A zip: its single top folder left out, a Mac's leftovers too.
    archive = tmp_path / "songs.zip"
    with zipfile.ZipFile(archive, "w") as z:
        z.writestr("Dawn Album/dawn.ogg", OGG)
        z.writestr("Dawn Album/readme.md", "A song.")
        z.writestr("__MACOSX/Dawn Album/._dawn.ogg", "junk")
    song = lib.import_asset(project, [archive], "music", "Dawn", category="Northern Lights")
    assert song.id == "music/northern_lights/dawn"
    assert sorted(f["path"] for f in song.files) == ["dawn.ogg", "readme.md"]
    assert not list((project.root / ".midgard").glob("import_*"))  # (no scratch left)
    assets, _ = lib.scan(project)
    assert {a.name for a in assets} == {"Superhero", "Rusty Plates", "Dawn"}


def test_files_it_cant_use_are_skipped_not_fatal(project, tmp_path):
    """A pack of many kinds of file: what the type can use comes in; the rest is skipped and
    said (by ending), text about it kept."""
    pack = tmp_path / "pack"
    pack.mkdir()
    (pack / "song.ogg").write_bytes(OGG)
    (pack / "tool.exe").write_bytes(b"MZ")
    (pack / "source.blend").write_bytes(b"BLENDER")
    (pack / "fake.wav").write_bytes(b"not a sound")
    (pack / "LICENSE.txt").write_text("CC0", "utf-8")
    (pack / "manual.pdf").write_bytes(b"%PDF")
    asset = lib.import_asset(project, [pack], "music", "Song")
    assert sorted(f["path"] for f in asset.files) == ["LICENSE.txt", "manual.pdf", "song.ogg"]
    assert sorted(rel for rel, _why in asset.skipped) == ["fake.wav", "source.blend",
                                                          "tool.exe"]
    assert dict(asset.skipped)["fake.wav"] == "isn't a .wav file inside"
    assert lib.skipped_summary(asset.skipped) == "skipped 3 files: 1 .blend, 1 .exe, 1 .wav"


def test_imports_are_refused_with_a_reason_and_leave_nothing(project, tmp_path):
    import zipfile

    def refused(sources, type_, says, **meta):
        with pytest.raises(lib.LibraryError) as e:
            lib.import_asset(project, sources, type_, "Thing", **meta)
        assert says in str(e.value), str(e.value)

    (tmp_path / "song.ogg").write_bytes(OGG)
    (tmp_path / "fake.png").write_bytes(b"not a picture")
    (tmp_path / "notes.txt").write_text("hi", "utf-8")
    # Nothing usable: refused, saying what was skipped.
    refused([tmp_path / "song.ogg"], "textures", "nothing usable (only text, or files it can't "
            "use) (skipped 1 file: 1 .ogg)", category="wood")
    refused([tmp_path / "fake.png"], "images", "skipped 1 file: 1 .png")
    refused([tmp_path / "notes.txt"], "images", "nothing usable")
    refused([tmp_path / "song.ogg"], "music", "its author is needed", licence="CC-BY-4.0")
    refused([tmp_path / "song.ogg"], "music", "isn't allowed", licence="GPL-3.0")
    escape = tmp_path / "escape.zip"  # (a zip writing outside: the whole zip refused)
    with zipfile.ZipFile(escape, "w") as z:
        z.writestr("song.ogg", OGG)
        z.writestr("../../evil.ogg", OGG)
    refused([escape], "music", "would land outside the library")
    assert not (tmp_path.parent / "evil.ogg").exists()
    # The same file twice: refused, naming where it is.
    lib.import_asset(project, [tmp_path / "song.ogg"], "effects", "Chime")
    refused([tmp_path / "song.ogg"], "music", "in the library already, as 'Chime'")
    assets, _ = lib.scan(project)
    assert [a.name for a in assets] == ["Chime"]  # (nothing else got in)
    assert not (lib.library_dir(project) / "music").exists()


def test_a_gltf_naming_files_it_hasnt_got_is_warned_about(project, tmp_path):
    from test_characters import chain_gltf
    folder = tmp_path / "model"
    folder.mkdir()
    gltf = chain_gltf(folder)  # (names chain.bin, there, and skin.png, not there)
    asset = lib.import_asset(project, [gltf, folder / "chain.bin"], "models", "Chain")
    assert asset.warnings == ["chain.gltf names skin.png, which isn't with it"]


def test_a_pack_of_models_split_one_asset_each(project, tmp_path):
    import zipfile

    from test_characters import SAMPLES
    pack = tmp_path / "pack.zip"
    with zipfile.ZipFile(pack, "w") as z:
        z.write(SAMPLES / "superhero_male.glb", "Pack/hero/superhero_male.glb")
        z.write(SAMPLES / "superhero_male_basecolor.png", "Pack/hero/superhero_male_basecolor.png")
        z.write(SAMPLES / "animation_library.glb", "Pack/moves/animation_library.glb")
        z.writestr("Pack/LICENSE.txt", "CC0 1.0")
        z.writestr("Pack/hero/hero.fbx", "FBX")
        z.writestr("Pack/stray.png", PNG)
    assert lib.model_count([pack]) == 2
    result = lib.import_assets(project, [pack], "models", "Heroes", author="Quaternius",
                               split=True)
    by_name = {a.name: a for a in result.assets}
    assert set(by_name) == {"Heroes: Superhero male", "Heroes: Animation library"}
    hero = by_name["Heroes: Superhero male"]
    assert sorted(f["path"] for f in hero.files) == [
        "LICENSE.txt", "hero/superhero_male.glb", "hero/superhero_male_basecolor.png"]
    moves = by_name["Heroes: Animation library"]
    assert sorted(f["path"] for f in moves.files) == ["LICENSE.txt",
                                                      "moves/animation_library.glb"]
    assert dict(result.skipped) == {"hero/hero.fbx": "models can't use .fbx",
                                    "stray.png": "with no one model"}
    assert result.summary() == ("Imported 2 assets; skipped 2 files: 1 .fbx, 1 .png.")
    # Not split: one asset of them all.
    lib.remove(hero)
    lib.remove(moves)
    whole = lib.import_assets(project, [pack], "models", "Heroes", split=False)
    assert len(whole.assets) == 1 and len(whole.assets[0].files) == 5


# ---- texture sets: each picture's role (FixMe) -------------------------------------------------
def grey_picture(folder, name: str, value: int, size: int = 4):
    from PySide6.QtGui import QColor, QImage
    image = QImage(size, size, QImage.Format.Format_RGB32)
    image.fill(QColor(value, value, value))
    path = folder / name
    image.save(str(path))
    return path


def test_roles_are_guessed_from_the_common_packs_names():
    guesses = {
        "Rock001_2K_Color.jpg": "color", "rock_diff_2k.png": "color",
        "T_Wall_BaseColor.png": "color", "wood_albedo.png": "color",
        "Rock001_2K_NormalGL.jpg": "normal", "rock_nor_gl_2k.png": "normal",
        "Rock001_2K_Roughness.jpg": "roughness", "rock_rough_2k.png": "roughness",
        "Metal_Metalness.png": "metallic", "Rock001_2K_AmbientOcclusion.jpg": "occlusion",
        "rock_ao_2k.png": "occlusion", "lamp_emissive.png": "emissive",
        "rock_arm_2k.png": "metallic_roughness",
        "Rock001_2K_Displacement.jpg": "", "rock_disp_2k.png": "", "preview.png": "",
    }
    for name, role in guesses.items():
        assert lib.guess_role(name) == role, name


def test_a_texture_set_imported_by_role(project, tmp_path):
    import zipfile

    from PySide6.QtGui import QColor, QImage
    folder = tmp_path / "src"
    folder.mkdir()
    grey_picture(folder, "Rock_Color.png", 128)
    grey_picture(folder, "Rock_NormalGL.png", 200)
    grey_picture(folder, "Rock_Roughness.png", 90)
    grey_picture(folder, "Rock_Metalness.png", 30)
    grey_picture(folder, "Rock_Displacement.png", 60)
    pack = tmp_path / "rock.zip"
    with zipfile.ZipFile(pack, "w") as z:
        for p in folder.iterdir():
            z.write(p, f"Rock/{p.name}")
        z.writestr("Rock/LICENSE.txt", "CC0")
    found = lib.images_in([pack])
    assert sorted(found) == ["Rock_Color.png", "Rock_Displacement.png", "Rock_Metalness.png",
                             "Rock_NormalGL.png", "Rock_Roughness.png"]  # (top folder off)
    roles = {rel: lib.guess_role(rel) for rel in found}
    result = lib.import_assets(project, [pack], "textures", "Mossy Rock", category="stone",
                               roles=roles)
    asset = result.assets[0]
    assert sorted(f["path"] for f in asset.files) == [
        "LICENSE.txt", "mossy_rock_color.png", "mossy_rock_metallic.png",
        "mossy_rock_metallic_roughness.png", "mossy_rock_normal.png", "mossy_rock_roughness.png"]
    assert result.skipped == [("Rock_Displacement.png", "not used")]
    # The combined picture: roughness in green, metallic in blue.
    combined = QImage(str(asset.folder / "mossy_rock_metallic_roughness.png"))
    pixel = QColor(combined.pixel(1, 1))
    assert (pixel.green(), pixel.blue()) == (90, 30)
    assert set(lib.texture_set(asset)) == {"color", "normal", "roughness", "metallic",
                                           "metallic_roughness"}
    slots = lib.material_textures(project, asset)
    assert slots["base_color"] == "tex:library/textures/stone/mossy_rock/mossy_rock_color.png"
    assert set(slots) == {"base_color", "normal", "metallic_roughness"}
    assert slots["normal"].endswith("?linear")  # (not colour)


def test_two_pictures_with_one_role_are_refused(project, tmp_path):
    a, b = grey_picture(tmp_path, "a.png", 10), grey_picture(tmp_path, "b.png", 20)
    with pytest.raises(lib.LibraryError, match="a.png and b.png are both colour"):
        lib.import_asset(project, [a, b], "textures", "Twins", category="wood",
                         roles={"a.png": "color", "b.png": "color"})
    single = lib.import_asset(project, [a], "textures", "Plain", category="wood")
    assert lib.texture_set(single) == {"color": single.folder / "a.png"}  # (a lone one)
