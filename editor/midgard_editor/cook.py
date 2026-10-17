# SPDX-License-Identifier: Apache-2.0
"""Build ▸ Cook: turn a project into a Midgard package the engine runs (roadmap §2, Q1).

    <out>/manifest.json      what the package is, what built it, where to start
    <out>/{name}.config      the runtime part of {project}.config
    <out>/{name}.db          a read-only copy of {project}.db
    <out>/game.pak           cooked assets (zip): meshes as .glb; imported objects' meshes
                             and KTX2 textures under models/<object id>/
    <out>/scripts/           the project's scripts/ (*.py), for entity scripts
    <out>/icon.png           the game's icon (Project Page), for the game's window, if it has one

The manifest is checked against schema/manifest.schema.json before cooking finishes. When the
engine's _midgard module is available, the engine then checks the package the way the runtime
loads it, and its problems stop the cook (errors) or are reported (warnings).
"""
import hashlib
import json
import os
import platform
import shutil
import sqlite3
import time
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

from . import contract, engine, meshes, objects
from .project import Project
from .scene_model import MAIN_SCENE
from .version import BUILD, VERSION

ENGINE_ABI = 0
FORMAT_VERSION = "1.0"
RUNTIME_CONFIG_KEYS = ("game_version", "world_mode", "max_players")
BUILTIN_PREFIX = "builtin:"
EDITOR_ONLY_TABLES = ("snippet",)
PROJECT_ASSET_PREFIX = "asset:"  # a file in the project's objects/ folder


class CookError(Exception):
    """The project can't be cooked. The message is shown to the user."""


@dataclass
class CookResult:
    out_dir: Path
    manifest: dict
    warnings: list[str] = field(default_factory=list)


def host_target() -> dict[str, str]:
    os_name = "windows" if os.name == "nt" else "linux"  # the editor runs on Windows and Linux
    arch = "arm64" if platform.machine().lower() in ("aarch64", "arm64") else "x86_64"
    return {"os": os_name, "arch": arch, "role": "game"}


def default_out_dir(project: Project, target: dict[str, str]) -> Path:
    return project.root / "build" / "cook" / f"{target['os']}-{target['arch']}" / "data"


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def replace_file(tmp: Path, dest: Path, attempts: int = 20) -> None:
    """Moves tmp over dest. On Windows a running game may have dest open for a moment, so
    retry briefly before giving up."""
    for attempt in range(attempts):
        try:
            os.replace(tmp, dest)
            return
        except PermissionError:
            if attempt == attempts - 1:
                raise
            time.sleep(0.05)


def _cook_database(project: Project, out: Path) -> int:
    """A consistent copy of {project}.db taken with SQLite's backup API, written beside the
    old one and swapped in. Returns its schema version. The engine opens it read-only; player
    saves go elsewhere."""
    db_out = out / f"{project.name}.db"
    tmp = db_out.with_suffix(".db.tmp")
    tmp.unlink(missing_ok=True)
    with project.connect() as src:
        dst = sqlite3.connect(tmp)
        try:
            src.backup(dst)
            schema_version = contract.schema_version(dst)
            for table in EDITOR_ONLY_TABLES:  # the game never reads these
                dst.execute(f"DROP TABLE IF EXISTS {table}")
            dst.commit()
            dst.execute("VACUUM")
        finally:
            dst.close()
    replace_file(tmp, db_out)
    return schema_version


def mesh_asset_data(project: Project, asset: str) -> bytes | None:
    """The .glb bytes for a mesh asset id, or None if the project doesn't have it.
    "builtin:" meshes are built into the engine and need no file."""
    if asset in meshes.PRIMITIVES:
        return meshes.to_glb(meshes.primitive(asset))
    if asset.startswith(PROJECT_ASSET_PREFIX):
        file = project.objects_dir / asset[len(PROJECT_ASSET_PREFIX):]
        if file.is_file() and file.suffix.lower() == ".glb":
            return file.read_bytes()
    return None


def used_mesh_assets(project: Project) -> set[str]:
    """Mesh asset ids the main scene uses that live in game.pak."""
    return {a for e in project.entities(MAIN_SCENE)
            if (a := e.components.get("mesh", {}).get("asset")) and
            not a.startswith(BUILTIN_PREFIX)}


@dataclass
class CookedObjects:
    parts: dict[str, str] = field(default_factory=dict)  # imported object id -> parts JSON
    files: dict[str, tuple[str, bytes]] = field(default_factory=dict)  # content id -> (path, data)


