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

asset.json follows the library format shared with the Master Asset Library (milestone 5.13b;
its docs/library_format.md): {"format": 2, "id" (the asset's own id, the same in the Master and
every copy), "version", "name", "type", "category", "description", "tags", "licence",
"author", "source" (where it came from), "title" (the work's own title, for its credit),
"imported" (a date), "files": [{"path", "sha256", "bytes"}], "fingerprint" (its files as one
SHA-256), "purchase" (where a purchased one was bought), "origin" (where a copy from the Master
came from)}. Format 1 (5.13a's) is read too, and written as 2 when it changes.

Assets are always copied in, never linked. CC0 assets need nothing more; CC BY and CC BY-SA ones
need their author, and their title or source, and are credited automatically (`credit`).
Purchased ones (from the Master) need their seller and the seller's licence, kept with them;
their original files are never given away."""
import hashlib
import json
import re
import shutil
import uuid
from dataclasses import dataclass, field
from pathlib import Path

FORMAT = 2
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
    "CC-BY-SA-4.0": ("CC BY-SA 4.0", "https://creativecommons.org/licenses/by-sa/4.0/", True),
    "CC-BY-SA-3.0": ("CC BY-SA 3.0", "https://creativecommons.org/licenses/by-sa/3.0/", True),
    "Purchased": ("Purchased", "", False),
    "Own-Work": ("Own work (all rights reserved)", "", False),
}
PURCHASED = "Purchased"
OWN_WORK = "Own-Work"  # your own, all rights reserved: your games use it, no one else
SHARE_ALIKE = ("CC-BY-SA-4.0", "CC-BY-SA-3.0")
# The licences an asset is imported with here (purchased ones come from the Master, with where
# they were bought).
IMPORT_LICENCES = ("CC0-1.0", "CC-BY-4.0", "CC-BY-3.0", "CC-BY-SA-4.0", "CC-BY-SA-3.0",
                   "Own-Work")
# A format 1 asset's id: made from its path, so it is the same every time (as the Master's).
_V1_NAMESPACE = uuid.UUID("6a1d4f0e-5b8c-4e2a-9f37-0c1d2e3f4a5b")


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
    uid: str = ""         # its own id ("id" in asset.json; Asset.id is where it is)
    version: int = 1      # one more each time its files change
    tags: list[str] = field(default_factory=list)
    fingerprint: str = ""  # its files as one SHA-256
    purchase: dict | None = None  # where a purchased one was bought
    origin: dict | None = None    # a copy from the Master: which, and which version
    # From its import (not saved): files left out, and what to know.
    skipped: list[tuple[str, str]] = field(default_factory=list, compare=False, repr=False)
    warnings: list[str] = field(default_factory=list, compare=False, repr=False)

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
        return {"format": FORMAT, "id": self.uid, "version": self.version, "name": self.name,
                "type": self.type, "category": self.category, "description": self.description,
                "tags": self.tags, "licence": self.licence, "author": self.author,
                "source": self.source, "title": self.title, "imported": self.imported,
                "files": self.files, "fingerprint": self.fingerprint or fingerprint(self.files),
                "purchase": self.purchase, "origin": self.origin}


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
    if not isinstance(doc, dict) or doc.get("format") not in (1, 2):
        raise LibraryError(f"{folder.name}: {METADATA} isn't one this editor reads")
    fields = {k: doc.get(k) or "" for k in ("name", "type", "category", "description",
                                            "licence", "author", "source", "title", "imported")}
    files = [f for f in doc.get("files", []) if isinstance(f, dict) and "path" in f]
    asset = Asset(folder=folder, files=files, **fields)
    asset.uid = str(doc.get("id") or "") if doc["format"] == 2 else ""
    if not asset.uid:
        asset.uid = str(uuid.uuid5(_V1_NAMESPACE, asset.id))
    asset.version = int(doc.get("version") or 1)
    asset.tags = [str(t) for t in doc.get("tags") or []]
    asset.fingerprint = str(doc.get("fingerprint") or fingerprint(files))
    asset.purchase = doc.get("purchase") if isinstance(doc.get("purchase"), dict) else None
    asset.origin = doc.get("origin") if isinstance(doc.get("origin"), dict) else None
    return asset


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


def fingerprint(files: list[dict]) -> str:
    """An asset's files as one SHA-256: a line "<path>\t<sha256>\n" per file, sorted by path
    (the library format's)."""
    lines = sorted((f["path"], f.get("sha256", "")) for f in files)
    return hashlib.sha256("".join(f"{p}\t{d}\n" for p, d in lines).encode()).hexdigest()


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
    elif needs_credit(asset):
        if not asset.author.strip():
            out.append("it needs crediting: its author is needed for its credit")
        if not (asset.title.strip() or asset.source.strip()):
            out.append("it needs crediting: its title or source is needed for its credit")
    if asset.licence == OWN_WORK and not asset.author.strip():
        out.append("it is your own work: its artist (author) is needed")
    if asset.licence == PURCHASED:
        p = asset.purchase or {}
        if not str(p.get("seller", "")).strip():
            out.append("it was bought: who sold it is needed")
        licence_file = str(p.get("licence_file", ""))
        if not licence_file:
            out.append("it was bought: the seller's licence is needed, kept with it")
        elif not any(f.get("path") == licence_file for f in asset.files):
            out.append(f"its licence file, {licence_file}, isn't one of its files")
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
    if asset.licence == PURCHASED:
        return bool((asset.purchase or {}).get("credit"))
    return asset.licence in LICENCES and LICENCES[asset.licence][2]


def share_alike(asset: Asset) -> bool:
    return asset.licence in SHARE_ALIKE


def may_give_away(asset: Asset) -> bool:
    """Whether its original files may be passed on (a purchased one's, or your own all rights
    reserved work's, may not)."""
    return asset.licence in LICENCES and asset.licence not in (PURCHASED, OWN_WORK)


def credit(asset: Asset) -> str:
    """Its credit (title, author, source, licence): '"Rusty Plates" by Ana Ruiz
    (https://example.org/rusty), CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/)'."""
    name, deed, _ = LICENCES.get(asset.licence, (asset.licence, "", False))
    if asset.licence == PURCHASED:
        seller = (asset.purchase or {}).get("seller")
        name = f"from {seller}" if seller else "purchased"
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
    if asset.tags:
        lines.append(f"- Tags: {', '.join(asset.tags)}")
    if asset.licence == PURCHASED and asset.purchase:
        lines += ["", "Bought:", ""]
        for label, key in (("Seller", "seller"), ("Product", "product"), ("Order", "order"),
                           ("Date", "date"), ("The seller's licence", "licence_file")):
            if asset.purchase.get(key):
                lines.append(f"- {label}: {asset.purchase[key]}")
        lines += ["", "Its original files may not be given away."]
    if share_alike(asset):
        lines += ["", "Share-alike: changed versions of it must be offered under the same "
                  "licence."]
    if needs_credit(asset):
        lines += ["", "Credit (shown in the game's credits):", "", credit(asset)]
    lines += ["", "## Files", ""] + [f"- {f['path']}" for f in asset.files]
    return "\n".join(lines) + "\n"


def write(asset: Asset) -> None:
    """Its asset.json and LICENSE.md (as format 2: an id is made if it has none)."""
    if not asset.uid:
        asset.uid = str(uuid.uuid4())
    asset.folder.mkdir(parents=True, exist_ok=True)
    (asset.folder / METADATA).write_text(json.dumps(asset.to_json(), indent=2) + "\n", "utf-8")
    (asset.folder / LICENCE_FILE).write_text(licence_text(asset), "utf-8")


def record_files(asset: Asset) -> bool:
    """Its files as they are now (every file in its folder but its own two), and its
    fingerprint. Returns whether they changed (its version goes up when they did)."""
    before = asset.fingerprint
    asset.files = [{"path": p.relative_to(asset.folder).as_posix(), "sha256": sha256(p),
                    "bytes": p.stat().st_size}
                   for p in sorted(asset.folder.rglob("*"))
                   if p.is_file() and p.relative_to(asset.folder).as_posix() not in
                   (METADATA, LICENCE_FILE)]
    asset.fingerprint = fingerprint(asset.files)
    changed = bool(before) and before != asset.fingerprint
    if changed:
        asset.version += 1
    return changed


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


@dataclass
class ImportResult:
    """What an import made, and what it left out."""
    assets: list[Asset] = field(default_factory=list)
    skipped: list[tuple[str, str]] = field(default_factory=list)  # (file, why)
    warnings: list[str] = field(default_factory=list)
    refused: list[tuple[str, str]] = field(default_factory=list)  # (an asset's name, why)

    def summary(self) -> str:
        """'Imported 2 assets; skipped 14 files: 9 .fbx, 4 .blend, 1 .exe.'"""
        made = f"Imported {len(self.assets)} asset{'' if len(self.assets) == 1 else 's'}"
        parts = [made]
        if self.skipped:
            parts.append(skipped_summary(self.skipped))
        for name, why in self.refused:
            parts.append(f"{name} not imported: {why}")
        return "; ".join(parts) + "."


def skipped_summary(skipped: list[tuple[str, str]]) -> str:
    """'skipped 14 files: 9 .fbx, 4 .blend, 1 .exe' (by ending, most first)."""
    counts: dict[str, int] = {}
    for rel, _why in skipped:
        ext = Path(rel).suffix.lower() or "(no ending)"
        counts[ext] = counts.get(ext, 0) + 1
    listed = ", ".join(f"{n} {ext}" for ext, n in sorted(counts.items(),
                                                          key=lambda kv: (-kv[1], kv[0])))
    return f"skipped {len(skipped)} file{'' if len(skipped) == 1 else 's'}: {listed}"


def _sorted(files: list[tuple[Path, str]], type_: str) -> tuple[list[tuple[Path, str]],
                                                                list[tuple[str, str]]]:
    """The files an asset of the type keeps (text about it always), and those it leaves out,
    each with why: a type it can't hold, or contents that aren't what the name says."""
    allowed = ALLOWED[type_]
    kept, skipped = [], []
    for p, rel in files:
        ext = p.suffix.lower()
        if ext in TEXT:
            kept.append((p, rel))
        elif ext not in allowed:
            skipped.append((rel, f"{TYPES[type_][0].lower()} can't use {ext or 'it'}"))
        elif (why := _looks_like(p)) is not None:
            skipped.append((rel, why))
        else:
            kept.append((p, rel))
    return kept, skipped


def _make(project, files: list[tuple[Path, str]], type_: str, name: str, *, category: str,
          description: str, licence: str, author: str, source: str, title: str,
          today: str) -> Asset:
    """An asset from files already sorted (their content first). LibraryError if it can't
    be made (nothing usable, already in the library, its licence): nothing is left behind."""
    import datetime
    content = [(p, rel) for p, rel in files if p.suffix.lower() not in TEXT]
    if not content:
        raise LibraryError("there is nothing usable (only text, or files it can't use)")
    if (twin := find_duplicate(project, content[0][0])) is not None:
        raise LibraryError(f"{content[0][1]} is in the library already, as "
                           f"'{twin.name}' ({twin.id})")
    parent = type_folder(project, type_, category)
    folder = parent / slug(name)
    n = 2
    while folder.exists():  # (never over another's)
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
    # A .gltf naming files that aren't with it (left out, or never there).
    from urllib.parse import unquote

    from .objects import gltf_dependencies
    for gltf in folder.rglob("*.gltf"):
        for uri in gltf_dependencies(gltf):
            if not (gltf.parent / unquote(uri)).is_file():
                asset.warnings.append(f"{gltf.relative_to(folder).as_posix()} names "
                                      f"{unquote(uri)}, which isn't with it")
    return asset


def import_asset(project, sources: list[Path], type_: str, name: str, *, category: str = "",
                 description: str = "", licence: str = "CC0-1.0", author: str = "",
                 source: str = "", title: str = "", today: str = "",
                 roles: dict[str, str] | None = None) -> Asset:
    """An asset copied into the library from files, a folder or a zip (never linked). Files it
    can't use (another type, or not what their name says) are left out (asset.skipped says
    which); text about it (a licence, a read-me) is kept. LibraryError says why nothing could
    be imported: nothing is left behind then."""
    result = import_assets(project, sources, type_, name, category=category,
                           description=description, licence=licence, author=author,
                           source=source, title=title, today=today, split=False, roles=roles)
    if not result.assets:
        why = result.refused[0][1] if result.refused else "there is nothing to import"
        if result.skipped:
            why += f" ({skipped_summary(result.skipped)})"
        raise LibraryError(why)
    asset = result.assets[0]
    asset.skipped = result.skipped
    return asset


def _model_groups(kept: list[tuple[Path, str]]) -> list[tuple[str, list[tuple[Path, str]]]]:
    """The models among the files, each with what goes with it: the files a .gltf names, the
    pictures named after it (or all of its folder's, when the folder has no other model), and
    the text about them all. Files with no one model are left over (the last group, "")."""
    from urllib.parse import unquote

    from .objects import gltf_dependencies
    text = [(p, rel) for p, rel in kept if p.suffix.lower() in TEXT]
    models = [(p, rel) for p, rel in kept if p.suffix.lower() in MODEL_SUFFIXES]
    taken: set[str] = set()
    groups = []
    for p, rel in models:
        folder = rel.rsplit("/", 1)[0] if "/" in rel else ""
        mine = [(p, rel)]
        if p.suffix.lower() == ".gltf":
            for uri in gltf_dependencies(p):
                dep = (folder + "/" if folder else "") + unquote(uri)
                mine += [(q, r) for q, r in kept if r == dep]
        alone = sum(1 for _q, r in models
                    if (r.rsplit("/", 1)[0] if "/" in r else "") == folder) == 1
        for q, r in kept:
            r_folder = r.rsplit("/", 1)[0] if "/" in r else ""
            if q.suffix.lower() in IMAGES and r_folder == folder and \
                    (alone or q.stem.lower().startswith(p.stem.lower())) and \
                    all(r != m for _m, m in mine):
                mine.append((q, r))
        taken |= {r for _q, r in mine}
        groups.append((p.stem, mine + text))
    left = [(q, r) for q, r in kept if r not in taken and q.suffix.lower() not in TEXT]
    if left:
        groups.append(("", left))
    return groups


def model_count(sources: list[Path]) -> int:
    """How many models (.glb, .gltf) files, folders and zips hold (to ask about splitting)."""
    import zipfile
    n = 0
    for s in (Path(s) for s in sources):
        if s.is_dir():
            n += sum(1 for p in s.rglob("*") if p.suffix.lower() in MODEL_SUFFIXES)
        elif s.suffix.lower() == ".zip":
            try:
                with zipfile.ZipFile(s) as z:
                    n += sum(1 for name in z.namelist()
                             if Path(name).suffix.lower() in MODEL_SUFFIXES and
                             not name.startswith("__MACOSX/"))
            except (OSError, zipfile.BadZipFile):
                pass
        elif s.suffix.lower() in MODEL_SUFFIXES:
            n += 1
    return n


def pretty(stem: str) -> str:
    """'oak_tree-01' -> 'Oak tree 01'."""
    words = re.sub(r"[_\-]+", " ", stem).strip()
    return words[:1].upper() + words[1:] if words else stem


def import_assets(project, sources: list[Path], type_: str, name: str, *, category: str = "",
                  description: str = "", licence: str = "CC0-1.0", author: str = "",
                  source: str = "", title: str = "", today: str = "",
                  split: bool = False, roles: dict[str, str] | None = None) -> ImportResult:
    """Files, folders and zips into the library: one asset, or (split, for models) one for each
    model with what goes with it and the pack's text. Files it can't use are skipped, and why;
    an asset that can't be made is refused (and why) without stopping the rest. `roles` (for
    textures: each picture's path, as images_in gives them, to its role) renames the pictures
    <name>_<role> and leaves out those not used."""
    import tempfile
    if type_ not in TYPES:
        raise LibraryError(f"there is no asset type '{type_}'")
    name = name.strip()
    if not name:
        raise LibraryError("an asset needs a name")
    result = ImportResult()
    work = Path(project.root) / ".midgard"
    work.mkdir(parents=True, exist_ok=True)
    meta = {"category": category, "description": description, "licence": licence,
            "author": author, "source": source, "title": title, "today": today}
    with tempfile.TemporaryDirectory(dir=work, prefix="import_") as scratch:
        files = _gathered([Path(s) for s in sources], Path(scratch))
        if not files:
            raise LibraryError("there is nothing to import")
        kept, result.skipped = _sorted(files, type_)
        if roles is not None and type_ == "textures":
            kept, unused = _named_by_role(kept, roles, name, Path(scratch))
            result.skipped += unused
        groups = [(name, kept)]
        if split and type_ in ("models", "objects"):
            found = _model_groups(kept)
            if sum(1 for stem, _g in found if stem) > 1:
                groups = [(f"{name}: {pretty(stem)}", g) for stem, g in found if stem]
                leftover = next((g for stem, g in found if not stem), [])
                result.skipped += [(r, "with no one model") for _q, r in leftover]
        for asset_name, group in groups:
            try:
                asset = _make(project, group, type_, asset_name, **meta)
            except LibraryError as e:
                result.refused.append((asset_name, str(e)))
                continue
            result.assets.append(asset)
            result.warnings += [f"{asset.name}: {w}" for w in asset.warnings]
    return result


# ---- using assets (step 5) -------------------------------------------------------------------
MODEL_PREFIX = "lib:"  # a model in the library, by its path there: "lib:models/hero/hero.glb"
MODEL_SUFFIXES = (".glb", ".gltf")


def model_id(project, file: Path) -> str:
    """A library model file's content id."""
    return MODEL_PREFIX + Path(file).resolve().relative_to(
        library_dir(project).resolve()).as_posix()


def model_file(project, asset_id: str) -> Path | None:
    """The file a "lib:" content id names (None: not one, or it isn't there)."""
    if not asset_id.startswith(MODEL_PREFIX):
        return None
    relative = asset_id[len(MODEL_PREFIX):]
    file = (library_dir(project) / relative).resolve()
    if not file.is_relative_to(library_dir(project).resolve()) or not file.is_file():
        return None  # (never outside the library)
    return file


def files_of(project, types: tuple[str, ...],
             suffixes: tuple[str, ...]) -> list[tuple[Asset, Path]]:
    """The library's files of some types with some endings, each with its asset."""
    assets, _ = scan(project)
    return [(a, p) for a in assets if a.type in types for p in a.file_paths()
            if p.suffix.lower() in suffixes and p.is_file()]


def models(project) -> dict[str, str]:
    """The library's models (and objects'), content id -> label."""
    return {model_id(project, p): f"{a.name} ({p.name})"
            for a, p in files_of(project, ("models", "objects"), MODEL_SUFFIXES)}


# ---- credits in the game (step 6) --------------------------------------------------------------
CREDITS_FILE = "CREDITS.txt"


def asset_of(project, file: Path, assets: list[Asset] | None = None) -> Asset | None:
    """The library asset a file belongs to (None: it isn't in the library)."""
    file = Path(file).resolve()
    if assets is None:
        assets, _ = scan(project)
    for a in assets:
        if file.is_relative_to(a.folder.resolve()):
            return a
    return None


def used_by(project, content_ids) -> list[Asset]:
    """The library assets a game's content uses: models ("lib:"), images ("tex:" paths inside
    library/) and sounds ("snd:" paths inside library/), each once, by name."""
    from . import sounds, textures
    assets, _ = scan(project)
    found: dict[str, Asset] = {}
    for content in content_ids:
        file = model_file(project, content) if content.startswith(MODEL_PREFIX) else None
        if file is None and (parsed := textures.parse(content)) is not None:
            file = Path(project.root) / parsed[0]
        if file is None and (sound := sounds.parse(content)) is not None:
            file = Path(project.root) / sound
        if file is not None and (asset := asset_of(project, file, assets)) is not None:
            found[asset.id] = asset
    return sorted(found.values(), key=lambda a: a.name.lower())


def credits_text(game_title: str, used: list[Asset]) -> str:
    """CREDITS.txt: the credits CC BY assets need, and thanks for the CC0 ones."""
    lines = [f"{game_title}: credits", ""]
    needed = credits(used)
    if needed:
        lines += ["This game uses these works under Creative Commons Attribution licences:", ""]
        lines += [f"- {c}" for c in needed] + [""]
    free = sorted({f"{a.title or a.name}" + (f" by {a.author}" if a.author else "")
                   for a in used if not needs_credit(a)})
    if free:
        lines += ["And these, given to the public domain (CC0), with thanks:", ""]
        lines += [f"- {c}" for c in free] + [""]
    return "\n".join(lines)


# ---- sample assets (step 7) ----------------------------------------------------------------------
# The CC0 assets that come with Midgard (assets/third_party, listed with their sources in its
# SOURCES.toml), as library assets: (type, name, description, files from that folder).
SAMPLES = (
    ("models", "Superhero",
     "A humanoid character (65 joints) to animate with the Universal Animation Library's clips.",
     ("characters/quaternius/superhero_male.glb",
      "characters/quaternius/superhero_male_basecolor.png")),
    ("models", "Universal Animation Library",
     "43 humanoid clips (idle, walk, run, jump, sit, swim...) on a mannequin, for characters "
     "with the same joints.",
     ("characters/quaternius/animation_library.glb",)),
    ("models", "Avocado",
     "A small textured model (base colour, normal and roughness/metallic maps).",
     ("models/avocado/Avocado.gltf", "models/avocado/Avocado.bin",
      "models/avocado/Avocado_baseColor.png", "models/avocado/Avocado_normal.png",
      "models/avocado/Avocado_roughnessMetallic.png")),
)


def samples_dir() -> Path:
    """Where Midgard's own CC0 assets are (beside the editor, or in the repository)."""
    from . import BUNDLE_DIR
    return Path(BUNDLE_DIR) / "assets" / "third_party"


def sample_source(path: str) -> dict:
    """SOURCES.toml's entry for a file there (title, author, source, licence)."""
    import tomllib
    try:
        doc = tomllib.loads((samples_dir() / "SOURCES.toml").read_text("utf-8"))
    except (OSError, ValueError):
        return {}
    for entry in doc.get("asset", []):
        listed = entry.get("path", "")
        if path == listed or (listed.endswith("/") and path.startswith(listed)):
            return entry
    return {}


def add_samples(project) -> tuple[list[Asset], list[str]]:
    """Midgard's sample assets copied into the project's library (those it hasn't already).
    Returns the assets added, and what was left out (with why)."""
    added, left = [], []
    for type_, name, description, files in SAMPLES:
        paths = [samples_dir() / f for f in files]
        if not all(p.is_file() for p in paths):
            left.append(f"{name}: not with this editor")
            continue
        source = sample_source(files[0])
        try:
            added.append(import_asset(
                project, paths, type_, name, description=description,
                licence=source.get("license", ""), author=source.get("author", ""),
                source=source.get("source", ""), title=source.get("title", "")))
        except LibraryError as e:
            left.append(f"{name}: {e}")
    return added, left


# ---- texture sets: each picture's role (FixMe) ---------------------------------------------------
# The roles a texture asset's pictures can have (what Midgard's materials use), and the material
# slot each fills. "" is "not used": the picture is left out.
TEXTURE_ROLES: dict[str, str] = {
    "color": "Colour",
    "normal": "Normal",
    "roughness": "Roughness",
    "metallic": "Metallic",
    "metallic_roughness": "Metallic & roughness (combined)",
    "occlusion": "Ambient occlusion",
    "emissive": "Emissive",
    "": "Not used (left out)",
}
ROLE_SLOTS = {"color": "base_color", "normal": "normal",
              "metallic_roughness": "metallic_roughness", "occlusion": "occlusion",
              "emissive": "emissive"}
# Words in file names that say a role (the common packs' namings: ambientCG, Poly Haven...).
_ROLE_WORDS = (
    # (A "combined" map: occlusion in red, roughness in green, metallic in blue: glTF reads its
    # green and blue as its metallic & roughness.)
    ("metallic_roughness", ("metallicroughness", "roughnessmetallic", "orm", "arm", "mr",
                            "combined")),
    ("color", ("color", "colour", "basecolor", "basecolour", "albedo", "diffuse", "diff",
               "col", "base", "basecol")),
    ("normal", ("normal", "normalgl", "normaldx", "nor", "norm", "nrm", "nor_gl", "normal_gl")),
    ("roughness", ("roughness", "rough", "rgh")),
    ("metallic", ("metallic", "metalness", "metal", "met")),
    ("occlusion", ("ambientocclusion", "occlusion", "ao")),
    ("emissive", ("emissive", "emission", "emit", "glow")),
)


def guess_role(filename: str) -> str:
    """A picture's role from its name ("rock_Color.png": "color"; "" when it doesn't say, or
    it is a kind Midgard doesn't use, like a height map)."""
    words = [w for w in re.split(r"[^a-z0-9]+", Path(filename).stem.lower()) if w]
    joined = "".join(words)
    # The role word is usually last ("Metal_Floor_001_Emissive" is emissive, not metallic): the
    # words are read from the end.
    for word in reversed(words):
        for role, names in _ROLE_WORDS:
            if word in names:
                return role
    for role, names in _ROLE_WORDS:
        if any(joined.endswith(n) for n in names if len(n) > 4):
            return role
    return ""


def images_in(sources: list[Path]) -> list[str]:
    """The pictures files, folders and zips hold, by their path in an asset (as an import would
    put them), to choose their roles before importing."""
    import tempfile
    with tempfile.TemporaryDirectory(prefix="midgard_images_") as scratch:
        return [rel for p, rel in _gathered([Path(s) for s in sources], Path(scratch))
                if p.suffix.lower() in IMAGES]


def _combined(roughness: Path | None, metallic: Path | None, out: Path) -> Path | None:
    """The combined picture the engine reads (glTF's: roughness in green, metallic in blue),
    from separate roughness and metallic maps (a missing one: rough, not metal)."""
    from PySide6.QtGui import QImage
    maps = {k: QImage(str(p)) for k, p in (("r", roughness), ("m", metallic)) if p is not None}
    if not maps or any(img.isNull() for img in maps.values()):
        return None
    size = next(iter(maps.values())).size()
    w, h = size.width(), size.height()

    def grey(img: QImage | None, fill: int) -> bytes:  # one byte a pixel, rows unpadded
        if img is None:
            return bytes([fill]) * (w * h)
        g = img.scaled(size).convertToFormat(QImage.Format.Format_Grayscale8)
        data, stride = bytes(g.constBits()), g.bytesPerLine()
        return b"".join(data[y * stride:y * stride + w] for y in range(h))

    pixels = bytearray(w * h * 4)  # (RGB32 in memory: blue, green, red, alpha)
    pixels[0::4] = grey(maps.get("m"), 0)
    pixels[1::4] = grey(maps.get("r"), 255)
    pixels[3::4] = bytes([255]) * (w * h)
    image = QImage(bytes(pixels), w, h, w * 4, QImage.Format.Format_RGB32)
    return out if image.save(str(out)) else None


def _named_by_role(files: list[tuple[Path, str]], roles: dict[str, str], name: str,
                   scratch: Path) -> tuple[list[tuple[Path, str]], list[tuple[str, str]]]:
    """A texture set's files renamed by role (<name>_<role>.<ext>), pictures "not used" left
    out, and the combined metallic & roughness made from separate maps. LibraryError if two
    pictures have the same role."""
    base = slug(name)
    out, skipped, by_role = [], [], {}
    for p, rel in files:
        if p.suffix.lower() not in IMAGES:
            out.append((p, rel))  # (text about it, kept as it is)
            continue
        role = roles.get(rel, "")
        if not role:
            skipped.append((rel, "not used"))
            continue
        if role not in TEXTURE_ROLES:
            raise LibraryError(f"'{role}' isn't a role a picture can have")
        if role in by_role:
            raise LibraryError(f"{by_role[role][1]} and {rel} are both "
                               f"{TEXTURE_ROLES[role].lower()}: one picture each")
        by_role[role] = (p, rel)
        out.append((p, f"{base}_{role}{p.suffix.lower()}"))
    if ("roughness" in by_role or "metallic" in by_role) and "metallic_roughness" not in by_role:
        made = _combined(by_role.get("roughness", (None,))[0],
                         by_role.get("metallic", (None,))[0],
                         scratch / f"{base}_metallic_roughness.png")
        if made is not None:
            out.append((made, made.name))
    return out, skipped


def texture_set(asset: Asset) -> dict[str, Path]:
    """A texture asset's pictures by role: from their names (<name>_<role>), or guessed from
    them; a lone picture is its colour."""
    pictures = [p for p in asset.file_paths() if p.suffix.lower() in IMAGES and p.is_file()]
    found: dict[str, Path] = {}
    for p in pictures:
        stem = p.stem.lower()
        named = sorted((r for r in TEXTURE_ROLES if r), key=len, reverse=True)  # (longest first)
        role = next((r for r in named if stem.endswith("_" + r)), "") or guess_role(p.name)
        if role and role not in found:
            found[role] = p
    if not found and len(pictures) == 1:
        found["color"] = pictures[0]
    return found


def material_textures(project, asset: Asset) -> dict[str, str]:
    """A texture set as a material's texture slots ("tex:" ids, used where they are)."""
    from . import textures
    root = Path(project.root).resolve()
    slots = {}
    for role, path in texture_set(asset).items():
        slot = ROLE_SLOTS.get(role)
        if slot is not None:
            slots[slot] = textures.ref(path.resolve().relative_to(root).as_posix(), slot)
    return slots
