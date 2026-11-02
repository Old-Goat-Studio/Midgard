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
    assert doc["format"] == 1 and doc["files"][0]["path"] == "rusty_basecolor.png"
    assert len(doc["files"][0]["sha256"]) == 64
    again = lib.read(asset.folder)
    assert again == asset
    text = (asset.folder / "LICENSE.md").read_text("utf-8")
    assert "# Rusty Plates" in text and "CC BY 4.0" in text and "Ana Ruiz" in text
    assert "rusty_basecolor.png" in text
    assert lib.problems(again) == []


def test_only_cc0_and_credited_cc_by_are_allowed(project):
    assert lib.problems(make(project, "Free", licence="CC0-1.0")) == []
    bare = make(project, "Bare", licence="CC-BY-4.0")
    said = lib.problems(bare)
    assert any("its author is needed" in p for p in said)
    assert any("its title or source is needed" in p for p in said)
    other = make(project, "Other", licence="CC-BY-NC-4.0", author="Someone")
    assert any("isn't allowed (only CC0-1.0, CC-BY-4.0, CC-BY-3.0)" in p
               for p in lib.problems(other))


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


def test_imports_are_refused_with_a_reason_and_leave_nothing(project, tmp_path):
    import zipfile

    def refused(sources, type_, says, **meta):
        with pytest.raises(lib.LibraryError) as e:
            lib.import_asset(project, sources, type_, "Thing", **meta)
        assert says in str(e.value), str(e.value)

    (tmp_path / "song.ogg").write_bytes(OGG)
    (tmp_path / "tool.exe").write_bytes(b"MZ")
    (tmp_path / "fake.png").write_bytes(b"not a picture")
    (tmp_path / "notes.txt").write_text("hi", "utf-8")
    refused([tmp_path / "song.ogg", tmp_path / "tool.exe"], "music", "can't hold tool.exe")
    refused([tmp_path / "song.ogg"], "textures", "can't hold song.ogg", category="wood")
    refused([tmp_path / "fake.png"], "images", "fake.png isn't a .png file inside")
    refused([tmp_path / "notes.txt"], "images", "only text files")
    refused([tmp_path / "song.ogg"], "music", "its author is needed", licence="CC-BY-4.0")
    refused([tmp_path / "song.ogg"], "music", "isn't allowed", licence="GPL-3.0")
    escape = tmp_path / "escape.zip"
    with zipfile.ZipFile(escape, "w") as z:
        z.writestr("../../evil.ogg", OGG)
    refused([escape], "music", "would land outside the library")
    assert not (tmp_path.parent / "evil.ogg").exists()
    # The same file twice: refused, naming where it is.
    lib.import_asset(project, [tmp_path / "song.ogg"], "effects", "Chime")
    refused([tmp_path / "song.ogg"], "music", "in the library already, as 'Chime'")
    assets, _ = lib.scan(project)
    assert [a.name for a in assets] == ["Chime"]  # (nothing else got in)
    assert not (lib.library_dir(project) / "music").exists()
