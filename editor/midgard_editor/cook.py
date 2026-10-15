# SPDX-License-Identifier: Apache-2.0
"""Build ▸ Cook: turn a project into a Midgard package the engine runs (roadmap §2, Q1).

    <out>/manifest.json      what the package is, what built it, where to start
    <out>/{name}.config      the runtime part of {project}.config
    <out>/{name}.db          a read-only copy of {project}.db
    <out>/game.pak           cooked assets (zip): meshes as .glb

The manifest is checked against schema/manifest.schema.json before cooking finishes.
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

from . import contract, meshes
from .project import Project
from .scene_model import MAIN_SCENE
from .version import BUILD, VERSION

ENGINE_ABI = 0
FORMAT_VERSION = "1.0"
RUNTIME_CONFIG_KEYS = ("game_version", "world_mode", "max_players")
BUILTIN_PREFIX = "builtin:"
PROJECT_ASSET_PREFIX = "asset:"  # a file in the project's assets/ folder


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
        file = project.root / "assets" / asset[len(PROJECT_ASSET_PREFIX):]
        if file.is_file() and file.suffix.lower() == ".glb":
            return file.read_bytes()
    return None


def used_mesh_assets(project: Project) -> set[str]:
    """Mesh asset ids the main scene uses that live in game.pak."""
    return {a for e in project.entities(MAIN_SCENE)
            if (a := e.components.get("mesh", {}).get("asset")) and
            not a.startswith(BUILTIN_PREFIX)}


def _cook_assets(project: Project, out: Path, warnings: list[str]) -> list[dict[str, str]]:
    """game.pak: every mesh the main scene uses, as .glb files. Returns the content list."""
    assets: dict[str, bytes] = {}
    for entity in project.entities(MAIN_SCENE):
        asset = entity.components.get("mesh", {}).get("asset")
        if not asset or asset.startswith(BUILTIN_PREFIX) or asset in assets:
            continue
        data = mesh_asset_data(project, asset)
        if data is None:
            warnings.append(f"Entity '{entity.name}' uses mesh '{asset}', which is not in the "
                            "project; it will not be drawn.")
        else:
            assets[asset] = data
    content: list[dict[str, str]] = []
    tmp = out / "game.pak.tmp"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as pak:
        for asset, data in sorted(assets.items()):
            path = f"meshes/{asset.removeprefix(PROJECT_ASSET_PREFIX)}"
            if not path.endswith(".glb"):
                path += ".glb"
            pak.writestr(path, data)
            content.append({"id": asset, "path": path, "sha256": _sha256(data)})
    replace_file(tmp, out / "game.pak")
    return content


def _write_manifest(project: Project, out: Path, target: dict[str, str],
                    content: list[dict[str, str]], schema_version: int) -> dict:
    content_hash = hashlib.sha256()
    for item in content:
        content_hash.update(item["sha256"].encode("ascii"))
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
    content = _cook_assets(project, out, warnings)
    runtime_config = {k: project.config[k] for k in RUNTIME_CONFIG_KEYS if k in project.config}
    (out / f"{project.name}.config").write_text(json.dumps(runtime_config, indent=2) + "\n",
                                                "utf-8")
    manifest = _write_manifest(project, out, target, content, schema_version)
    return CookResult(out, manifest, warnings)


def recook(project: Project, out: Path, database: bool = False,
           assets: bool = False) -> tuple[CookResult, set[str]]:
    """Hot reload: rewrites parts of an existing package (the database and/or game.pak) and its
    manifest, without touching the rest. Returns the result and the ids of content whose data
    changed (added, removed or different)."""
    old = json.loads((out / "manifest.json").read_text("utf-8"))
    warnings: list[str] = []
    schema_version = _cook_database(project, out) if database else old["schema_version"]
    content = _cook_assets(project, out, warnings) if assets else old["content"]
    before = {c["id"]: c["sha256"] for c in old["content"]}
    after = {c["id"]: c["sha256"] for c in content}
    changed = {i for i in before.keys() | after.keys() if before.get(i) != after.get(i)}
    manifest = _write_manifest(project, out, old["target"], content, schema_version)
    return CookResult(out, manifest, warnings), changed
