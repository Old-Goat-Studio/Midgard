# SPDX-License-Identifier: Apache-2.0
"""Build ▸ Cook: turn a project into a Midgard package the engine runs (roadmap §2, Q1).

    <out>/manifest.json      what the package is, what built it, where to start
    <out>/{name}.config      the runtime part of {project}.config
    <out>/{name}.db          a read-only copy of {project}.db
    <out>/game.pak           cooked assets (zip): meshes as .glb; imported objects' meshes
                             and KTX2 textures under models/<object id>/; image files that
                             materials use as KTX2 under images/
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

from . import (
    ambience,
    asset_library,
    contract,
    engine,
    gltf_pack,
    menus,
    meshes,
    model_materials,
    music,
    objects,
    scenes,
    sound_events,
    sounds,
    stats,
    surface_shaders,
    textures,
    weather,
)
from .project import Project
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
    credits: list[str] = field(default_factory=list)  # (CC BY assets', in CREDITS.txt)


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
            # A model used as a mesh brings its own material, under the entity's (5.13a).
            for entity_id, text in dst.execute("SELECT id, components FROM entity").fetchall():
                components = json.loads(text)
                drawn = model_materials.effective(project, components)
                if drawn is not components:
                    dst.execute("UPDATE entity SET components = ? WHERE id = ?",
                                (json.dumps(drawn), entity_id))
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
    file = None
    if asset.startswith(PROJECT_ASSET_PREFIX):
        file = project.objects_dir / asset[len(PROJECT_ASSET_PREFIX):]
    elif asset.startswith(asset_library.MODEL_PREFIX):  # (a model in the Asset Library)
        file = asset_library.model_file(project, asset)
    if file is not None:
        if file.is_file() and file.suffix.lower() == ".glb":
            return file.read_bytes()
        if file.is_file() and file.suffix.lower() == ".gltf":  # (with its .bin: one .glb)
            try:
                return gltf_pack.pack(file)
            except gltf_pack.PackError:
                return None
    return None


def character_assets_used(components: dict) -> list[str]:
    """The model and clip files a character component names (milestone 5.13)."""
    character = components.get("character", {})
    return [a for a in (character.get("model"), character.get("clips")) if a]


def used_mesh_assets(project: Project, model_textures: bool = True) -> set[str]:
    """Mesh asset ids the game's scenes use that live in game.pak, and the image files their
    materials use (the live link cooks them in before sending an entity that uses them).
    model_textures=False leaves out the textures models bring ("mdl:"), which can take a
    minute to make the first time (the credits only need the models)."""
    entities = project.all_entities()
    models = {t for e in entities for t in model_materials.textures_of(project, e.components)
              } if model_textures else set()
    return models | {a for e in entities
            if (a := e.components.get("mesh", {}).get("asset")) and
            not a.startswith(BUILTIN_PREFIX)} | {
        a for e in entities for a in character_assets_used(e.components)} | {
        t for e in entities for t in textures.components_textures(e.components)
        if textures.parse(t) is not None} | {
        s for e in entities for s in sounds.sounds_in(e.components)} | {
        s for ev in sound_events.load_all(project) for s in sound_events.sounds_of(ev)}


@dataclass
class CookedObjects:
    parts: dict[str, str] = field(default_factory=dict)  # imported object id -> parts JSON
    files: dict[str, tuple[str, bytes]] = field(default_factory=dict)  # content id -> (path, data)


def _cook_objects(project: Project, warnings: list[str]) -> CookedObjects:
    """The objects the game's scenes place: imported ones made from their model files by the
    engine. Those made in the editor need nothing (their meshes are cooked with the rest)."""
    library = {o.id: o for o in project.objects()}
    cooked = CookedObjects()
    done: set[str] = set()
    for entity in project.all_entities():
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
            result = objects.object_content(project, obj)
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
    """game.pak: every mesh the game's scenes and objects use, as .glb files, and imported
    objects' content. Returns the content list."""
    assets: dict[str, bytes] = {}
    all_entities = project.all_entities()
    users = [(f"Entity '{e.name}'", e.components.get("mesh", {}).get("asset"))
             for e in all_entities]
    users += [(f"Character '{e.name}'", a) for e in all_entities
              for a in character_assets_used(e.components)]
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
        if asset.startswith(asset_library.MODEL_PREFIX):
            path = f"meshes/library/{asset.removeprefix(asset_library.MODEL_PREFIX)}"
        if not path.endswith(".glb"):
            path += ".glb"
        files[asset] = (path, data)
    if cooked:
        files |= cooked.files
    # Image files the scenes' materials, and templates made in the editor, use.
    texture_users = [(f"Entity '{e.name}'", e.components) for e in all_entities]
    texture_users += [(f"Object '{o.name}'", p.get("components", {})) for o in project.objects()
                      if not o.source for p in o.parts]
    for user, components in texture_users:
        for texture in textures.components_textures(components):
            if texture in files or textures.parse(texture) is None:
                continue
            data = textures.texture_data(project.root, texture)
            if data is None:
                warnings.append(f"{user} uses texture '{textures.describe(texture)}', which is "
                                "not in the project (or can't be read); it is drawn without it.")
            else:
                files[texture] = (textures.pak_path(texture), data)
    # Surface shaders the materials use (milestone 5.17), compiled to SPIR-V: one that doesn't
    # compile is left out (drawn magenta), its mistakes warned of with their lines.
    for user, components in texture_users:
        shader = surface_shaders.components_shader(components)
        if not shader or shader in files:
            continue
        name = surface_shaders.parse(shader) or shader
        files[shader] = ("", b"")  # (warned once; taken out below)
        path = surface_shaders.source_path(project.root, shader)
        if path is None or not path.is_file():
            warnings.append(f"{user} uses surface shader '{name}', which is not in the "
                            "project; it is drawn magenta.")
            continue
        result = surface_shaders.compile_shader(project.root, shader)
        if result.ok:
            files[shader] = (surface_shaders.pak_path(shader), result.spirv)
            continue
        mistakes = [m for m in result.messages if m.get("error", True)] or result.messages
        warnings.append(f"{user} uses surface shader '{name}', which doesn't compile; it is "
                        "drawn magenta. " + "; ".join(surface_shaders.describe(m)
                                                       for m in mistakes[:5]))
    files = {k: v for k, v in files.items() if v[0]}
    # Models used as meshes: their own textures (milestone 5.13a).
    for e in all_entities:
        for texture, data in model_materials.textures_of(project, e.components).items():
            files[texture] = (model_materials.pak_path(texture), data)
    # Sounds the scenes name and every sound event plays (milestone 5.14; scripts play events
    # by name, so all of them): Ogg Vorbis or Opus, read back before they go in.
    events = sound_events.load_all(project)
    known = {e["id"] for e in events} | {e["name"] for e in events}
    for e in all_entities:
        event = e.components.get("sound", {}).get("event", "")
        if "sound" in e.components and event not in known:
            warnings.append(f"Entity '{e.name}' plays sound event '{event}', which the project "
                            "doesn't have; it will be silent.")
    warnings += music.problems(project, events)  # (playlists: tracks no event plays)
    warnings += ambience.problems(project, events)
    warnings += weather.problems(project, events)  # (weather types: their sounds, their numbers)
    warnings += weather.world_problems(project)  # (world types' weather tables)
    sound_users = texture_users + [(f"Sound event '{e['name']}'", e["event"]) for e in events]
    for user, components in sound_users:
        for sound in sounds.sounds_in(components):
            if sound in files:
                continue
            data = sounds.sound_data(project.root, sound)
            if data is None:
                warnings.append(f"{user} uses sound '{sounds.describe(sound)}', which is not "
                                "in the project (or can't be read); it will be silent.")
            else:
                files[sound] = (sounds.pak_path(sound), data)
    # Pictures the Menu editor's screens show, and the splash, crosshair and menu pointer (6.0):
    # PNG or JPEG as they are (the game's UI reads them), at ui/<their path in the project>.
    used = [image for screen in menus.load_all(project) for image in
            menus.images_of(screen["screen"])] + menus.settings_images(menus.game_settings(project))
    for image in used:
        file = menus.image_file(project, image)
        if file is not None and file.suffix.lower() in menus.IMAGE_SUFFIXES:
            files[image] = ("ui/" + image[len(menus.IMAGE_PREFIX):], file.read_bytes())
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
            for e in project.all_entities()
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
                    content: list[dict[str, str]], schema_version: int,
                    entry_scene: str | None = None) -> dict:
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
        "entry": {"scene": entry_scene or scenes.start_scene(project)},
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
         target: dict[str, str] | None = None, entry_scene: str | None = None) -> CookResult:
    """The project as a package for target (this machine if none), starting in entry_scene
    (Play's open scene; none: the starting scene)."""
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
    credits = _cook_credits(project, out, content, warnings)
    warnings += _script_warnings(project, _cook_scripts(project, out))
    warnings += stats.problems(project)  # (6.0: their ranges, names given twice)
    warnings += menus.problems(project)  # (6.0: the Menu editor's screens)
    warnings += menus.settings_problems(project)  # (the splash, crosshair, pointer, music)
    warnings += stats.script_problems(project, script_files(project.root / "scripts"))
    _cook_icon(project, out)
    runtime_config = {k: project.config[k] for k in RUNTIME_CONFIG_KEYS if k in project.config}
    (out / f"{project.name}.config").write_text(json.dumps(runtime_config, indent=2) + "\n",
                                                "utf-8")
    manifest = _write_manifest(project, out, target, content, schema_version, entry_scene)
    report = engine.validate(out)
    if report is not None:
        if not report["ok"]:
            raise CookError("The engine can't load the cooked package:\n" +
                            "\n".join(report["errors"]))
        warnings += [w for w in report["warnings"] if not _already_warned(w, warnings)]
    return CookResult(out, manifest, warnings, credits)


