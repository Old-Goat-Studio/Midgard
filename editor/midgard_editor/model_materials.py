# SPDX-License-Identifier: Apache-2.0
"""A model's own material, for an entity that uses the model as its mesh (or a character's
model): its colours and surface, and its textures (milestone 5.13a).

An object imported through the Objects panel has its model's materials as parts; a model used
directly (from objects/ or the Asset Library) is one mesh, so it is drawn with its first
material. The entity's own material component wins wherever it says something (a colour, a
texture slot); the model's fills in the rest. Its textures are cooked to KTX2 by the engine's
importer, as content "mdl:<model's content id>#<n>" (images/models/... in game.pak).

Importing a model and encoding its textures can take a minute for big ones (a character with
4K maps), so the result is kept on disk (build/cache/models/, by the contents of the model and
the files it names) and made once; the editor's views make it on a worker thread
(ModelPreparer) and draw the model plainly meanwhile."""
import contextlib
import json
import threading
from pathlib import Path
from typing import Any

from PySide6.QtCore import QObject, Signal

from . import content_cache, engine
from .objects import _material

PREFIX = "mdl:"
Result = tuple[dict[str, Any] | None, dict[str, bytes]]
_cache: dict[tuple[str, int, int], Result] = {}
_lock = threading.Lock()
_importing: dict[tuple[str, int, int], threading.Lock] = {}  # one import of a model at a time


def texture_id(asset: str, index: int) -> str:
    return f"{PREFIX}{asset}#{index}"


def pak_path(texture: str) -> str:
    """Where a model's texture goes in game.pak."""
    asset, index = texture[len(PREFIX):].rsplit("#", 1)
    safe = "".join(c if c.isalnum() or c in "._-" else "_" for c in asset)
    return f"images/models/{safe}/{index}.ktx2"


def model_file(project, asset: str) -> Path | None:
    """The file of a model content id (objects/ or the Asset Library); None for others."""
    from . import asset_library
    if asset.startswith("asset:"):
        file = Path(project.objects_dir) / asset[len("asset:"):]
        return file if file.is_file() else None
    if asset.startswith(asset_library.MODEL_PREFIX):
        return asset_library.model_file(project, asset)
    return None


def _files_named(file: Path) -> list[Path]:
    """The files a .gltf names (its buffers and images, beside it); none for a .glb."""
    if file.suffix.lower() != ".gltf":
        return []
    try:
        doc = json.loads(file.read_text("utf-8"))
    except (OSError, ValueError):
        return []
    uris = [x.get("uri", "") for x in doc.get("buffers", []) + doc.get("images", [])]
    return sorted({file.parent / u for u in uris if u and not u.startswith("data:")})


def _keys(project, asset: str) -> tuple[tuple[str, int, int], Path] | None:
    file = model_file(project, asset)
    module = engine.module()
    if file is None or module is None or not hasattr(module, "import_model"):
        return None
    try:
        stat = file.stat()
    except OSError:
        return None
    return (str(file), stat.st_mtime_ns, stat.st_size), file


def _disk(project, file: Path, asset: str) -> tuple[content_cache.DiskCache, str]:
    disk = content_cache.DiskCache(content_cache.cache_dir(Path(project.root), "models"))
    named = [content_cache.file_hash(f) for f in _files_named(file)]
    return disk, content_cache.key_of("model-material", asset, content_cache.file_hash(file),
                                      named)


def _from_disk(found) -> Result:
    data, blobs = found
    return data["material"], {texture: blobs[name] for texture, name in data["textures"].items()
                              if name in blobs}


def ready(project, asset: str) -> bool:
    """Whether the model's material can be had now, without importing it (in memory, on disk,
    or nothing to import)."""
    keys = _keys(project, asset)
    if keys is None:
        return True
    key, file = keys
    with _lock:
        if key in _cache:
            return True
    disk, disk_key = _disk(project, file, asset)
    found = disk.get(disk_key)
    if found is None:
        return False
    with _lock:
        _cache[key] = _from_disk(found)
    return True


