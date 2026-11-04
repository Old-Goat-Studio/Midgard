# SPDX-License-Identifier: Apache-2.0
"""The Master Asset Library from Midgard's side (milestone 5.13b): the shared library format
read and written; Assets ▸ Add from Master…; a copy's newer version; Send to Master; purchased
originals kept out of Git."""
import json
import uuid
from pathlib import Path

import pytest

from midgard_editor import asset_library as lib
from midgard_editor import master as mst
from midgard_editor import vcs

PNG = b"\x89PNG\r\n\x1a\n"


def make_master(folder: Path) -> mst.Master:
    """A Master library, as the Master Asset Library program makes it."""
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "library.json").write_text(json.dumps({
        "format": 2, "kind": "master", "id": str(uuid.uuid4()), "name": "Master Asset Library",
        "created": "2026-11-03"}))
    return mst.open_master(folder)


def master_asset(master: mst.Master, path: str, name: str, data: bytes = PNG + b"plates",
                 **fields) -> lib.Asset:
    """An asset in the Master, as the program writes it (format 2)."""
    folder = master.root / path
    folder.mkdir(parents=True)
    (folder / f"{folder.name}.png").write_bytes(data)
    extra = fields.pop("extra_files", {})
    for rel, content in extra.items():
        (folder / rel).write_bytes(content)
    parts = path.split("/")
    asset = lib.Asset(folder=folder, name=name, type=parts[0],
                      category=parts[1] if len(parts) == 3 else "", imported="2026-11-03",
                      **fields)
    lib.record_files(asset)
    asset.uid = str(uuid.uuid4())
    lib.write(asset)
    return lib.read(folder)


@pytest.fixture
def master(tmp_path, settings):
    m = make_master(tmp_path / "Master Asset Library")
    settings.set("master_library_dir", str(m.root))
    return m


# ---- the shared format -------------------------------------------------------------------------
def test_a_masters_asset_is_read_as_the_program_wrote_it(master):
    a = master_asset(master, "models/village", "Village", tags=["fantasy"],
                     licence=lib.PURCHASED, author="Studio Nine", title="Village",
                     purchase={"seller": "Example Asset Store", "product": "Village Pack",
                               "licence_file": "EULA.txt", "credit": True},
                     extra_files={"EULA.txt": b"terms"})
    again = lib.read(a.folder)
    assert again.uid == a.uid and again.version == 1 and again.tags == ["fantasy"]
    assert again.purchase["seller"] == "Example Asset Store"
    assert lib.problems(again) == []
    assert not lib.may_give_away(again) and lib.needs_credit(again)
    assert lib.credit(again) == '"Village" by Studio Nine, from Example Asset Store'
    assert "may not be given away" in lib.licence_text(again)


def test_a_purchased_asset_needs_its_seller_and_licence_file(master):
    a = master_asset(master, "images/logo", "Logo", licence=lib.PURCHASED, purchase={})
    said = lib.problems(a)
    assert any("who sold it" in p for p in said) and any("licence is needed" in p for p in said)


def test_5_13a_assets_are_still_read_and_keep_one_id(project):
    folder = lib.type_folder(project, "textures", "wood") / "planks"
    folder.mkdir(parents=True)
    (folder / "planks.png").write_bytes(PNG)
    files = [{"path": "planks.png", "sha256": lib.sha256(folder / "planks.png"), "bytes": 8}]
    (folder / "asset.json").write_text(json.dumps({
        "format": 1, "name": "Planks", "type": "textures", "category": "wood",
        "description": "", "licence": "CC0-1.0", "author": "", "source": "", "title": "",
        "imported": "2026-10-30", "files": files}))
    a, b = lib.read(folder), lib.read(folder)
    assert a.uid == b.uid and a.version == 1 and a.fingerprint == lib.fingerprint(files)
    assert lib.problems(a) == []


def test_record_files_counts_versions(project):
    folder = lib.type_folder(project, "images") / "splash"
    folder.mkdir(parents=True)
    (folder / "splash.png").write_bytes(PNG + b"one")
    a = lib.Asset(folder=folder, name="Splash", type="images")
    assert not lib.record_files(a) and a.version == 1
    (folder / "splash.png").write_bytes(PNG + b"two")
    assert lib.record_files(a) and a.version == 2


