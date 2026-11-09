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
import contextlib
import json
import re
import shutil
from collections.abc import Callable, Iterable
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


# ---- organizing: renaming, moving, ordering --------------------------------------------------
# Each change is all or nothing: if any part of it fails (a file in use, say), what it had done is
# put back, and OrganizeError says why. Each returns the folders it moved, (old path, new path) in
# the library, so a game using them can be changed to match (Midgard): given `then`, it is called
# with them as the change's last part (when any moved), and if it raises, the change is undone.
SIMILAR = (("music", "ambiance", "effects"), ("images", "gui", "icons"))  # may swap assets
ONLY_IN = {".ico": "icons"}  # files only one type may hold
LICENCE_FILE = "LICENSE.md"

Moved = list[tuple[str, str]]
Then = Callable[[Moved], None]  # the last part of a change: given the folders moved
UpdateAsset = Callable[[Path, dict], None]  # writes fields of an asset (its asset.json, LICENSE.md)


class OrganizeError(Exception):
    """Shown to the user; nothing was changed."""


def similar(type_a: str, type_b: str) -> bool:
    """Whether assets may move between two types."""
    return type_a == type_b or any(type_a in g and type_b in g for g in SIMILAR)


def write_asset_fields(folder: Path, fields: dict) -> None:
    """Changes fields of an asset's asset.json, keeping the rest (programs pass their own, which
    also writes its LICENSE.md)."""
    path = Path(folder) / METADATA
    doc = json.loads(path.read_text("utf-8"))
    doc.update(fields)
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(doc, indent=2) + "\n", "utf-8")
    tmp.replace(path)


class _Journal:
    """What a change has done, so it can be undone when a later part of it fails."""

    def __init__(self) -> None:
        self._undo: list[Callable[[], None]] = []

    def rename(self, source: Path, target: Path) -> None:
        try:
            source.rename(target)
        except OSError as e:
            raise OrganizeError(f"{source.name} can't be moved or renamed (is one of its files "
                                f"open in another program?): {e.strerror or e}") from e
        self._undo.append(lambda: target.rename(source))

    def mkdir(self, folder: Path) -> None:
        folder.mkdir(parents=True)
        self._undo.append(lambda: shutil.rmtree(folder, ignore_errors=True))

    def keep(self, *files: Path) -> None:
        """Remembers files as they are, before they change."""
        for file in files:
            if file.is_file():
                data = file.read_bytes()
                self._undo.append(lambda file=file, data=data: file.write_bytes(data))
            else:
                self._undo.append(lambda file=file: file.unlink(missing_ok=True))

    def __enter__(self) -> "_Journal":
        return self

    def __exit__(self, kind, error, tb) -> bool:
        if error is None:
            return False
        for undo in reversed(self._undo):
            with contextlib.suppress(OSError):  # (put back as much as can be)
                undo()
        if isinstance(error, OSError):
            raise OrganizeError(f"The change couldn't be made, and was undone: "
                                f"{error.strerror or error}") from error
        return False


def _folder(root: Path, path: str) -> Path:
    """The folder of a path in the library (it must be there)."""
    parts = path.split("/")
    if not path or any(p in ("", ".", "..") for p in parts):
        raise OrganizeError(f"'{path}' isn't a place in the library")
    folder = Path(root).joinpath(*parts)
    if not folder.is_dir():
        raise OrganizeError(f"{path} isn't in the library (has it been moved?)")
    return folder


def _parent(path: str) -> str:
    return path.rsplit("/", 1)[0]


def _check_collection(root: Path, path: str) -> Path:
    if "/" not in path:
        raise OrganizeError("a type can't be renamed, moved or deleted")
    folder = _folder(root, path)
    if is_asset(folder):
        raise OrganizeError(f"{path} is an asset, not a collection")
    return folder


def _container(root: Path, path: str) -> Path:
    """A type's folder (made if needed) or a collection's: where things can go."""
    if "/" not in path:
        if not path or path in (".", ".."):
            raise OrganizeError(f"'{path}' isn't a place in the library")
        folder = Path(root) / path
        folder.mkdir(parents=True, exist_ok=True)
        return folder
    return _check_collection(root, path)


def _assets_in(folder: Path, root: Path) -> dict[str, tuple[str, str]]:
    """Every asset in a folder (and in its collections): path -> (name, category)."""
    found = {}
    for meta in Path(folder).rglob(METADATA):
        try:
            doc = json.loads(meta.read_text("utf-8"))
        except (OSError, ValueError):
            continue
        if isinstance(doc, dict):
            found[meta.parent.relative_to(root).as_posix()] = (
                str(doc.get("name") or meta.parent.name), str(doc.get("category") or ""))
    return found


