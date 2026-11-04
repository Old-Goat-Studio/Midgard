# SPDX-License-Identifier: Apache-2.0
"""The Master Asset Library (milestone 5.13b): one library of every asset you have, kept by its
own program, that a project's Asset Library takes copies from (Assets ▸ Add from Master…) and
gives new ones back to (Send to Master). Both follow the same library format, so assets move
between them as plain folders, always copied: a project stays complete by itself.

Where the Master is: Edit ▸ Preferences ▸ Master library, or else where the Master Asset Library
program keeps it (its own settings), or else Documents/Master Asset Library."""
import datetime
import json
import shutil
from dataclasses import dataclass
from pathlib import Path

from PySide6.QtCore import QStandardPaths

from . import asset_library as lib

LIBRARY_FILE = "library.json"


class MasterError(Exception):
    """Shown to the user."""


@dataclass
class Master:
    root: Path
    id: str
    name: str = "Master Asset Library"

    def scan(self) -> tuple[list[lib.Asset], list[str]]:
        """Its assets (read as a project's are: the same format), and what couldn't be read."""
        assets, problems = [], []
        for meta in sorted(self.root.rglob(lib.METADATA)):
            try:
                asset = lib.read(meta.parent)
            except lib.LibraryError as e:
                problems.append(str(e))
                continue
            assets.append(asset)
        assets.sort(key=lambda a: (a.type, a.category, a.name.lower()))
        return assets, problems

    def path_of(self, asset: lib.Asset) -> str:
        """An asset's path in the Master: "textures/metal/rusty_plates"."""
        return asset.folder.relative_to(self.root).as_posix()


def _documents() -> Path:
    found = QStandardPaths.writableLocation(QStandardPaths.StandardLocation.DocumentsLocation)
    return Path(found or Path.home())


def program_setting() -> Path | None:
    """Where the Master Asset Library program keeps its library (its settings), if it says."""
    # (Beside Midgard's own settings folder: %APPDATA%/Master Asset Library on Windows,
    # ~/.local/share/Master Asset Library on Linux.)
    ours = QStandardPaths.writableLocation(QStandardPaths.StandardLocation.AppDataLocation)
    if not ours:
        return None
    try:
        doc = json.loads((Path(ours).parent / "Master Asset Library" / "master_library.json")
                         .read_text("utf-8"))
    except (OSError, ValueError):
        return None
    if isinstance(doc, dict) and doc.get("library_dir"):
        return Path(doc["library_dir"])
    return None


def master_dir(settings) -> Path:
    """Where the Master is (it may not be made yet)."""
    chosen = settings.get("master_library_dir") if settings is not None else ""
    if chosen:
        return Path(chosen)
    return program_setting() or _documents() / "Master Asset Library"


def open_master(folder: Path) -> Master:
    """The Master library in a folder; MasterError if there is none there."""
    try:
        doc = json.loads((Path(folder) / LIBRARY_FILE).read_text("utf-8"))
    except (OSError, ValueError) as e:
        raise MasterError(f"There is no Master Asset Library in {folder} (open the Master Asset "
                          "Library program once to make it, or choose its folder in "
                          "Edit ▸ Preferences).") from e
    if not isinstance(doc, dict) or doc.get("kind") != "master" or not doc.get("id"):
        raise MasterError(f"{folder} isn't a Master Asset Library")
    return Master(Path(folder), str(doc["id"]), str(doc.get("name") or "Master Asset Library"))


def find_master(settings) -> Master:
    return open_master(master_dir(settings))


def _today() -> str:
    return datetime.date.today().isoformat()


def _free(folder: Path) -> Path:
    if not folder.exists():
        return folder
    n = 2
    while (candidate := folder.with_name(f"{folder.name}_{n}")).exists():
        n += 1
    return candidate


# ---- from the Master into a project ------------------------------------------------------------
def in_project(project, master_asset: lib.Asset) -> lib.Asset | None:
    """The project's copy of a Master asset (the same id), if it has one."""
    assets, _ = lib.scan(project)
    return next((a for a in assets if a.uid == master_asset.uid), None)


def add_to_project(project, master: Master, asset: lib.Asset) -> lib.Asset:
    """A copy of a Master asset in the project's library, at the same path (numbered beside one
    already there), remembering where it came from. MasterError if the project has it."""
    if (have := in_project(project, asset)) is not None:
        raise MasterError(f"'{asset.name}' is in the project's library already ({have.id})")
    target = _free(lib.library_dir(project) / master.path_of(asset))
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(asset.folder, target)
    copy = lib.read(target)
    copy.uid, copy.version, copy.fingerprint = asset.uid, asset.version, asset.fingerprint
    copy.origin = {"library": master.id, "path": master.path_of(asset), "version": asset.version,
                   "fingerprint": asset.fingerprint, "copied": _today()}
    lib.write(copy)
    return copy


def newer_in_master(copy: lib.Asset, master: Master,
                    master_assets: list[lib.Asset] | None = None) -> lib.Asset | None:
    """The Master's asset, when it is newer than the project's copy of it."""
    if not copy.origin or copy.origin.get("library") != master.id:
        return None
    assets = master_assets if master_assets is not None else master.scan()[0]
    original = next((a for a in assets if a.uid == copy.uid), None)
    if original is None:
        return None
    return original if original.version > int(copy.origin.get("version", 0)) else None


