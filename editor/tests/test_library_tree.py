# SPDX-License-Identifier: Apache-2.0
"""Collections and their order (library format version 3). The same tests are in the Master
Asset Library (tests/test_library_tree.py), as library_tree is the same file in both."""
import json
from pathlib import Path

import pytest

from midgard_editor import library_tree as lt

TYPES = [("textures", "Textures"), ("effects", "FX"), ("models", "Models")]


def asset(root: Path, path: str, name: str, category: str = "") -> tuple[str, str, str]:
    folder = root / path
    folder.mkdir(parents=True)
    (folder / "asset.json").write_text(json.dumps(
        {"format": 2, "name": name, "type": path.split("/")[0], "category": category}))
    return path, name, category


def names(node: lt.Node) -> list[str]:
    return [c.name for c in node.children]


def test_a_library_without_collection_files_shows_as_before(tmp_path):
    assets = [asset(tmp_path, "textures/metal/rusty", "Rusty", "metal"),
              asset(tmp_path, "textures/metal/bright", "Bright", "metal"),
              asset(tmp_path, "textures/wood/oak", "Oak", "wood"),
              asset(tmp_path, "models/zebra", "Zebra"),
              asset(tmp_path, "models/apple", "Apple")]
    textures, effects, models = lt.tree(tmp_path, TYPES, assets)
    assert (textures.name, textures.kind) == ("Textures", "type")
    assert names(textures) == ["Metal", "Wood"]  # (kinds, tidied)
    assert names(textures.children[0]) == ["Bright", "Rusty"]  # A-Z
    assert textures.children[0].path == "textures/metal"
    assert textures.children[0].children[0].kind == "asset"
    assert effects.children == []  # (no folder yet: nothing in it)
    assert names(models) == ["Apple", "Zebra"]


def test_collections_nest_as_deep_as_you_like_and_come_before_assets(tmp_path):
    assets = [asset(tmp_path, "effects/nox/grass/walk", "Walk"),
              asset(tmp_path, "effects/nox/grass/deep/run", "Run"),
              asset(tmp_path, "effects/nox/bang", "Bang")]
    effects = lt.tree(tmp_path, TYPES, assets)[1]
    nox = effects.children[0]
    assert names(nox) == ["Grass", "Bang"]  # (a collection, then an asset)
    grass = nox.children[0]
    assert [(c.name, c.kind) for c in grass.children] == [("Deep", "collection"),
                                                          ("Walk", "asset")]
    assert lt.find([effects], "effects/nox/grass/deep/run").name == "Run"
    assert lt.collection_of("effects/nox/grass/deep/run") == "nox/grass/deep"
    assert lt.collection_of("models/zebra") == ""


def test_an_album_keeps_the_name_it_was_imported_with(tmp_path):
    assets = [asset(tmp_path, "effects/nox_footsteps/walk", "Walk", "NOX Footsteps")]
    effects = lt.tree(tmp_path, TYPES, assets)[1]
    assert names(effects) == ["NOX Footsteps"]


def test_empty_collections_are_shown_and_broken_assets_left_out(tmp_path):
    (tmp_path / "models" / "props" / "empty").mkdir(parents=True)
    broken = tmp_path / "models" / "props" / "broken"
    broken.mkdir()
    (broken / "asset.json").write_text("not json")
    models = lt.tree(tmp_path, TYPES, [])[2]  # (the scan couldn't read "broken")
    props = models.children[0]
    assert props.name == "Props" and names(props) == ["Empty"]
    assert props.children[0].kind == "collection"


def test_a_chosen_name_and_order_are_kept(tmp_path):
    assets = [asset(tmp_path, "models/apple", "Apple"),
              asset(tmp_path, "models/zebra", "Zebra"),
              asset(tmp_path, "models/props/chair", "Chair"),
              asset(tmp_path, "models/mango", "Mango")]
    lt.write_info(tmp_path / "models", order=["zebra", "props", "apple"])
    lt.write_info(tmp_path / "models" / "props", name="Furniture & Props")
    models = lt.tree(tmp_path, TYPES, assets)[2]
    # The chosen order, then the rest (mango, not in it) at the end.
    assert names(models) == ["Zebra", "Furniture & Props", "Apple", "Mango"]


def test_collection_json_keeps_what_isnt_changed(tmp_path):
    lt.write_info(tmp_path, name="Props", order=["a", "b"])
    lt.write_info(tmp_path, order=["b", "a"])
    assert lt.read_info(tmp_path) == {"name": "Props", "order": ["b", "a"]}
    doc = json.loads((tmp_path / "collection.json").read_text())
    assert doc == {"format": 3, "name": "Props", "order": ["b", "a"]}
    assert not (tmp_path / "collection.json.tmp").exists()


