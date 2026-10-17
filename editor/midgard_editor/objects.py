# SPDX-License-Identifier: Apache-2.0
"""Objects: things made once and placed many times (roadmap §5.5).

An object is a template kept in the project's object table: a list of parts, each a name and a
set of components like an entity's. An entity with {"object": {"id": ...}} is an instance; the
engine fills in the template's parts as its children, so changing the template changes every
instance, and the instance moves (and falls) as one, like a group.

An object made from a model file (its source, in the project's objects/ folder) is imported by
the engine: each of the model's materials becomes a part with a mesh (content "obj:<id>/<n>")
and a material whose textures are KTX2 content ("obj:<id>/tex<n>"), and the object's collider
setting adds the collision shape. Simpler versions of the model (levels of detail, "lods") are
drawn from a distance on.
"""
import json
import math
import re
import shutil
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from . import engine

# How an imported object collides: one box round it all (the default, cheap and stable), each
# part's convex hull, each part's exact triangles (static scenery only), or not at all.
COLLIDERS = {"box": "Box round it all", "convex": "Convex hull of each part",
             "mesh": "Exact triangles (static only)", "none": "None"}
TEXTURE_SLOTS = ("base_color", "metallic_roughness", "normal", "occlusion", "emissive")
MODEL_SUFFIXES = (".gltf", ".glb")
MIN_BOX = 0.01  # a flat model still gets a box this thick


class ObjectError(Exception):
    """An object can't be imported. The message is shown to the user."""


@dataclass
class ObjectDef:
    id: str
    name: str
    source: str | None = None  # a model file relative to objects/, or None (made in the editor)
    settings: dict[str, Any] = field(default_factory=dict)  # scale, z_up, collider, lods
    parts: list[dict[str, Any]] = field(default_factory=list)  # [{"name", "components"}]


@dataclass
class CookedObject:
    parts: list[dict[str, Any]]
    files: dict[str, tuple[str, bytes]]  # content id -> (path in game.pak, data)
    warnings: list[str] = field(default_factory=list)
    materials: list[dict[str, Any]] = field(default_factory=list)  # as the model has them
    bounds: list[float] = field(default_factory=lambda: [-0.5, -0.5, -0.5, 0.5, 0.5, 0.5])


def mesh_id(object_id: str, index: int, lod: int = 0) -> str:
    return f"obj:{object_id}/{index}" + (f"/lod{lod}" if lod else "")


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


def _import(module, file: Path, settings: dict[str, Any]) -> dict:
    try:
        return module.import_model(file, json.dumps(settings))
    except RuntimeError as e:
        raise ObjectError(str(e)) from None


def cook_object(objects_dir: Path, obj: ObjectDef) -> CookedObject:
    """An imported object's parts and package content, made by the engine from its source.
    Raises ObjectError when the model can't be imported."""
    module = engine.module()
    if module is None or not hasattr(module, "import_model"):
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
    model = _import(module, file, settings)
    warnings = list(model["warnings"])

    files: dict[str, tuple[str, bytes]] = {}
    for i, texture in enumerate(model["textures"]):
        files[texture_id(obj.id, i)] = (f"models/{obj.id}/tex{i}.ktx2", texture["ktx2"])
    # Levels of detail: the same parts (one per material, in the same order), simpler.
    lods: list[tuple[float, list[dict]]] = []
    for k, lod in enumerate(obj.settings.get("lods", []), start=1):
        lod_file = objects_dir / lod.get("source", "")
        if not lod.get("source") or not lod_file.is_file():
            warnings.append(f"level of detail {k}: '{lod.get('source')}' is not in the "
                            "objects folder; left out")
            continue
        try:
            lod_model = _import(module, lod_file, settings)
        except ObjectError as e:
            warnings.append(f"level of detail {k}: {e}; left out")
            continue
        if len(lod_model["parts"]) != len(model["parts"]):
            warnings.append(f"level of detail {k} has {len(lod_model['parts'])} parts (one per "
                            f"material) where the model has {len(model['parts'])}; left out")
            continue
        lods.append((float(lod.get("distance", 0.0)), lod_model["parts"]))
    parts: list[dict[str, Any]] = []
    for i, part in enumerate(model["parts"]):
        files[mesh_id(obj.id, i)] = (f"models/{obj.id}/{i}.glb", part["glb"])
        mesh: dict[str, Any] = {"asset": mesh_id(obj.id, i)}
        for k, (distance, lod_parts) in enumerate(lods, start=1):
            files[mesh_id(obj.id, i, k)] = (f"models/{obj.id}/{i}.lod{k}.glb",
                                            lod_parts[i]["glb"])
            mesh.setdefault("lods", []).append({"asset": mesh_id(obj.id, i, k),
                                                "distance": distance})
        components: dict[str, Any] = {"mesh": mesh}
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
    return CookedObject(parts, files, warnings, list(model["materials"]), list(model["bounds"]))