def changed_here(copy: lib.Asset) -> bool:
    """Whether a project's copy has been changed since it came from the Master."""
    return bool(copy.origin) and copy.origin.get("fingerprint") != lib.fingerprint(copy.files)


def _replace_files(target: lib.Asset, source: lib.Asset) -> None:
    """The target asset's files replaced by the source's (its own two files kept)."""
    for p in list(target.folder.iterdir()):
        if p.name in (lib.METADATA, lib.LICENCE_FILE):
            continue
        shutil.rmtree(p) if p.is_dir() else p.unlink()
    for p in source.folder.iterdir():
        if p.name in (lib.METADATA, lib.LICENCE_FILE):
            continue
        dest = target.folder / p.name
        shutil.copytree(p, dest) if p.is_dir() else shutil.copy2(p, dest)


def update_from_master(copy: lib.Asset, master: Master, original: lib.Asset) -> lib.Asset:
    """The project's copy brought up to the Master's version: its files and details."""
    _replace_files(copy, original)
    for key in ("name", "description", "tags", "licence", "author", "source", "title",
                "purchase"):
        setattr(copy, key, getattr(original, key))
    lib.record_files(copy)
    copy.version, copy.fingerprint = original.version, original.fingerprint
    copy.origin = {"library": master.id, "path": master.path_of(original),
                   "version": original.version, "fingerprint": original.fingerprint,
                   "copied": _today()}
    lib.write(copy)
    return copy


# ---- from a project into the Master ------------------------------------------------------------
def send_to_master(asset: lib.Asset, master: Master, *, replace: bool = False) -> lib.Asset:
    """A project's asset given to the Master: a new asset there, or (replace, for a copy of the
    Master's own, changed in the project) the Master's asset brought up to it as its next
    version. MasterError if the Master has these files already, or if replacing isn't said for
    a changed copy."""
    master_assets, _ = master.scan()
    if any(a.fingerprint == asset.fingerprint for a in master_assets):
        raise MasterError(f"'{asset.name}' is in the Master already (the same files)")
    original = next((a for a in master_assets if a.uid == asset.uid), None) \
        if asset.origin and asset.origin.get("library") == master.id else None
    if original is not None:
        if not replace:
            raise MasterError(f"'{asset.name}' came from the Master and was changed here: "
                              "replace the Master's with it (its next version)?")
        _replace_files(original, asset)
        for key in ("description", "tags"):
            setattr(original, key, getattr(asset, key))
        lib.record_files(original)  # (a new version, as its files changed)
        lib.write(original)
        asset.version, asset.fingerprint = original.version, original.fingerprint
        asset.origin = {**asset.origin, "version": original.version,
                        "fingerprint": original.fingerprint, "copied": _today()}
        lib.write(asset)
        return original
    path = asset.id  # (its path in the project's library: the same in the Master)
    target = _free(master.root / path)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(asset.folder, target)
    given = lib.read(target)
    given.uid = asset.uid if not any(a.uid == asset.uid for a in master_assets) else ""
    given.version, given.origin = asset.version, None
    if not given.imported:
        given.imported = _today()
    lib.write(given)
    # The project's asset is now a copy of the Master's.
    asset.uid = given.uid
    asset.origin = {"library": master.id, "path": master.path_of(given),
                    "version": given.version, "fingerprint": given.fingerprint,
                    "copied": _today()}
    lib.write(asset)
    return given


# ---- purchased originals -----------------------------------------------------------------------
def purchased(project) -> list[lib.Asset]:
    """The project's assets whose original files may not be given away: purchased ones, and
    your own all rights reserved work."""
    assets, _ = lib.scan(project)
    return [a for a in assets if not lib.may_give_away(a) and a.licence in lib.LICENCES]


def purchased_paths(project, paths: list[str]) -> dict[str, lib.Asset]:
    """Which of these paths (relative to the project folder) are purchased assets' files."""
    out = {}
    folders = {a.folder.resolve(): a for a in purchased(project)}
    root = Path(project.root).resolve()
    for rel in paths:
        p = (root / rel).resolve()
        asset = next((a for f, a in folders.items() if p == f or f in p.parents), None)
        if asset is not None:
            out[rel] = asset
    return out


def keep_out_of_git(project, assets: list[lib.Asset]) -> list[str]:
    """The assets' folders added to the project's .gitignore (so their originals never go into
    the project's history). Returns the lines added."""
    gitignore = Path(project.root) / ".gitignore"
    try:
        text = gitignore.read_text("utf-8")
    except OSError:
        text = ""
    have = set(text.splitlines())
    root = Path(project.root).resolve()
    added = []
    for a in assets:
        line = "/" + a.folder.resolve().relative_to(root).as_posix() + "/"
        if line not in have:
            added.append(line)
            have.add(line)
    if added:
        if text and not text.endswith("\n"):
            text += "\n"
        text += "# Purchased assets: their originals may not be given away\n" + \
            "\n".join(added) + "\n"
        gitignore.write_text(text, "utf-8")
    return added
