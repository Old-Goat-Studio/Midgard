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


def cook(project: Project, out_dir: Path | None = None,
         target: dict[str, str] | None = None) -> CookResult:
    target = target or host_target()
    out = out_dir or default_out_dir(project, target)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    warnings: list[str] = []
    name = project.name

    # The database: a consistent copy taken with SQLite's backup API, made read-only in spirit
    # (the engine opens it read-only; player saves go elsewhere).
    db_out = out / f"{name}.db"
    with project.connect() as src:
        dst = sqlite3.connect(db_out)
        try:
            src.backup(dst)
            schema_version = contract.schema_version(dst)
            dst.execute("VACUUM")
        finally:
            dst.close()

    # Assets: every mesh the main scene uses. Editor primitives are generated as .glb files;
    # "builtin:" meshes are built into the engine and need no file.
    content: list[dict[str, str]] = []
    assets: dict[str, bytes] = {}
    for entity in project.entities(MAIN_SCENE):
        asset = entity.components.get("mesh", {}).get("asset")
        if not asset or asset.startswith(BUILTIN_PREFIX) or asset in assets:
            continue
        if asset in meshes.PRIMITIVES:
            assets[asset] = meshes.to_glb(meshes.primitive(asset))
        else:
            warnings.append(f"Entity '{entity.name}' uses mesh '{asset}', which is not in the "
                            "project; it will not be drawn.")
    with zipfile.ZipFile(out / "game.pak", "w", zipfile.ZIP_DEFLATED) as pak:
        for asset, data in sorted(assets.items()):
            path = f"meshes/{asset}.glb"
            pak.writestr(path, data)
            content.append({"id": asset, "path": path, "sha256": _sha256(data)})

    runtime_config = {k: project.config[k] for k in RUNTIME_CONFIG_KEYS if k in project.config}
    (out / f"{name}.config").write_text(json.dumps(runtime_config, indent=2) + "\n", "utf-8")

    content_hash = hashlib.sha256()
    for item in content:
        content_hash.update(item["sha256"].encode("ascii"))
    content_hash.update(_sha256(db_out.read_bytes()).encode("ascii"))

    manifest = {
        "format": "midgard-package",
        "format_version": FORMAT_VERSION,
        "game": {"name": name, "version": project.config.get("game_version", "0.1.0")},
        "built_with": {"editor": f"{VERSION}+{BUILD}", "engine_abi": ENGINE_ABI},
        "target": target,
        "world_mode": project.config.get("world_mode", "system_map"),
        "entry": {"scene": MAIN_SCENE},
        "database": db_out.name,
        "schema_version": schema_version,
        "content_hash": "sha256:" + content_hash.hexdigest(),
        "content": content,
    }
    errors = contract.manifest_errors(manifest)
    if errors:
        raise CookError("The cooked manifest is invalid:\n" + "\n".join(errors))
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", "utf-8")
    return CookResult(out, manifest, warnings)