# ---- a cache, so the editor imports each model once per change ----------------------------

def _stamp(file: Path) -> tuple[int, int]:
    try:
        s = file.stat()
        return s.st_mtime_ns, s.st_size
    except OSError:
        return 0, 0


class ObjectCache:
    """Imported objects, made again when the object's settings or its files change."""

    def __init__(self, size: int = 32):
        self.size = size
        self._items: dict[tuple, CookedObject] = {}

    def key(self, objects_dir: Path, obj: ObjectDef) -> tuple:
        sources = [obj.source or ""] + [lod.get("source", "") for lod in
                                        obj.settings.get("lods", [])]
        return (str(objects_dir), obj.id, json.dumps(obj.settings, sort_keys=True),
                tuple((s, _stamp(objects_dir / s)) for s in sources if s))

    def cooked(self, objects_dir: Path, obj: ObjectDef) -> CookedObject:
        key = self.key(objects_dir, obj)
        if key not in self._items:
            result = cook_object(objects_dir, obj)  # raises ObjectError
            if len(self._items) >= self.size:
                self._items.pop(next(iter(self._items)))
            self._items[key] = result
        return self._items[key]


CACHE = ObjectCache()


def object_content(project, obj: ObjectDef) -> CookedObject:
    """What it takes to show an object: its parts, the meshes and textures they use (content
    id -> (path, data)) and its bounds. Imported objects come from the cache (raising
    ObjectError if the model can't be imported); those made in the editor are their template,
    with the meshes of the editor's primitives and of objects/."""
    if obj.source:
        return CACHE.cooked(project.objects_dir, obj)
    from .cook import BUILTIN_PREFIX, mesh_asset_data  # (cook uses this module)
    files: dict[str, tuple[str, bytes]] = {}
    warnings = []
    for part in obj.parts:
        mesh = part.get("components", {}).get("mesh", {})
        for asset in [mesh.get("asset")] + [lod.get("asset") for lod in mesh.get("lods", [])]:
            if not asset or asset.startswith(BUILTIN_PREFIX) or asset in files:
                continue
            data = mesh_asset_data(project, asset)
            if data is None:
                warnings.append(f"mesh '{asset}' is not in the project")
            else:
                files[asset] = (asset, data)
    return CookedObject(list(obj.parts), files, warnings, [], template_bounds(obj.parts))


# ---- the object library -------------------------------------------------------------------

def new_object_id(taken: set[str], name: str) -> str:
    base = "obj_" + (re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_") or "object")
    found, n = base, 1
    while found in taken:
        n += 1
        found = f"{base}_{n}"
    return found


def model_files(objects_dir: Path) -> list[str]:
    """Model files in objects/ (relative, '/'-separated), in order."""
    if not objects_dir.is_dir():
        return []
    return sorted(f.relative_to(objects_dir).as_posix() for f in objects_dir.rglob("*")
                  if f.suffix.lower() in MODEL_SUFFIXES and f.is_file()
                  and not any(p.startswith(".") for p in f.relative_to(objects_dir).parts))


def new_models(objects_dir: Path, objects: list[ObjectDef]) -> list[str]:
    """Model files in objects/ that no object uses yet (as its model or a level of detail)."""
    used = set()
    for obj in objects:
        used.add(obj.source)
        used |= {lod.get("source") for lod in obj.settings.get("lods", [])}
    return [f for f in model_files(objects_dir) if f not in used]


def object_for_model(objects: list[ObjectDef], relative: str) -> ObjectDef:
    """A new object for a model file in objects/, named after it."""
    name = Path(relative).stem.replace("_", " ").replace("-", " ").strip().title() or "Object"
    return ObjectDef(new_object_id({o.id for o in objects}, name), name, relative, {})


def gltf_dependencies(file: Path) -> list[str]:
    """The relative files a .gltf refers to (buffers and images), as written in it."""
    if file.suffix.lower() != ".gltf":
        return []
    try:
        doc = json.loads(file.read_text("utf-8"))
    except (OSError, ValueError):
        return []
    uris = [item.get("uri", "") for key in ("buffers", "images") for item in doc.get(key, [])
            if isinstance(item, dict)]
    return [u for u in uris if u and not u.startswith("data:") and "://" not in u]


def copy_model(file: Path, objects_dir: Path) -> str:
    """Copies a model file from elsewhere into objects/<its name>/ with the files it refers to.
    Returns its path relative to objects/. A file already inside objects/ stays where it is."""
    file = file.resolve()
    objects_dir = objects_dir.resolve()
    if file.is_relative_to(objects_dir):
        return file.relative_to(objects_dir).as_posix()
    def same(existing: Path) -> bool:  # the same file imported again
        try:
            return existing.read_bytes() == file.read_bytes()
        except OSError:
            return False

    # Its own folder; never over another model of the same name.
    folder = objects_dir / file.stem
    n = 1
    while folder.exists() and not same(folder / file.name):
        n += 1
        folder = objects_dir / f"{file.stem}_{n}"
    folder.mkdir(parents=True, exist_ok=True)
    shutil.copy2(file, folder / file.name)
    from urllib.parse import unquote
    for uri in gltf_dependencies(file):
        src = (file.parent / unquote(uri)).resolve()
        if src.is_file() and src.is_relative_to(file.parent):
            dest = folder / unquote(uri)
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dest)
    return (folder / file.name).relative_to(objects_dir).as_posix()


