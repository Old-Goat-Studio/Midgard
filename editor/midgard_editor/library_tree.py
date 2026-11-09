# SPDX-License-Identifier: Apache-2.0
"""Collections and their order (the library format, version 3).

This file is the same in the Master Asset Library (master_library/library_tree.py) and in Midgard
(midgard_editor/library_tree.py): change both together.

Inside each type's folder, any folder that isn't an asset is a **collection**, and collections
may hold collections, as deep as you like:

    effects/                              a type (fixed)
        collection.json                   the order of what is in it
        nox_footsteps/                    a collection
            collection.json               its name, and the order of what is in it
            grass/                        a collection inside it
                footsteps_grass_walk/     an asset (it has an asset.json)

collection.json: {"format": 3, "name": "NOX Footsteps", "order": ["grass", "stone", ...]}
- `name`: the name shown (a type's folder has none: its name is the type's). Without one, the
  name is the album an asset in it was imported with, or else the folder's name tidied.
- `order`: the folder names of what is in it, collections and assets mixed, in the order chosen.
  Anything not listed (new, or added by hand) comes after, collections first, then by name.
Both are optional, the file too: a library without any is shown as before, in A-Z order."""
import json
import re
from collections.abc import Iterable
from dataclasses import dataclass, field
from pathlib import Path

FORMAT = 3
COLLECTION_FILE = "collection.json"
METADATA = "asset.json"


def slug(name: str) -> str:
    """A folder name from a name: lower case, words joined by underscores."""
    s = re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")
    return s or "asset"


def tidy(folder_name: str) -> str:
    """A folder's name as shown when it has none of its own: "nox_footsteps" -> "Nox footsteps"."""
    return folder_name.replace("_", " ").capitalize()


def is_asset(folder: Path) -> bool:
    return (Path(folder) / METADATA).is_file()


def collection_of(asset_path: str) -> str:
    """The collection an asset is in, from its path in the library:
    "effects/nox_footsteps/grass/walk" -> "nox_footsteps/grass" ("" when it is in its type's
    folder itself)."""
    return "/".join(asset_path.split("/")[1:-1])


# ---- collection.json ---------------------------------------------------------------------------
def read_info(folder: Path) -> dict:
    """A folder's collection.json as {"name": str, "order": [str]} (empty when there is none, or
    it can't be read: a broken file never stops the library opening)."""
    try:
        doc = json.loads((Path(folder) / COLLECTION_FILE).read_text("utf-8"))
    except (OSError, ValueError):
        return {"name": "", "order": []}
    if not isinstance(doc, dict):
        return {"name": "", "order": []}
    order = doc.get("order")
    return {"name": str(doc.get("name") or ""),
            "order": [str(n) for n in order if isinstance(n, str)] if isinstance(order, list)
            else []}


def write_info(folder: Path, *, name: str | None = None, order: list[str] | None = None) -> None:
    """Changes a folder's collection.json (what isn't given is kept as it is)."""
    folder = Path(folder)
    info = read_info(folder)
    if name is not None:
        info["name"] = name
    if order is not None:
        info["order"] = list(order)
    doc = {"format": FORMAT}
    if info["name"]:
        doc["name"] = info["name"]
    doc["order"] = info["order"]
    path = folder / COLLECTION_FILE
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(doc, indent=2) + "\n", "utf-8")
    tmp.replace(path)


# ---- the tree ----------------------------------------------------------------------------------
@dataclass
class Node:
    """A type, a collection or an asset, as shown."""
    path: str      # its folder from the library's: "effects/nox_footsteps" ("effects" for a type)
    name: str      # as shown
    kind: str      # "type", "collection" or "asset"
    children: list["Node"] = field(default_factory=list)

    @property
    def folder_name(self) -> str:
        return self.path.rsplit("/", 1)[-1]

    def walk(self) -> Iterable["Node"]:
        """It, then everything in it, in order."""
        yield self
        for child in self.children:
            yield from child.walk()


def _entries(folder: Path, root: Path, assets: dict[str, tuple[str, str]]) -> list[Node]:
    """What is in a folder (collections and assets), in order."""
    nodes: list[Node] = []
    try:
        children = [c for c in Path(folder).iterdir() if c.is_dir() and
                    not c.name.startswith(".")]
    except OSError:
        return nodes
    for child in children:
        path = child.relative_to(root).as_posix()
        if is_asset(child):
            if path in assets:  # (one that can't be read is reported by the scan instead)
                nodes.append(Node(path, assets[path][0], "asset"))
            continue
        node = Node(path, "", "collection", _entries(child, root, assets))
        node.name = read_info(child)["name"] or _album_name(node, assets) or tidy(child.name)
        nodes.append(node)
    return in_order(nodes, read_info(folder)["order"])


def _album_name(collection: Node, assets: dict[str, tuple[str, str]]) -> str:
    """The album (or kind) an asset directly in the collection was imported with, when the
    collection's folder was made from it ("NOX Footsteps" for nox_footsteps)."""
    for child in collection.children:
        category = assets.get(child.path, ("", ""))[1] if child.kind == "asset" else ""
        if category and slug(category) == collection.folder_name and \
                category.lower() != collection.folder_name:
            return category
    return ""


def in_order(nodes: list[Node], order: list[str]) -> list[Node]:
    """Nodes in a chosen order (folder names); the rest after it, collections first, then by
    name."""
    place = {name: i for i, name in enumerate(order)}
    return sorted(nodes, key=lambda n: (
        place.get(n.folder_name, len(place)),
        n.folder_name in place or n.kind != "collection",
        n.name.casefold()))


def tree(root: Path, types: Iterable[tuple[str, str]],
         assets: Iterable[tuple[str, str, str]]) -> list[Node]:
    """The library as shown: one node a type (`types`: its folder and name, in the order shown),
    each holding its collections and assets in their order. `assets` are the assets read (their
    path in the library, name and category); empty collections are included."""
    root = Path(root)
    known = {path: (name, category) for path, name, category in assets}
    return [Node(type_, label, "type", _entries(root / type_, root, known))
            for type_, label in types]


def find(nodes: Iterable[Node], path: str) -> Node | None:
    for top in nodes:
        for node in top.walk():
            if node.path == path:
                return node
    return None