def test_a_broken_collection_json_never_stops_the_library_opening(tmp_path):
    assets = [asset(tmp_path, "models/apple", "Apple"), asset(tmp_path, "models/kiwi", "Kiwi")]
    (tmp_path / "models" / "collection.json").write_text("{broken")
    assert names(lt.tree(tmp_path, TYPES, assets)[2]) == ["Apple", "Kiwi"]
    (tmp_path / "models" / "collection.json").write_text('{"order": "kiwi", "name": 7}')
    assert lt.read_info(tmp_path / "models") == {"name": "7", "order": []}


def test_hidden_folders_are_not_collections(tmp_path):
    (tmp_path / "models" / ".thumbnails").mkdir(parents=True)
    assert lt.tree(tmp_path, TYPES, [])[2].children == []


# ---- organizing --------------------------------------------------------------------------------
def shown(root: Path, path: str) -> list[str]:
    return [n.name for n in lt.children(root, path)]


def doc(root: Path, path: str) -> dict:
    return json.loads((root / path / "asset.json").read_text())


def test_renaming_an_asset_renames_its_folder_and_keeps_its_place(tmp_path):
    asset(tmp_path, "models/apple", "Apple")
    asset(tmp_path, "models/kiwi", "Kiwi")
    lt.write_info(tmp_path / "models", order=["kiwi", "apple"])
    moved = lt.rename_asset(tmp_path, "models/apple", "Green Apple")
    assert moved == [("models/apple", "models/green_apple")]
    assert doc(tmp_path, "models/green_apple")["name"] == "Green Apple"
    assert shown(tmp_path, "models") == ["Kiwi", "Green Apple"]  # (its place kept)
    # Only the name changes when the folder already fits it.
    assert lt.rename_asset(tmp_path, "models/green_apple", "GREEN apple") == []
    assert doc(tmp_path, "models/green_apple")["name"] == "GREEN apple"


def test_a_taken_folder_name_is_numbered_and_a_numbered_one_kept(tmp_path):
    asset(tmp_path, "models/walk", "Walk")
    asset(tmp_path, "models/run", "Run")
    assert lt.rename_asset(tmp_path, "models/run", "Walk") == [("models/run", "models/walk_2")]
    assert lt.rename_asset(tmp_path, "models/walk_2", "walk") == []  # (still fits)


def test_renaming_with_the_programs_own_writer(tmp_path):
    asset(tmp_path, "models/apple", "Apple")
    calls = []
    lt.rename_asset(tmp_path, "models/apple", "Pear",
                    lambda folder, fields: calls.append((folder.name, fields)))
    assert calls == [("apple", {"name": "Pear"})]


def test_renaming_a_collection(tmp_path):
    asset(tmp_path, "effects/nox_footsteps/walk", "Walk", "NOX Footsteps")
    asset(tmp_path, "effects/bang", "Bang")
    lt.write_info(tmp_path / "effects", order=["bang", "nox_footsteps"])
    moved = lt.rename_collection(tmp_path, "effects/nox_footsteps", "Steps")
    assert moved == [("effects/nox_footsteps", "effects/steps")]
    assert shown(tmp_path, "effects") == ["Bang", "Steps"]
    assert (tmp_path / "effects/steps/walk/asset.json").is_file()
    assert lt.rename_collection(tmp_path, "effects/steps", "STEPS!") == []
    assert shown(tmp_path, "effects") == ["Bang", "STEPS!"]


def test_types_cant_be_renamed_moved_or_deleted(tmp_path):
    asset(tmp_path, "models/apple", "Apple")
    with pytest.raises(lt.OrganizeError, match="type"):
        lt.rename_collection(tmp_path, "models", "Things")
    with pytest.raises(lt.OrganizeError, match="type"):
        lt.delete_collection(tmp_path, "models")
    with pytest.raises(lt.OrganizeError, match="type"):
        lt.move(tmp_path, ["models"], "effects")
    with pytest.raises(lt.OrganizeError, match="asset"):
        lt.rename_collection(tmp_path, "models/apple", "Pear")
    with pytest.raises(lt.OrganizeError):
        lt.rename_asset(tmp_path, "models/apple", "  ")
    with pytest.raises(lt.OrganizeError):
        lt.rename_asset(tmp_path, "models/../models/apple", "Pear")