def _cook_credits(project: Project, out: Path, content: list[dict[str, str]],
                  warnings: list[str]) -> list[str]:
    """CREDITS.txt for the Asset Library's assets the game uses (milestone 5.13a): the credits
    CC BY ones need, and thanks for the CC0 ones; a warning for any whose licence or files
    aren't right."""
    used = asset_library.used_by(project, [c["id"] for c in content])
    for asset in used:
        for problem in asset_library.problems(asset):
            warnings.append(f"Library asset '{asset.name}' ({asset.id}): {problem}.")
    if used:
        (out / asset_library.CREDITS_FILE).write_text(
            asset_library.credits_text(project.game_title, used), "utf-8")
    return asset_library.credits(used)


def _already_warned(engine_warning: str, warnings: list[str]) -> bool:
    """The editor already warns about meshes and objects it couldn't find; the engine's
    version of the same warning would only repeat it."""
    if "surface shader" in engine_warning:  # (5.17: the editor said why already)
        return any("surface shader" in w for w in warnings)
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
    with project.connect() as con:  # the scene it started in, while there is one
        kept = con.execute("SELECT id FROM scene WHERE id = ?",
                           (old["entry"]["scene"],)).fetchone()
    manifest = _write_manifest(project, out, old["target"], content, schema_version,
                               kept[0] if kept else None)
    return CookResult(out, manifest, warnings), changed
