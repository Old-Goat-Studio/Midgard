# SPDX-License-Identifier: Apache-2.0
"""A project's Asset Library (milestone 5.13a): the assets it may use, organised, each with its
description and licence.

It lives in the project's own `library/` folder, one folder per type (and per kind, for
textures), one folder per asset:

    library/textures/metal/rusty_plates/       an asset: its files, and
        asset.json                             what the editor reads (below)
        LICENSE.md                             the same for people, written from it
        rusty_plates_basecolor.png ...
    library/models/superhero/ ...
    library/music/<album, or miscellaneous>/<asset>/ ...

asset.json: {"format": 1, "name", "type", "category", "description", "licence" ("CC0-1.0",
"CC-BY-4.0" or "CC-BY-3.0"), "author", "source" (where it came from), "title" (the work's own
title, for its credit), "imported" (a date), "files": [{"path", "sha256", "bytes"}]}.

Assets are always copied in, never linked. CC0 assets need nothing more; CC-BY ones need their
author, and their title or source, and are credited automatically (`credit`). Nothing else is
allowed in."""
import hashlib
import json
import re
import shutil
from dataclasses import dataclass, field
from pathlib import Path

FORMAT = 1
FOLDER = "library"
METADATA = "asset.json"
LICENCE_FILE = "LICENSE.md"

# The asset types (folders), their names, and the kinds a type is sorted into.
TEXTURE_KINDS = ("metal", "wood", "stone", "concrete", "bark", "brick", "fabric", "floor",
                 "ground", "leather", "road", "roof", "wall", "miscellaneous")
TYPES: dict[str, tuple[str, tuple[str, ...]]] = {
    "gui": ("GUI", ()),
    "fonts": ("Fonts", ()),
    "textures": ("Textures", TEXTURE_KINDS),
    "images": ("Images", ()),
    "icons": ("Icons", ()),
    "music": ("Music", ()),  # (by album: the category is the album's name, or miscellaneous)
    "ambiance": ("Ambiance", ()),
    "effects": ("FX", ()),
    "models": ("Models", ()),
    "objects": ("Objects", ()),
}
ALBUMS = ("music", "ambiance", "effects")  # types sorted by album
MISCELLANEOUS = "miscellaneous"

# The licences assets may have: (name, its deed, whether it needs crediting).
LICENCES: dict[str, tuple[str, str, bool]] = {
    "CC0-1.0": ("CC0 1.0 (public domain)",
                "https://creativecommons.org/publicdomain/zero/1.0/", False),
    "CC-BY-4.0": ("CC BY 4.0", "https://creativecommons.org/licenses/by/4.0/", True),
    "CC-BY-3.0": ("CC BY 3.0", "https://creativecommons.org/licenses/by/3.0/", True),
}


class LibraryError(Exception):
    pass


@dataclass
class Asset:
    folder: Path  # the asset's own folder
    name: str
    type: str
    category: str = ""
    description: str = ""
    licence: str = "CC0-1.0"
    author: str = ""
    source: str = ""
    title: str = ""
    imported: str = ""
    files: list[dict] = field(default_factory=list)  # {"path", "sha256", "bytes"}

    @property
    def id(self) -> str:
        """Where it is in the library: "textures/metal/rusty_plates" (its folder, from the
        library's)."""
        library = next((p for p in self.folder.parents if p.name == FOLDER), None)
        if library is None:
            return self.folder.name
        return self.folder.relative_to(library).as_posix()

    def file_paths(self) -> list[Path]:
        return [self.folder / f["path"] for f in self.files]

    def to_json(self) -> dict:
        return {"format": FORMAT, "name": self.name, "type": self.type,
                "category": self.category, "description": self.description,
                "licence": self.licence, "author": self.author, "source": self.source,
                "title": self.title, "imported": self.imported, "files": self.files}


def library_dir(project) -> Path:
    return Path(project.root) / FOLDER


def slug(name: str) -> str:
    """A folder name from an asset's name: lower case, words joined by underscores."""
    s = re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")
    return s or "asset"


def type_folder(project, type_: str, category: str = "") -> Path:
    """Where assets of a type (and kind, or album) go."""
    if type_ not in TYPES:
        raise LibraryError(f"there is no asset type '{type_}'")
    kinds = TYPES[type_][1]
    if kinds and category not in kinds:
        raise LibraryError(f"textures are one of: {', '.join(kinds)}")
    folder = library_dir(project) / type_
    if type_ in ALBUMS:
        return folder / slug(category or MISCELLANEOUS)
    return folder / category if category else folder