def test_new_collections_go_at_the_end_or_where_asked(tmp_path):
    asset(tmp_path, "models/apple", "Apple")
    asset(tmp_path, "models/kiwi", "Kiwi")
    assert lt.new_collection(tmp_path, "models", "Props") == "models/props"
    assert shown(tmp_path, "models") == ["Apple", "Kiwi", "Props"]  # (not first: at the end)
    assert lt.new_collection(tmp_path, "models", "Props", index=0) == "models/props_2"
    assert shown(tmp_path, "models") == ["Props", "Apple", "Kiwi", "Props"]
    assert lt.new_collection(tmp_path, "models/props", "Old Chairs") == "models/props/old_chairs"
    assert shown(tmp_path, "models/props") == ["Old Chairs"]
    # A type with nothing in it yet takes one too.
    assert lt.new_collection(tmp_path, "objects", "Doors") == "objects/doors"


def test_deleting_a_collection_only_when_it_holds_no_assets(tmp_path):
    asset(tmp_path, "models/props/chair", "Chair")
    lt.new_collection(tmp_path, "models", "Empty")
    lt.new_collection(tmp_path, "models/empty", "Emptier")
    with pytest.raises(lt.OrganizeError, match="1 asset"):
        lt.delete_collection(tmp_path, "models/props")
    (tmp_path / "models/empty/notes.txt").write_text("mine")
    with pytest.raises(lt.OrganizeError, match="notes.txt"):
        lt.delete_collection(tmp_path, "models/empty")
    (tmp_path / "models/empty/notes.txt").unlink()
    lt.delete_collection(tmp_path, "models/empty")  # (with the empty one in it)
    assert not (tmp_path / "models/empty").exists()
    assert "empty" not in lt.read_info(tmp_path / "models")["order"]


def test_moving_assets_and_collections_into_a_collection(tmp_path):
    asset(tmp_path, "models/apple", "Apple")
    asset(tmp_path, "models/kiwi", "Kiwi")
    asset(tmp_path, "models/props/chair", "Chair")
    asset(tmp_path, "models/props/table", "Table")
    moved = lt.move(tmp_path, ["models/kiwi", "models/apple"], "models/props", index=1)
    assert moved == [("models/kiwi", "models/props/kiwi"), ("models/apple", "models/props/apple")]
    assert shown(tmp_path, "models/props") == ["Chair", "Kiwi", "Apple", "Table"]
    assert shown(tmp_path, "models") == ["Props"]
    # A collection, with all that is in it; a name taken there is numbered.
    asset(tmp_path, "models/fruit/kiwi", "Kiwi")
    moved = lt.move(tmp_path, ["models/fruit"], "models/props")
    assert moved == [("models/fruit", "models/props/fruit")]
    moved = lt.move(tmp_path, ["models/props/fruit/kiwi"], "models/props")
    assert moved == [("models/props/fruit/kiwi", "models/props/kiwi_2")]


def test_moving_within_a_place_changes_only_the_order(tmp_path):
    for name in ("a", "b", "c", "d"):
        asset(tmp_path, f"models/{name}", name.upper())
    assert lt.move(tmp_path, ["models/d", "models/c"], "models", index=0) == []
    assert shown(tmp_path, "models") == ["D", "C", "A", "B"]


def test_something_and_what_is_in_it_chosen_together_move_once(tmp_path):
    asset(tmp_path, "models/props/chair", "Chair")
    lt.new_collection(tmp_path, "models", "Store")
    moved = lt.move(tmp_path, ["models/props/chair", "models/props"], "models/store")
    assert moved == [("models/props", "models/store/props")]


def test_a_collection_cant_go_inside_itself(tmp_path):
    asset(tmp_path, "models/props/old/chair", "Chair")
    with pytest.raises(lt.OrganizeError, match="inside itself"):
        lt.move(tmp_path, ["models/props"], "models/props/old")
    with pytest.raises(lt.OrganizeError, match="inside itself"):
        lt.move(tmp_path, ["models/props"], "models/props")


def test_assets_move_between_similar_types_and_take_the_new_type(tmp_path):
    asset(tmp_path, "music/album/song", "Song")
    asset(tmp_path, "music/album/tune", "Tune")
    moved = lt.move(tmp_path, ["music/album"], "effects")
    assert moved == [("music/album", "effects/album")]
    assert doc(tmp_path, "effects/album/song")["type"] == "effects"
    assert doc(tmp_path, "effects/album/tune")["type"] == "effects"
    asset(tmp_path, "textures/metal/rusty", "Rusty")
    with pytest.raises(lt.OrganizeError, match="similar types"):
        lt.move(tmp_path, ["textures/metal/rusty"], "models")
    # An icon's .ico file can't go to images.
    asset(tmp_path, "icons/app", "App")
    (tmp_path / "icons/app/app.ico").write_bytes(b"ico")
    with pytest.raises(lt.OrganizeError, match="only be in icons"):
        lt.move(tmp_path, ["icons/app"], "images")
    assert lt.similar("gui", "icons") and not lt.similar("music", "images")