def model_material(project, asset: str) -> Result:
    """The model's first material as a material component (None: it has none, or it can't be
    read), and the KTX2 textures it names. Made once per change of its files (kept on disk);
    this waits for it (seconds, a minute for big ones): see ready() and ModelPreparer."""
    keys = _keys(project, asset)
    if keys is None:
        return None, {}
    key, file = keys
    with _lock:
        if key in _cache:
            return _cache[key]
        importing = _importing.setdefault(key, threading.Lock())
    with importing:  # (another thread importing it: its result, when it's done)
        with _lock:
            if key in _cache:
                return _cache[key]
        disk, disk_key = _disk(project, file, asset)
        found = disk.get(disk_key)
        if found is not None:
            result = _from_disk(found)
        else:
            result = _import(engine.module(), file, asset)
            material, textures = result
            names = {texture: f"t{n}" for n, texture in enumerate(textures)}
            disk.put(disk_key, {"material": material, "textures": names},
                     {names[t]: data for t, data in textures.items()})
        with _lock:
            _cache[key] = result
            _importing.pop(key, None)
    return result


def _import(module, file: Path, asset: str) -> Result:
    try:
        model = module.import_model(file, json.dumps({}))  # (without the GIL)
    except RuntimeError:
        return None, {}
    index = next((p["material"] for p in model["parts"] if p["material"] >= 0), -1)
    if index < 0:
        return None, {}
    m = model["materials"][index]
    material = _material("x", m)  # (texture ids set below)
    used = dict(m["textures"])  # slot -> the model's texture index
    if used:
        material["textures"] = {slot: texture_id(asset, i) for slot, i in used.items()}
    return material, {texture_id(asset, i): model["textures"][i]["ktx2"] for i in used.values()}


class ModelPreparer(QObject):
    """Models' materials made on a worker thread, for a view that draws the model plainly
    meanwhile: ready(asset) comes on the view's thread when one is done."""
    ready = Signal(str)

    def __init__(self, parent: QObject | None = None):
        super().__init__(parent)
        self._busy: set[str] = set()

    def prepare(self, project, asset: str) -> None:
        if asset in self._busy:
            return
        self._busy.add(asset)

        def work() -> None:
            try:
                model_material(project, asset)
            finally:
                with contextlib.suppress(RuntimeError):  # (its view has gone)
                    self.ready.emit(asset)

        threading.Thread(target=work, name=f"prepare {asset}", daemon=True).start()

    def done(self, asset: str) -> None:
        self._busy.discard(asset)


def _model_of(components: dict) -> str:
    """The model whose material an entity's components take (its mesh's, or its character's)."""
    mesh = components.get("mesh", {}).get("asset", "")
    if mesh.startswith(("asset:", "lib:")):
        return mesh
    model = components.get("character", {}).get("model", "")
    return model if model.startswith(("asset:", "lib:")) else ""


def model_of(components: dict) -> str:
    return _model_of(components)


def effective(project, components: dict) -> dict:
    """The components as drawn: the model's material under the entity's own, and a surface
    shader's settings laid out for the engine (surface_shaders.drawn)."""
    from . import surface_shaders
    merged = _with_model_material(project, components)
    return surface_shaders.drawn(project.root, merged)


def _with_model_material(project, components: dict) -> dict:
    asset = _model_of(components)
    if not asset:
        return components
    material, _ = model_material(project, asset)
    if material is None:
        return components
    own = components.get("material", {})
    merged = {**material, **{k: v for k, v in own.items() if k != "textures"}}
    slots = {**material.get("textures", {}), **own.get("textures", {})}
    if slots:
        merged["textures"] = slots
    else:
        merged.pop("textures", None)
    return {**components, "material": merged}


def textures_of(project, components: dict) -> dict[str, bytes]:
    """The model textures an entity's components draw with (those its own don't replace)."""
    asset = _model_of(components)
    if not asset:
        return {}
    _material_, data = model_material(project, asset)
    used = set(effective(project, components).get("material", {}).get("textures", {}).values())
    return {t: d for t, d in data.items() if t in used}