# ---- reading ---------------------------------------------------------------------------------
def read(folder: Path) -> Asset:
    """An asset from its folder's asset.json; LibraryError if it can't be read."""
    try:
        doc = json.loads((folder / METADATA).read_text("utf-8"))
    except (OSError, ValueError) as e:
        raise LibraryError(f"{folder.name}: {METADATA} can't be read: {e}") from e
    if not isinstance(doc, dict) or doc.get("format") != FORMAT:
        raise LibraryError(f"{folder.name}: {METADATA} isn't one this editor reads")
    fields = {k: doc.get(k, "") for k in ("name", "type", "category", "description", "licence",
                                          "author", "source", "title", "imported")}
    return Asset(folder=folder, files=list(doc.get("files", [])), **fields)


def scan(project) -> tuple[list[Asset], list[str]]:
    """Every asset in the library (by type, then name), and what couldn't be read."""
    root = library_dir(project)
    assets, problems = [], []
    if not root.is_dir():
        return assets, problems
    for meta in sorted(root.rglob(METADATA)):
        try:
            assets.append(read(meta.parent))
        except LibraryError as e:
            problems.append(str(e))
    assets.sort(key=lambda a: (a.type, a.category, a.name.lower()))
    return assets, problems


def sha256(file: Path) -> str:
    h = hashlib.sha256()
    with open(file, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def problems(asset: Asset) -> list[str]:
    """What is wrong with an asset (none: all well): its licence, its credit, its files."""
    out = []
    if asset.type not in TYPES:
        out.append(f"there is no asset type '{asset.type}'")
    if asset.licence not in LICENCES:
        allowed = ", ".join(LICENCES)
        out.append(f"its licence, '{asset.licence or 'none'}', isn't allowed (only {allowed})")
    elif LICENCES[asset.licence][2]:
        if not asset.author.strip():
            out.append("it is CC BY: its author is needed for its credit")
        if not (asset.title.strip() or asset.source.strip()):
            out.append("it is CC BY: its title or source is needed for its credit")
    if not asset.files:
        out.append("it has no files")
    for f in asset.files:
        path = asset.folder / f.get("path", "")
        if not path.is_file():
            out.append(f"{f.get('path')} is missing")
        elif f.get("sha256") and sha256(path) != f["sha256"]:
            out.append(f"{f.get('path')} has changed since it was imported")
    return out


# ---- credits ---------------------------------------------------------------------------------
def needs_credit(asset: Asset) -> bool:
    return asset.licence in LICENCES and LICENCES[asset.licence][2]


def credit(asset: Asset) -> str:
    """Its credit (title, author, source, licence): '"Rusty Plates" by Ana Ruiz
    (https://example.org/rusty), CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/)'."""
    name, deed, _ = LICENCES.get(asset.licence, (asset.licence, "", False))
    title = asset.title or asset.name
    text = f'"{title}" by {asset.author or "an unknown author"}'
    if asset.source:
        text += f" ({asset.source})"
    return f"{text}, {name}" + (f" ({deed})" if deed else "")


def credits(assets: list[Asset]) -> list[str]:
    """The credits CC BY assets need, each once, in order."""
    return sorted({credit(a) for a in assets if needs_credit(a)})


# ---- writing ---------------------------------------------------------------------------------
def licence_text(asset: Asset) -> str:
    """LICENSE.md: the asset's description and licence, for people."""
    name, deed, attribute = LICENCES.get(asset.licence, (asset.licence, "", False))
    lines = [f"# {asset.name}", ""]
    if asset.description:
        lines += [asset.description, ""]
    lines += [f"- Licence: {name}" + (f" ({deed})" if deed else "")]
    if asset.title:
        lines.append(f"- Title: {asset.title}")
    if asset.author:
        lines.append(f"- Author: {asset.author}")
    if asset.source:
        lines.append(f"- Source: {asset.source}")
    if asset.imported:
        lines.append(f"- Imported: {asset.imported}")
    if attribute:
        lines += ["", "Credit (shown in the game's credits):", "", credit(asset)]
    lines += ["", "## Files", ""] + [f"- {f['path']}" for f in asset.files]
    return "\n".join(lines) + "\n"


def write(asset: Asset) -> None:
    """Its asset.json and LICENSE.md."""
    asset.folder.mkdir(parents=True, exist_ok=True)
    (asset.folder / METADATA).write_text(json.dumps(asset.to_json(), indent=2) + "\n", "utf-8")
    (asset.folder / LICENCE_FILE).write_text(licence_text(asset), "utf-8")


def record_files(asset: Asset) -> None:
    """Its files as they are now (every file in its folder but its own two)."""
    asset.files = [{"path": p.relative_to(asset.folder).as_posix(), "sha256": sha256(p),
                    "bytes": p.stat().st_size}
                   for p in sorted(asset.folder.rglob("*"))
                   if p.is_file() and p.name not in (METADATA, LICENCE_FILE)]


def remove(asset: Asset) -> None:
    """The asset's folder deleted (with everything in it), and its empty parents."""
    library = next((p for p in asset.folder.parents if p.name == FOLDER), None)
    shutil.rmtree(asset.folder)
    parent = asset.folder.parent
    while library is not None and parent != library and parent.is_dir() and \
            not any(parent.iterdir()):
        parent.rmdir()
        parent = parent.parent


# ---- importing -------------------------------------------------------------------------------
# The files each type may hold (besides text about them: licences, read-mes).
IMAGES = (".png", ".jpg", ".jpeg", ".ktx2")
AUDIO = (".ogg", ".opus", ".wav", ".mp3", ".flac")
ALLOWED: dict[str, tuple[str, ...]] = {
    "gui": IMAGES,
    "fonts": (".ttf", ".otf", ".fnt", ".png"),
    "textures": IMAGES,
    "images": IMAGES,
    "icons": IMAGES + (".ico",),
    "music": AUDIO,
    "ambiance": AUDIO,
    "effects": AUDIO,
    "models": (".glb", ".gltf", ".bin") + IMAGES,
    "objects": (".glb", ".gltf", ".bin", ".json") + IMAGES,
}
TEXT = (".md", ".txt", ".pdf")  # licences and read-mes, kept with any asset
MAX_IMPORT_BYTES = 2 << 30      # a zip's contents, at most (2 GB)


def _looks_like(path: Path) -> str | None:
    """What is wrong with a file's contents for its extension (None: it looks right)."""
    ext = path.suffix.lower()
    try:
        with open(path, "rb") as f:
            head = f.read(16)
    except OSError as e:
        return f"can't be read: {e}"
    if ext == ".png":
        ok = head.startswith(b"\x89PNG\r\n\x1a\n")
    elif ext in (".jpg", ".jpeg"):
        ok = head.startswith(b"\xff\xd8\xff")
    elif ext == ".ktx2":
        ok = head.startswith(b"\xabKTX 20\xbb")
    elif ext == ".ico":
        ok = head[:4] == b"\x00\x00\x01\x00"
    elif ext == ".glb":
        ok = head.startswith(b"glTF")
    elif ext in (".gltf", ".json"):
        try:
            ok = isinstance(json.loads(path.read_text("utf-8")), dict)
        except (OSError, ValueError):
            ok = False
    elif ext in (".ogg", ".opus"):
        ok = head.startswith(b"OggS")
    elif ext == ".wav":
        ok = head[:4] == b"RIFF" and head[8:12] == b"WAVE"
    elif ext == ".mp3":
        ok = head.startswith(b"ID3") or (len(head) > 1 and head[0] == 0xFF and
                                         head[1] & 0xE0 == 0xE0)
    elif ext == ".flac":
        ok = head.startswith(b"fLaC")
    elif ext in (".ttf", ".otf"):
        ok = head[:4] in (b"\x00\x01\x00\x00", b"OTTO", b"true")
    else:
        ok = True  # (.bin buffers, .fnt descriptions, text: nothing to check)
    return None if ok else f"isn't a {ext} file inside"


def _gathered(sources: list[Path], scratch: Path) -> list[tuple[Path, str]]:
    """The files to import, each with its path in the asset: loose files by name, a folder's
    files as they are under it, a zip's (opened into `scratch`, never outside it) likewise."""
    import zipfile
    out: list[tuple[Path, str]] = []
    for source in sources:
        source = Path(source)
        if source.is_dir():
            for p in sorted(source.rglob("*")):
                if p.is_file():
                    out.append((p, p.relative_to(source).as_posix()))
        elif source.suffix.lower() == ".zip":
            target = scratch / slug(source.stem)
            unpacked: list[tuple[Path, str]] = []
            with zipfile.ZipFile(source) as z:
                total = 0
                for info in z.infolist():
                    name = info.filename
                    if info.is_dir() or name.startswith("__MACOSX/") or \
                            Path(name).name.startswith("._"):
                        continue
                    parts = Path(name).parts
                    if name.startswith(("/", "\\")) or ".." in parts or ":" in name:
                        raise LibraryError(f"{source.name}: '{name}' would land outside the "
                                           "library (refused)")
                    total += info.file_size
                    if total > MAX_IMPORT_BYTES:
                        raise LibraryError(f"{source.name} is larger than 2 GB unpacked")
                    dest = target / Path(*parts)
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    with z.open(info) as src, open(dest, "wb") as out_file:
                        shutil.copyfileobj(src, out_file)
                    unpacked.append((dest, Path(*parts).as_posix()))
            # A single top folder (as zips often have): its contents, without it.
            tops = {rel.split("/", 1)[0] for _p, rel in unpacked}
            if len(tops) == 1 and all("/" in rel for _p, rel in unpacked):
                unpacked = [(p, rel.split("/", 1)[1]) for p, rel in unpacked]
            out += unpacked
        elif source.is_file():
            out.append((source, source.name))
        else:
            raise LibraryError(f"{source} isn't there")
    return out


def find_duplicate(project, file: Path) -> Asset | None:
    """An asset already holding a file with these contents."""
    digest = sha256(file)
    assets, _ = scan(project)
    for a in assets:
        if any(f.get("sha256") == digest for f in a.files):
            return a
    return None


def import_asset(project, sources: list[Path], type_: str, name: str, *, category: str = "",
                 description: str = "", licence: str = "CC0-1.0", author: str = "",
                 source: str = "", title: str = "", today: str = "") -> Asset:
    """An asset copied into the library from files, a folder or a zip (never linked). Every
    file must suit the type (and look like what its name says); text about it (a licence, a
    read-me) is kept too. LibraryError says why it was refused: nothing is left behind then."""
    import datetime
    import tempfile
    if type_ not in TYPES:
        raise LibraryError(f"there is no asset type '{type_}'")
    name = name.strip()
    if not name:
        raise LibraryError("an asset needs a name")
    allowed = ALLOWED[type_]
    work = Path(project.root) / ".midgard"
    work.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=work, prefix="import_") as scratch:
        files = _gathered([Path(s) for s in sources], Path(scratch))
        if not files:
            raise LibraryError("there is nothing to import")
        content = [(p, rel) for p, rel in files if p.suffix.lower() not in TEXT]
        if not content:
            raise LibraryError("there are only text files: nothing to use")
        wrong = sorted(rel for p, rel in content if p.suffix.lower() not in allowed)
        if wrong:
            raise LibraryError(f"{TYPES[type_][0].lower()} can't hold "
                               f"{', '.join(wrong[:5])}" + (" ..." if len(wrong) > 5 else "") +
                               f" (only {', '.join(allowed)})")
        for p, rel in content:
            if (why := _looks_like(p)) is not None:
                raise LibraryError(f"{rel} {why}")
        main = content[0][0]
        if (twin := find_duplicate(project, main)) is not None:
            raise LibraryError(f"{content[0][1]} is in the library already, as "
                               f"'{twin.name}' ({twin.id})")
        # Its own folder (never over another's).
        parent = type_folder(project, type_, category)
        folder = parent / slug(name)
        n = 2
        while folder.exists():
            folder = parent / f"{slug(name)}_{n}"
            n += 1
        asset = Asset(folder=folder, name=name, type=type_, category=category,
                      description=description.strip(), licence=licence, author=author.strip(),
                      source=source.strip(), title=title.strip(),
                      imported=today or datetime.date.today().isoformat())
        try:
            for p, rel in files:
                dest = folder / rel
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(p, dest)
            record_files(asset)
            said = problems(asset)
            if said:
                raise LibraryError("; ".join(said))
            write(asset)
        except BaseException:
            if folder.exists():
                remove(asset)
            raise
    return asset