# ---- previews and thumbnails ------------------------------------------------------------

def template_bounds(parts: list[dict[str, Any]]) -> list[float]:
    """Roughly what a template made in the editor takes up: each part's unit box, scaled and
    placed (rotations taken as the worst case)."""
    lo, hi = [math.inf] * 3, [-math.inf] * 3
    for part in parts:
        t = part.get("components", {}).get("transform", {})
        p = t.get("position", [0, 0, 0])
        s = t.get("scale", [1, 1, 1])
        r = 0.5 * math.sqrt(sum(x * x for x in s)) if t.get("rotation") else None
        for i in range(3):
            half = r if r is not None else 0.5 * abs(s[i])
            lo[i], hi[i] = min(lo[i], p[i] - half), max(hi[i], p[i] + half)
    if lo[0] == math.inf:
        return [-0.5, -0.5, -0.5, 0.5, 0.5, 0.5]
    return lo + hi


class ObjectRenderer:
    """Draws one object on its own, offscreen, for the Object editor's preview and the
    thumbnails. Without the engine module or a Vulkan driver, render() returns None (and
    error says why)."""

    LIGHT = {"transform": {"rotation": [-45.0, 35.0, 0.0]},
             "light": {"type": "directional", "color": [1.0, 0.98, 0.94], "intensity": 1.0}}
    SETTINGS = {"sky": [0.16, 0.18, 0.22], "ambient": [0.42, 0.43, 0.47]}

    def __init__(self):
        module = engine.module()
        self.view = module.Viewport() if module is not None else None
        self.error = None if self.view is not None else "the engine module isn't available"
        self._size = (0, 0)
        self._sent: dict[str, int] = {}  # content id -> hash of what the view has

    def send(self, content_id: str, data: bytes) -> None:
        """A mesh (.glb) or texture (KTX2) for the view, if it doesn't have it already."""
        if self.view is None or self._sent.get(content_id) == hash(data):
            return
        if data[:4] == b"glTF":
            self.view.set_mesh(content_id, data)
        else:
            self.view.set_texture(content_id, data)
        self._sent[content_id] = hash(data)

    def render(self, object_id: str, parts: list[dict[str, Any]], bounds: list[float],
               width: int, height: int, yaw: float = 35.0, pitch: float = 22.0,
               zoom: float = 1.0):
        """The object as an image (QImage), or None. Its meshes and textures must have been
        sent."""
        from PySide6.QtGui import QImage
        if self.view is None:
            return None
        try:
            if self._size != (width, height):
                self.view.attach_offscreen(width, height)
                self._size = (width, height)
            self.view.clear()
            self.view.set_settings(json.dumps(self.SETTINGS))
            self.view.set_object(object_id, json.dumps(parts))
            self.view.put_entity(json.dumps({"id": "light", "name": "Light",
                                             "components": self.LIGHT}))
            self.view.put_entity(json.dumps({"id": "preview", "name": "Preview",
                                             "components": {"object": {"id": object_id}}}))
            centre = [(bounds[i] + bounds[i + 3]) / 2 for i in range(3)]
            radius = max(0.5 * math.dist(bounds[:3], bounds[3:]), 0.05)
            fov = 40.0
            distance = zoom * radius / math.sin(math.radians(fov / 2)) * 1.05
            y, p = math.radians(yaw), math.radians(pitch)
            eye = [centre[0] + distance * math.cos(p) * math.sin(y),
                   centre[1] + distance * math.sin(p),
                   centre[2] + distance * math.cos(p) * math.cos(y)]
            self.view.set_camera(eye, centre, fov)
            self.view.render()
            pixels = self.view.read_pixels()
        except RuntimeError as e:  # no Vulkan driver, or a part the engine refused
            self.error = str(e)
            self.view = None if "Vulkan" in str(e) else self.view
            return None
        self.error = None
        return QImage(pixels, width, height, QImage.Format.Format_RGBA8888).copy()