def children(root: Path, path: str) -> list[Node]:
    """What is in a type or collection, in its order (each with what is in it)."""
    root = Path(root)
    folder = root.joinpath(*path.split("/"))
    return _entries(folder, root, _assets_in(folder, root)) if folder.is_dir() else []


def _order_now(root: Path, path: str) -> list[str]:
    return [n.folder_name for n in children(root, path)]


def _target(parent: Path, name: str, current: Path | None = None) -> Path:
    """The folder for a name in `parent`: its slug, or numbered (_2, _3 …) when that is taken.
    A folder already named for it (`current`, perhaps numbered) keeps its name."""
    base = slug(name)
    if current is not None and current.parent == parent and \
            re.fullmatch(re.escape(base) + r"(_\d+)?", current.name):
        return current
    folder, n = parent / base, 2
    while folder.exists():
        folder = parent / f"{base}_{n}"
        n += 1
    return folder


def _renamed_in_order(journal: _Journal, parent: Path, old: str, new: str) -> None:
    info = read_info(parent)
    if old in info["order"]:
        journal.keep(parent / COLLECTION_FILE)
        write_info(parent, order=[new if n == old else n for n in info["order"]])


def _pin_name(journal: _Journal, root: Path, path: str) -> None:
    """Writes a collection's shown name into its collection.json when it has none there, so
    moving its assets about never changes it (it may come from one of them)."""
    if "/" not in path:
        return
    folder = Path(root).joinpath(*path.split("/"))
    if not folder.is_dir() or is_asset(folder) or read_info(folder)["name"]:
        return
    node = next((n for n in children(root, _parent(path)) if n.path == path), None)
    if node is not None and node.name != tidy(folder.name):
        journal.keep(folder / COLLECTION_FILE)
        write_info(folder, name=node.name)


def rename_asset(root: Path, path: str, name: str,
                 update_asset: UpdateAsset | None = None, then: Then | None = None) -> Moved:
    """Renames an asset; its folder follows its new name."""
    root, name = Path(root), name.strip()
    if not name:
        raise OrganizeError("an asset needs a name")
    folder = _folder(root, path)
    if not is_asset(folder):
        raise OrganizeError(f"{path} isn't an asset")
    with _Journal() as journal:
        journal.keep(folder / METADATA, folder / LICENCE_FILE)
        (update_asset or write_asset_fields)(folder, {"name": name})
        target = _target(folder.parent, name, folder)
        if target == folder:
            return []
        journal.rename(folder, target)
        _renamed_in_order(journal, folder.parent, folder.name, target.name)
        moved = [(path, target.relative_to(root).as_posix())]
        if then is not None:
            then(moved)
    return moved


def rename_collection(root: Path, path: str, name: str, then: Then | None = None) -> Moved:
    """Renames a collection; its folder follows its new name."""
    root, name = Path(root), name.strip()
    if not name:
        raise OrganizeError("a collection needs a name")
    folder = _check_collection(root, path)
    with _Journal() as journal:
        journal.keep(folder / COLLECTION_FILE)
        write_info(folder, name=name)
        target = _target(folder.parent, name, folder)
        if target == folder:
            return []
        journal.rename(folder, target)
        _renamed_in_order(journal, folder.parent, folder.name, target.name)
        moved = [(path, target.relative_to(root).as_posix())]
        if then is not None:
            then(moved)
    return moved


def new_collection(root: Path, parent: str, name: str, index: int | None = None) -> str:
    """Makes a collection in a type or a collection (at the end, or at `index` in its order);
    returns its path."""
    root, name = Path(root), name.strip()
    if not name:
        raise OrganizeError("a collection needs a name")
    container = _container(root, parent)
    order = _order_now(root, parent)
    with _Journal() as journal:
        folder = _target(container, name)
        journal.mkdir(folder)
        write_info(folder, name=name)
        order.insert(len(order) if index is None else max(0, min(index, len(order))),
                     folder.name)
        journal.keep(container / COLLECTION_FILE)
        write_info(container, order=order)
    return folder.relative_to(root).as_posix()


