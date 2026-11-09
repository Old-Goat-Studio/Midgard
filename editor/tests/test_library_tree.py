# SPDX-License-Identifier: Apache-2.0
"""Collections and their order (library format version 3). The same tests are in the Master
Asset Library (tests/test_library_tree.py), as library_tree is the same file in both."""
import json
from pathlib import Path

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