def _cook_objects(project: Project, warnings: list[str]) -> CookedObjects:
    """The objects the main scene places: imported ones made from their model files by the
    engine. Those made in the editor need nothing (their meshes are cooked with the rest)."""
    library = {o.id: o for o in project.objects()}
    cooked = CookedObjects()
    done: set[str] = set()
    for entity in project.entities(MAIN_SCENE):
        object_id = entity.components.get("object", {}).get("id")
        if not object_id or object_id in done:
            continue
        done.add(object_id)
        obj = library.get(object_id)
        if obj is None:
            warnings.append(f"Entity '{entity.name}' places object '{object_id}', which is not "
                            "in the project; nothing will be shown.")
            continue
        if not obj.source:
            continue
        try:
            result = objects.CACHE.cooked(project.objects_dir, obj)
        except objects.ObjectError as e:
            warnings.append(f"Object '{obj.name}' can't be imported: {e}")
            continue
        cooked.parts[obj.id] = json.dumps(result.parts)
        cooked.files |= result.files
        warnings += [f"Object '{obj.name}': {w}" for w in result.warnings]
    return cooked


def _write_object_parts(project: Project, out: Path, cooked: CookedObjects) -> None:
    """Imported objects' parts, as cooked, into the package's database."""
    con = sqlite3.connect(out / f"{project.name}.db")
    try:
        con.executemany("UPDATE object SET parts = ? WHERE id = ?",
                        [(parts, object_id) for object_id, parts in cooked.parts.items()])
        con.commit()
    finally:
        con.close()


def _template_meshes(project: Project) -> list[tuple[str, str]]:
    """(object name, mesh asset) for the parts of objects made in the editor."""
    out = []
    for obj in project.objects():
        if obj.source:
            continue
        for part in obj.parts:
            if asset := part.get("components", {}).get("mesh", {}).get("asset"):
                out.append((obj.name, asset))
    return out


def _cook_assets(project: Project, out: Path, warnings: list[str],
                 cooked: CookedObjects | None = None) -> list[dict[str, str]]:
    """game.pak: every mesh the main scene and its objects use, as .glb files, and imported
    objects' content. Returns the content list."""
    assets: dict[str, bytes] = {}
    users = [(f"Entity '{e.name}'", e.components.get("mesh", {}).get("asset"))
             for e in project.entities(MAIN_SCENE)]
    users += [(f"Object '{name}'", asset) for name, asset in _template_meshes(project)]
    for user, asset in users:
        if not asset or asset.startswith(BUILTIN_PREFIX) or asset in assets:
            continue
        data = mesh_asset_data(project, asset)
        if data is None:
            warnings.append(f"{user} uses mesh '{asset}', which is not in the "
                            "project; it will not be drawn.")
        else:
            assets[asset] = data
    files: dict[str, tuple[str, bytes]] = {}
    for asset, data in assets.items():
        path = f"meshes/{asset.removeprefix(PROJECT_ASSET_PREFIX)}"
        if not path.endswith(".glb"):
            path += ".glb"
        files[asset] = (path, data)
    if cooked:
        files |= cooked.files
    content: list[dict[str, str]] = []
    tmp = out / "game.pak.tmp"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as pak:
        for asset, (path, data) in sorted(files.items()):
            pak.writestr(path, data)
            content.append({"id": asset, "path": path, "sha256": _sha256(data)})
    replace_file(tmp, out / "game.pak")
    return content


SCRIPT_PREFIX = "script:"  # recook reports changed scripts as "script:<module>"


def script_files(folder: Path) -> dict[str, Path]:
    """Python modules in a scripts folder: module name ("enemies.wolf") -> file."""
    found = {}
    if folder.is_dir():
        for file in sorted(folder.rglob("*.py")):
            relative = file.relative_to(folder)
            if "__pycache__" in relative.parts:
                continue
            parts = list(relative.with_suffix("").parts)
            if parts[-1] == "__init__":
                parts = parts[:-1]
            if parts:
                found[".".join(parts)] = file
    return found


def _script_hashes(folder: Path) -> dict[str, str]:
    return {m: _sha256(f.read_bytes()) for m, f in script_files(folder).items()}


def _cook_scripts(project: Project, out: Path) -> dict[str, str]:
    """Copies the project's scripts/ into the package (only what changed, so a running game
    sees each file change once). Returns module -> sha256."""
    source, dest = project.root / "scripts", out / "scripts"
    wanted = {f.relative_to(source): f for f in script_files(source).values()}
    if dest.is_dir():
        for old in [p for p in dest.rglob("*.py") if p.relative_to(dest) not in wanted]:
            old.unlink()
    for relative, file in wanted.items():
        target = dest / relative
        data = file.read_bytes()
        if not target.is_file() or target.read_bytes() != data:
            target.parent.mkdir(parents=True, exist_ok=True)
            tmp = target.with_suffix(".py.tmp")
            tmp.write_bytes(data)
            replace_file(tmp, target)
    return _script_hashes(dest)


def _script_warnings(project: Project, modules: dict[str, str]) -> list[str]:
    return [f"Entity '{e.name}' uses script '{m}', which is not in the project's scripts/ "
            "folder; it will not run."
            for e in project.entities(MAIN_SCENE)
            if (m := e.components.get("script", {}).get("module")) and m not in modules]


ICON_PIXELS = 256  # the window icon; the Windows executable gets its own sizes at export