def test_a_collections_name_stays_when_its_assets_move_out(tmp_path):
    # Its name came from its album (in its assets); moving them out keeps it.
    asset(tmp_path, "effects/nox_footsteps/walk", "Walk", "NOX Footsteps")
    lt.move(tmp_path, ["effects/nox_footsteps/walk"], "effects")
    assert "NOX Footsteps" in shown(tmp_path, "effects")
    assert lt.read_info(tmp_path / "effects/nox_footsteps")["name"] == "NOX Footsteps"


def test_a_change_that_fails_part_way_is_undone(tmp_path, monkeypatch):
    asset(tmp_path, "models/apple", "Apple")
    asset(tmp_path, "models/kiwi", "Kiwi")
    lt.new_collection(tmp_path, "models", "Fruit")
    before = {p.relative_to(tmp_path).as_posix(): p.read_bytes()
              for p in tmp_path.rglob("*") if p.is_file()}
    real = Path.rename

    def rename(self, target):  # the second folder is in use
        if self.name == "kiwi":
            raise PermissionError(13, "The file is in use")
        return real(self, target)
    monkeypatch.setattr(Path, "rename", rename)
    with pytest.raises(lt.OrganizeError, match="in use"):
        lt.move(tmp_path, ["models/apple", "models/kiwi"], "models/fruit")
    monkeypatch.undo()
    after = {p.relative_to(tmp_path).as_posix(): p.read_bytes()
             for p in tmp_path.rglob("*") if p.is_file()}
    assert after == before


def test_move_up_and_down(tmp_path):
    for name in ("a", "b", "c"):
        asset(tmp_path, f"models/{name}", name.upper())
    lt.move_by(tmp_path, "models/c", -1)
    assert shown(tmp_path, "models") == ["A", "C", "B"]
    lt.move_by(tmp_path, "models/a", -1)  # (already first)
    lt.move_by(tmp_path, "models/a", +1)
    assert shown(tmp_path, "models") == ["C", "A", "B"]


def test_sort_a_to_z_puts_collections_first(tmp_path):
    asset(tmp_path, "models/zebra", "Zebra")
    asset(tmp_path, "models/apple", "apple")
    lt.new_collection(tmp_path, "models", "Props")
    lt.move_by(tmp_path, "models/apple", +1)
    lt.sort_a_z(tmp_path, "models")
    assert shown(tmp_path, "models") == ["Props", "apple", "Zebra"]
    asset(tmp_path, "models/mango", "Mango")  # (new: at the end)
    assert shown(tmp_path, "models") == ["Props", "apple", "Zebra", "Mango"]


def test_the_last_part_of_a_change_is_given_what_moved_and_can_undo_it(tmp_path):
    asset(tmp_path, "models/apple", "Apple")
    asset(tmp_path, "music/song", "Song")
    lt.new_collection(tmp_path, "models", "Box")
    seen = []
    lt.rename_asset(tmp_path, "models/apple", "Pear", then=seen.append)
    assert seen == [[("models/apple", "models/pear")]]
    lt.rename_asset(tmp_path, "models/pear", "PEAR", then=seen.append)  # (nothing moved)
    assert len(seen) == 1
    before = {p.relative_to(tmp_path).as_posix(): p.read_bytes()
              for p in tmp_path.rglob("*") if p.is_file()}

    def fails(moved):
        raise lt.OrganizeError("the game couldn't be changed")
    with pytest.raises(lt.OrganizeError, match="game"):
        lt.rename_asset(tmp_path, "models/pear", "Plum", then=fails)
    with pytest.raises(lt.OrganizeError, match="game"):
        lt.rename_collection(tmp_path, "models/box", "Crate", then=fails)
    with pytest.raises(lt.OrganizeError, match="game"):
        lt.move(tmp_path, ["models/pear"], "models/box", then=fails)
    with pytest.raises(lt.OrganizeError, match="game"):
        lt.move(tmp_path, ["music/song"], "effects", then=fails)  # (its type put back too)
    after = {p.relative_to(tmp_path).as_posix(): p.read_bytes()
             for p in tmp_path.rglob("*") if p.is_file()}
    assert after == before