# ---- where the Master is -----------------------------------------------------------------------
def test_the_master_is_found_by_preference_then_the_programs_setting(tmp_path, settings,
                                                                     monkeypatch):
    monkeypatch.setattr(mst, "program_setting", lambda: tmp_path / "From The Program")
    assert mst.master_dir(settings) == tmp_path / "From The Program"
    settings.set("master_library_dir", str(tmp_path / "Chosen"))
    assert mst.master_dir(settings) == tmp_path / "Chosen"
    with pytest.raises(mst.MasterError, match="no Master Asset Library"):
        mst.find_master(settings)


def test_the_programs_own_setting_is_read(tmp_path, monkeypatch):
    from PySide6.QtCore import QStandardPaths
    appdata = tmp_path / "AppData" / "Midgard"
    monkeypatch.setattr(QStandardPaths, "writableLocation", lambda _where: str(appdata))
    settings_file = tmp_path / "AppData" / "Master Asset Library" / "master_library.json"
    settings_file.parent.mkdir(parents=True)
    settings_file.write_text(json.dumps({"library_dir": "D:/Assets/Master"}))
    assert mst.program_setting() == Path("D:/Assets/Master")


# ---- from the Master into a project ------------------------------------------------------------
def test_assets_are_added_from_the_master_and_remember_where_from(project, master):
    original = master_asset(master, "textures/metal/rusty_plates", "Rusty Plates",
                            tags=["sci-fi"])
    copy = mst.add_to_project(project, master, original)
    assert copy.id == "textures/metal/rusty_plates"
    assert (copy.uid, copy.version, copy.fingerprint) == \
        (original.uid, original.version, original.fingerprint)
    assert copy.origin["library"] == master.id and copy.origin["version"] == 1
    assert lib.read(copy.folder).origin == copy.origin
    assert not mst.changed_here(copy)
    with pytest.raises(mst.MasterError, match="already"):
        mst.add_to_project(project, master, original)


def test_a_newer_master_version_is_offered_and_taken(project, master):
    original = master_asset(master, "images/logo", "Logo")
    copy = mst.add_to_project(project, master, original)
    assert mst.newer_in_master(copy, master) is None
    (original.folder / "logo.png").write_bytes(PNG + b"repainted")
    lib.record_files(original)
    original.description = "The new logo."
    lib.write(original)
    newer = mst.newer_in_master(copy, master)
    assert newer is not None and newer.version == 2
    mst.update_from_master(copy, master, newer)
    again = lib.read(copy.folder)
    assert (copy.folder / "logo.png").read_bytes() == PNG + b"repainted"
    assert again.version == 2 and again.origin["version"] == 2
    assert again.description == "The new logo."
    assert mst.newer_in_master(again, master) is None and not mst.changed_here(again)


# ---- from a project into the Master ------------------------------------------------------------
def test_a_projects_own_asset_is_sent_to_the_master(project, master):
    folder = lib.type_folder(project, "images") / "banner"
    folder.mkdir(parents=True)
    (folder / "banner.png").write_bytes(PNG + b"banner")
    a = lib.Asset(folder=folder, name="Banner", type="images", licence="CC0-1.0")
    lib.record_files(a)
    lib.write(a)
    given = mst.send_to_master(a, master)
    assert given.folder == master.root / "images" / "banner"
    assert given.uid == a.uid and given.origin is None
    # The project's asset is now a copy of the Master's.
    assert lib.read(a.folder).origin["library"] == master.id
    with pytest.raises(mst.MasterError, match="in the Master already"):
        mst.send_to_master(lib.read(a.folder), master)


def test_a_changed_copy_becomes_the_masters_next_version_when_wished(project, master):
    original = master_asset(master, "images/logo", "Logo")
    copy = mst.add_to_project(project, master, original)
    (copy.folder / "logo.png").write_bytes(PNG + b"improved here")
    lib.record_files(copy)
    lib.write(copy)
    assert mst.changed_here(copy)
    with pytest.raises(mst.MasterError, match="replace the Master's"):
        mst.send_to_master(copy, master)
    given = mst.send_to_master(copy, master, replace=True)
    assert given.version == 2
    assert (original.folder / "logo.png").read_bytes() == PNG + b"improved here"
    again = lib.read(copy.folder)
    assert again.origin["version"] == 2 and not mst.changed_here(again)


# ---- in the editor -----------------------------------------------------------------------------
@pytest.fixture
def library_window(qtbot, settings, project):
    from PySide6.QtWidgets import QWidget

    from midgard_editor.docks.asset_window import AssetWindow
    holder = QWidget()
    holder.settings = settings
    qtbot.addWidget(holder)
    window = AssetWindow(holder)
    window.quiet = True
    window.set_project(project)
    yield window
    del holder


