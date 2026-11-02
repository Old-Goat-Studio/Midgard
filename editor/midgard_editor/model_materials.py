# SPDX-License-Identifier: Apache-2.0
"""A model's own material, for an entity that uses the model as its mesh (or a character's
model): its colours and surface, and its textures (milestone 5.13a).

An object imported through the Objects panel has its model's materials as parts; a model used
directly (from objects/ or the Asset Library) is one mesh, so it is drawn with its first
material. The entity's own material component wins wherever it says something (a colour, a
texture slot); the model's fills in the rest. Its textures are cooked to KTX2 by the engine's
importer, as content "mdl:<model's content id>#<n>" (images/models/... in game.pak)."""
import json
from pathlib import Path
from typing import Any

from . import engine
from .objects import _material

PREFIX = "mdl:"
_cache: dict[tuple[str, int, int], tuple[dict[str, Any] | None, dict[str, bytes]]] = {}


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


def model_material(project, asset: str) -> tuple[dict[str, Any] | None, dict[str, bytes]]:
    """The model's first material as a material component (None: it has none, or it can't be
    read), and the KTX2 textures it names. Read once per change of the file."""
    file = model_file(project, asset)
    module = engine.module()
    if file is None or module is None or not hasattr(module, "import_model"):
        return None, {}
    stat = file.stat()
    key = (str(file), stat.st_mtime_ns, stat.st_size)
    if key in _cache:
        return _cache[key]
    result: tuple[dict[str, Any] | None, dict[str, bytes]] = (None, {})
    try:
        model = module.import_model(file, json.dumps({}))
    except RuntimeError:
        model = None
    if model is not None:
        index = next((p["material"] for p in model["parts"] if p["material"] >= 0), -1)
        if index >= 0:
            m = model["materials"][index]
            material = _material("x", m)  # (texture ids set below)
            used = dict(m["textures"])  # slot -> the model's texture index
            if used:
                material["textures"] = {slot: texture_id(asset, i) for slot, i in used.items()}
            result = (material, {texture_id(asset, i): model["textures"][i]["ktx2"]
                                 for i in used.values()})
    _cache[key] = result
    return result


def _model_of(components: dict) -> str:
    """The model whose material an entity's components take (its mesh's, or its character's)."""
    mesh = components.get("mesh", {}).get("asset", "")
    if mesh.startswith(("asset:", "lib:")):
        return mesh
    model = components.get("character", {}).get("model", "")
    return model if model.startswith(("asset:", "lib:")) else ""


def effective(project, components: dict) -> dict:
    """The components as drawn: the model's material under the entity's own."""
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
