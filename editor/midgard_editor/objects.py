# SPDX-License-Identifier: Apache-2.0
"""Objects: things made once and placed many times (roadmap §5.5).

An object is a template kept in the project's object table: a list of parts, each a name and a
set of components like an entity's. An entity with {"object": {"id": ...}} is an instance; the
engine fills in the template's parts as its children, so changing the template changes every
instance, and the instance moves (and falls) as one, like a group.

An object made from a model file (its source, in the project's objects/ folder) is imported by
the engine when the game is cooked: each of the model's materials becomes a part with a mesh
(content "obj:<id>/<n>") and a material whose textures are KTX2 content ("obj:<id>/tex<n>"),
and the object's collider setting adds the collision shape.
"""
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from . import engine

# How an imported object collides: one box round it all (the default, cheap and stable), each
# part's convex hull, each part's exact triangles (static scenery only), or not at all.
COLLIDERS = ("box", "convex", "mesh", "none")
TEXTURE_SLOTS = ("base_color", "metallic_roughness", "normal", "occlusion", "emissive")
MIN_BOX = 0.01  # a flat model still gets a box this thick


class ObjectError(Exception):
    """An object can't be cooked. The message is shown to the user."""


@dataclass
class ObjectDef:
    id: str
    name: str
    source: str | None = None  # a model file relative to objects/, or None (made in the editor)
    settings: dict[str, Any] = field(default_factory=dict)  # scale, z_up, collider
    parts: list[dict[str, Any]] = field(default_factory=list)  # [{"name", "components"}]


@dataclass
class CookedObject:
    parts: list[dict[str, Any]]
    files: dict[str, tuple[str, bytes]]  # content id -> (path in game.pak, data)
    warnings: list[str] = field(default_factory=list)


def mesh_id(object_id: str, index: int) -> str:
    return f"obj:{object_id}/{index}"


def texture_id(object_id: str, index: int) -> str:
    return f"obj:{object_id}/tex{index}"


def _srgb(c: float) -> float:
    """glTF's factors are linear; a Midgard material's colours are sRGB."""
    c = min(max(c, 0.0), 1.0)
    return c * 12.92 if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055


def _material(object_id: str, m: dict[str, Any]) -> dict[str, Any]:
    r, g, b, a = m["color"]
    out: dict[str, Any] = {
        "color": [round(_srgb(r), 6), round(_srgb(g), 6), round(_srgb(b), 6), a],
        "metallic": m["metallic"],
        "roughness": m["roughness"],
    }
    if any(m["emissive"]):
        out["emissive"] = [round(_srgb(c), 6) for c in m["emissive"]]
        out["emissive_strength"] = m["emissive_strength"]
    if m["normal_scale"] != 1.0:
        out["normal_scale"] = m["normal_scale"]
    if m["occlusion_strength"] != 1.0:
        out["occlusion_strength"] = m["occlusion_strength"]
    if m["textures"]:
        out["textures"] = {slot: texture_id(object_id, index)
                           for slot, index in m["textures"].items()}
    return out


def cook_object(objects_dir: Path, obj: ObjectDef) -> CookedObject:
    """An imported object's parts and package content, made by the engine from its source.
    Raises ObjectError when the model can't be imported."""
    module = engine.module()
    if module is None:
        raise ObjectError("importing models needs the engine module (_midgard), which isn't "
                          "available")
    if not obj.source:
        raise ObjectError("it has no model file")
    file = objects_dir / obj.source
    if not file.is_file():
        raise ObjectError(f"its model file '{obj.source}' is not in the objects folder")
    collider = obj.settings.get("collider", "box")
    if collider not in COLLIDERS:
        raise ObjectError(f"unknown collider '{collider}' (use one of {', '.join(COLLIDERS)})")
    settings = {k: obj.settings[k] for k in ("scale", "z_up") if k in obj.settings}
    try:
        model = module.import_model(file, json.dumps(settings))
    except RuntimeError as e:
        raise ObjectError(str(e)) from None

    files: dict[str, tuple[str, bytes]] = {}
    for i, texture in enumerate(model["textures"]):
        files[texture_id(obj.id, i)] = (f"models/{obj.id}/tex{i}.ktx2", texture["ktx2"])
    parts: list[dict[str, Any]] = []
    for i, part in enumerate(model["parts"]):
        files[mesh_id(obj.id, i)] = (f"models/{obj.id}/{i}.glb", part["glb"])
        components: dict[str, Any] = {"mesh": {"asset": mesh_id(obj.id, i)}}
        if part["material"] >= 0:
            components["material"] = _material(obj.id, model["materials"][part["material"]])
        if collider in ("convex", "mesh"):
            components["collider"] = {"shape": collider}
        parts.append({"name": part["name"] or f"part {i}", "components": components})
    if collider == "box":
        lo, hi = model["bounds"][:3], model["bounds"][3:]
        parts.append({"name": "collider", "components": {
            "transform": {"position": [(a + b) / 2 for a, b in zip(lo, hi, strict=True)]},
            "collider": {"size": [max(b - a, MIN_BOX) for a, b in zip(lo, hi, strict=True)]}}})
    return CookedObject(parts, files, list(model["warnings"]))