def _cook_icon(project: Project, out: Path) -> None:
    """icon.png for the runtime's window and taskbar icon."""
    from . import branding

    target = out / "icon.png"
    source = branding.path(project, "icon")
    if source is None:
        target.unlink(missing_ok=True)
        return
    from PySide6.QtCore import Qt
    from PySide6.QtGui import QImage

    image = QImage(str(source))
    if image.width() > ICON_PIXELS:
        image = image.scaled(ICON_PIXELS, ICON_PIXELS, Qt.AspectRatioMode.KeepAspectRatio,
                             Qt.TransformationMode.SmoothTransformation)
    target.write_bytes(branding.png_bytes(image))


def _write_manifest(project: Project, out: Path, target: dict[str, str],
                    content: list[dict[str, str]], schema_version: int) -> dict:
    content_hash = hashlib.sha256()
    for item in content:
        content_hash.update(item["sha256"].encode("ascii"))
    for module, sha in sorted(_script_hashes(out / "scripts").items()):
        content_hash.update(f"{module}={sha}".encode())
    if (out / "icon.png").is_file():
        content_hash.update(_sha256((out / "icon.png").read_bytes()).encode("ascii"))
    content_hash.update(_sha256((out / f"{project.name}.db").read_bytes()).encode("ascii"))
    manifest = {
        "format": "midgard-package",
        "format_version": FORMAT_VERSION,
        "game": {"name": project.name, "version": project.config.get("game_version", "0.1.0")},
        "built_with": {"editor": f"{VERSION}+{BUILD}", "engine_abi": ENGINE_ABI},
        "target": target,
        "world_mode": project.config.get("world_mode", "system_map"),
        "entry": {"scene": MAIN_SCENE},
        "database": f"{project.name}.db",
        "schema_version": schema_version,
        "content_hash": "sha256:" + content_hash.hexdigest(),
        "content": content,
    }
    errors = contract.manifest_errors(manifest)
    if errors:
        raise CookError("The cooked manifest is invalid:\n" + "\n".join(errors))
    tmp = out / "manifest.json.tmp"
    tmp.write_text(json.dumps(manifest, indent=2) + "\n", "utf-8")
    replace_file(tmp, out / "manifest.json")
    return manifest


def cook(project: Project, out_dir: Path | None = None,
         target: dict[str, str] | None = None) -> CookResult:
    target = target or host_target()
    out = out_dir or default_out_dir(project, target)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    warnings: list[str] = []
    schema_version = _cook_database(project, out)
    cooked = _cook_objects(project, warnings)
    _write_object_parts(project, out, cooked)
    content = _cook_assets(project, out, warnings, cooked)
    warnings += _script_warnings(project, _cook_scripts(project, out))
    _cook_icon(project, out)
    runtime_config = {k: project.config[k] for k in RUNTIME_CONFIG_KEYS if k in project.config}
    (out / f"{project.name}.config").write_text(json.dumps(runtime_config, indent=2) + "\n",
                                                "utf-8")
    manifest = _write_manifest(project, out, target, content, schema_version)
    report = engine.validate(out)
    if report is not None:
        if not report["ok"]:
            raise CookError("The engine can't load the cooked package:\n" +
                            "\n".join(report["errors"]))
        warnings += [w for w in report["warnings"] if not _already_warned(w, warnings)]
    return CookResult(out, manifest, warnings)


def _already_warned(engine_warning: str, warnings: list[str]) -> bool:
    """The editor already warns about meshes and objects it couldn't find; the engine's
    version of the same warning would only repeat it."""
    return "not in the package" in engine_warning and any("not in the project" in w
                                                          for w in warnings)


def recook(project: Project, out: Path, database: bool = False, assets: bool = False,
           scripts: bool = False) -> tuple[CookResult, set[str]]:
    """Hot reload: rewrites parts of an existing package (the database, game.pak and/or the
    scripts) and its manifest, without touching the rest. Returns the result and the ids of
    content whose data changed (added, removed or different); scripts as "script:<module>"."""
    old = json.loads((out / "manifest.json").read_text("utf-8"))
    warnings: list[str] = []
    schema_version = _cook_database(project, out) if database else old["schema_version"]
    cooked = _cook_objects(project, warnings) if database or assets else CookedObjects()
    if database:
        _write_object_parts(project, out, cooked)
    content = _cook_assets(project, out, warnings, cooked) if assets else old["content"]
    before = {c["id"]: c["sha256"] for c in old["content"]}
    after = {c["id"]: c["sha256"] for c in content}
    if scripts:
        before |= {SCRIPT_PREFIX + m: s for m, s in _script_hashes(out / "scripts").items()}
        cooked = _cook_scripts(project, out)
        after |= {SCRIPT_PREFIX + m: s for m, s in cooked.items()}
        warnings += _script_warnings(project, cooked)
    changed = {i for i in before.keys() | after.keys() if before.get(i) != after.get(i)}
    manifest = _write_manifest(project, out, old["target"], content, schema_version)
    return CookResult(out, manifest, warnings), changed