def delete_collection(root: Path, path: str) -> None:
    """Deletes a collection, with any empty collections in it; refused while it holds assets
    or any other files."""
    root = Path(root)
    folder = _check_collection(root, path)
    held = [p for p in folder.rglob("*") if p.is_file() and p.name != COLLECTION_FILE]
    if held:
        count = sum(1 for p in held if p.name == METADATA)
        raise OrganizeError(
            f"it still holds {count} asset{'s' if count != 1 else ''}: move them out first"
            if count else f"it holds files that aren't assets ({held[0].name})")
    parent = folder.parent
    with _Journal() as journal:
        info = read_info(parent)
        if folder.name in info["order"]:
            journal.keep(parent / COLLECTION_FILE)
            write_info(parent, order=[n for n in info["order"] if n != folder.name])
        shutil.rmtree(folder)  # (an OSError here undoes the order's change)


def move(root: Path, paths: list[str], to: str, index: int | None = None,
         update_asset: UpdateAsset | None = None, then: Then | None = None) -> Moved:
    """Moves assets and collections into a type or collection (`to`), at `index` in its order
    (at the end when None), keeping their order among themselves. Moving within the same place
    puts them in a new order. Between types, only similar ones (SIMILAR), and every asset moved
    takes the new type."""
    root = Path(root)
    # (one inside another chosen goes with it)
    chosen = [p for p in dict.fromkeys(paths)
              if not any(p.startswith(q + "/") for q in paths if q != p)]
    if not chosen:
        return []
    to_type = to.split("/")[0]
    for path in chosen:
        if "/" not in path:
            raise OrganizeError("a type can't be moved")
        folder = _folder(root, path)
        if to == path or to.startswith(path + "/"):
            raise OrganizeError(f"{folder.name} can't go inside itself")
        from_type = path.split("/")[0]
        if not similar(from_type, to_type):
            raise OrganizeError(f"{folder.name} can't move from {from_type} to {to_type}: assets "
                                "move only between similar types (music, ambiance and FX; "
                                "images, GUI and icons)")
        if from_type != to_type:
            for file in folder.rglob("*"):
                only = ONLY_IN.get(file.suffix.lower())
                if only and only != to_type:
                    raise OrganizeError(f"{file.name} can only be in {only}")
    container = _container(root, to)
    order = [n for n in _order_now(root, to)
             if f"{to}/{n}" not in chosen]  # (those moving within it are placed again)
    at = len(order) if index is None else max(0, min(index, len(order)))
    sources = {_parent(p) for p in chosen if _parent(p) != to}
    moved: Moved = []
    with _Journal() as journal:
        for path in (to, *sorted(sources), *chosen):
            _pin_name(journal, root, path)
        placed = []
        for path in chosen:
            folder = _folder(root, path)
            target = folder if folder.parent == container else _target(container, folder.name)
            if target != folder:
                journal.rename(folder, target)
                moved.append((path, target.relative_to(root).as_posix()))
            placed.append(target.name)
            if path.split("/")[0] != to_type:
                for meta in sorted(target.rglob(METADATA)):
                    journal.keep(meta, meta.parent / LICENCE_FILE)
                    (update_asset or write_asset_fields)(meta.parent, {"type": to_type})
        journal.keep(container / COLLECTION_FILE)
        write_info(container, order=order[:at] + placed + order[at:])
        for source in sorted(sources):
            folder = root.joinpath(*source.split("/"))
            info = read_info(folder)
            gone = {p.rsplit("/", 1)[1] for p in chosen if _parent(p) == source}
            if gone & set(info["order"]):
                journal.keep(folder / COLLECTION_FILE)
                write_info(folder, order=[n for n in info["order"] if n not in gone])
        if moved and then is not None:
            then(moved)
    return moved


def move_by(root: Path, path: str, step: int) -> None:
    """Moves an asset or collection up (-1) or down (+1) in its order (Move Up, Move Down)."""
    parent = _parent(path)
    order = _order_now(root, parent)
    name = path.rsplit("/", 1)[-1]
    if "/" not in path or name not in order:
        raise OrganizeError(f"{path} isn't in the library (has it been moved?)")
    i = order.index(name)
    j = max(0, min(i + step, len(order) - 1))
    if i != j:
        order.insert(j, order.pop(i))
        write_info(_container(root, parent), order=order)


def sort_a_z(root: Path, path: str) -> None:
    """Puts what is in a type or collection in A-Z order, collections first (Sort A-Z)."""
    nodes = sorted(children(root, path), key=lambda n: (n.kind != "collection",
                                                         n.name.casefold()))
    write_info(_container(root, path), order=[n.folder_name for n in nodes])