def test_add_from_master_copies_the_chosen_assets(library_window, master):
    master_asset(master, "textures/metal/rusty_plates", "Rusty Plates", tags=["sci-fi"])
    master_asset(master, "images/logo", "Logo", data=PNG + b"logo")
    dialog = library_window.add_from_master(run=False)
    assert dialog.tree.topLevelItemCount() == 2
    dialog.search.setText("sci-fi")
    assert dialog.tree.topLevelItemCount() == 1
    dialog.search.setText("")
    dialog.choose("Logo")
    added = dialog.add_chosen()
    assert [a.name for a in added] == ["Logo"]
    assert "Added 1 asset" in dialog.report
    # Now in the project: shown so, and not chosen again.
    logo_row = next(dialog.tree.topLevelItem(i) for i in range(2)
                    if dialog.tree.topLevelItem(i).text(0).startswith("Logo"))
    assert logo_row.text(0) == "Logo  (in the project)"
    library_window.refresh()
    assert [a.name for a in library_window.assets] == ["Logo"]


def test_the_library_window_offers_the_masters_newer_version(library_window, master, project):
    original = master_asset(master, "images/logo", "Logo")
    copy = mst.add_to_project(project, master, original)
    library_window.refresh()
    library_window.select(copy.id)
    assert library_window.update_button.isHidden()
    assert "From the Master (version 1)" in library_window.info.text()
    (original.folder / "logo.png").write_bytes(PNG + b"v2")
    lib.record_files(original)
    lib.write(original)
    library_window.refresh()
    library_window.select(copy.id)
    assert not library_window.update_button.isHidden()
    assert "the Master has version 2" in library_window.info.text()
    library_window.update_from_master()
    assert lib.read(copy.folder).version == 2
    assert library_window.update_button.isHidden()


def test_send_to_master_from_the_library_window(library_window, master, project):
    folder = lib.type_folder(project, "images") / "banner"
    folder.mkdir(parents=True)
    (folder / "banner.png").write_bytes(PNG + b"banner")
    a = lib.Asset(folder=folder, name="Banner", type="images")
    lib.record_files(a)
    lib.write(a)
    library_window.refresh()
    library_window.select(a.id)
    given = library_window.send_to_master()
    assert given is not None and "is in the Master Asset Library" in library_window.last_report
    assert library_window.send_to_master() is None  # (unchanged since: nothing new)
    assert "nothing new" in library_window.last_report


def test_without_a_master_the_window_says_how_to_get_one(library_window, settings, tmp_path):
    settings.set("master_library_dir", str(tmp_path / "nowhere"))
    assert library_window.add_from_master(run=False) is None
    assert "open the Master Asset Library program once" in library_window.last_report


@pytest.mark.skipif(not vcs.available(), reason="git is not installed")
def test_commit_warns_about_purchased_originals_and_keeps_them_out(qtbot, project, master,
                                                                   tmp_path, monkeypatch):
    config = tmp_path / "gitconfig"
    config.write_text("[user]\n\tname = Midgard Tests\n\temail = tests@example.com\n", "utf-8")
    monkeypatch.setenv("GIT_CONFIG_GLOBAL", str(config))
    monkeypatch.setenv("GIT_CONFIG_NOSYSTEM", "1")
    vcs.put_under_git(project)
    bought = master_asset(master, "models/village", "Village", licence=lib.PURCHASED,
                          purchase={"seller": "Shop", "licence_file": "EULA.txt"},
                          extra_files={"EULA.txt": b"terms"})
    mst.add_to_project(project, master, bought)
    from midgard_editor.dialogs import CommitDialog
    dialog = CommitDialog(project)
    qtbot.addWidget(dialog)
    assert not dialog.purchased_note.isHidden()
    assert "Village" in dialog.purchased_note.text()
    assert any(p.startswith("library/models/village/") for p in dialog.ticked())
    dialog.keep_purchased_out()
    assert not any(p.startswith("library/models/village/") for p in dialog.ticked())
    assert ".gitignore" in dialog.ticked()
    gitignore = (project.root / ".gitignore").read_text("utf-8")
    assert "/library/models/village/" in gitignore
    dialog.message.setPlainText("Add the village")
    dialog._commit()
    assert dialog.commit_id
    assert not any(f.path.startswith("library/models/village")
                   for f in vcs.changed_files(project))  # (ignored now)
    assert (project.root / "library/models/village/village.png").is_file()  # (still there)
