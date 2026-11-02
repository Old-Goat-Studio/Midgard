# SPDX-License-Identifier: Apache-2.0
"""Textures a material uses from the project's files (roadmap §5.5).

A material's texture slots hold content ids. An imported model's own textures are
"obj:<object>/tex<n>" (objects.py); an image file in the project is "tex:<path>" for the
colour slots (base colour, emissive; sRGB) and "tex:<path>?linear" for the others, whose
numbers aren't colours (metal/roughness, normal, occlusion). The path is relative to the
project folder, usually textures/<file>. The engine encodes them as KTX2 with mipmaps, for
the package and for the editor's views.
"""
import logging
import shutil
import threading
from pathlib import Path

from . import content_cache, engine

log = logging.getLogger(__name__)

PREFIX = "tex:"
LINEAR = "?linear"
SLOTS = {"base_color": "Base colour", "metallic_roughness": "Metal / roughness",
         "normal": "Normal", "occlusion": "Occlusion", "emissive": "Emission"}
COLOUR_SLOTS = ("base_color", "emissive")
IMAGE_SUFFIXES = (".png", ".jpg", ".jpeg")


def ref(relative: str, slot: str) -> str:
    """The content id for an image file (relative to the project folder) in a slot."""
    return PREFIX + relative + ("" if slot in COLOUR_SLOTS else LINEAR)


def parse(texture: str) -> tuple[str, bool] | None:
    """(the file relative to the project folder, sRGB) for a "tex:" id; None for others."""
    if not texture.startswith(PREFIX):
        return None
    path = texture[len(PREFIX):]
    linear = path.endswith(LINEAR)
    return (path[:-len(LINEAR)] if linear else path), not linear


def pak_path(texture: str) -> str:
    relative, srgb = parse(texture)
    return f"images/{relative}{'' if srgb else '.linear'}.ktx2"


def describe(texture: str) -> str:
    """How a texture slot reads in the editor."""
    parsed = parse(texture)
    if parsed:
        return parsed[0]
    if texture.startswith("obj:") and "/tex" in texture:
        return "the model's texture " + texture.rsplit("/tex", 1)[1]
    return texture


def material_textures(material: dict) -> list[str]:
    """The texture ids a material component uses."""
    return [t for t in material.get("textures", {}).values() if isinstance(t, str) and t]


def components_textures(components: dict) -> list[str]:
    return material_textures(components.get("material", {}))


def copy_texture(file: Path, project_root: Path) -> str:
    """An image file for a material, as a path relative to the project folder. Files inside
    the project stay where they are; others are copied into textures/ (never over a different
    file of the same name)."""
    file = Path(file).resolve()
    root = project_root.resolve()
    if file.is_relative_to(root):
        return file.relative_to(root).as_posix()
    folder = root / "textures"
    folder.mkdir(exist_ok=True)
    dest, n = folder / file.name, 1
    while dest.exists() and dest.read_bytes() != file.read_bytes():
        n += 1
        dest = folder / f"{file.stem}_{n}{file.suffix}"
    if not dest.exists():
        shutil.copy2(file, dest)
    return dest.relative_to(root).as_posix()


class TextureCache:
    """KTX2 bytes for "tex:" ids, encoded once per change of the file's contents."""

    def __init__(self, size: int = 64):
        self.size = size
        self._items: dict[tuple, bytes] = {}
        self.encodes = 0  # images actually encoded (for tests)
        # (Pictures are encoded on worker threads too: the table is guarded, the encoding,
        # which can take seconds, isn't.)
        self._lock = threading.Lock()

    def _key(self, project_root: Path, texture: str) -> tuple | None:
        parsed = parse(texture)
        if parsed is None:
            return None
        relative, srgb = parsed
        try:
            stat = (project_root / relative).stat()
        except OSError:
            return None
        return (str(project_root / relative), srgb, stat.st_mtime_ns, stat.st_size)

    def cached(self, project_root: Path, texture: str) -> bytes | None:
        """The texture's KTX2 bytes if they are ready now (in memory, or on disk), without
        encoding it: None if it would need encoding first."""
        key = self._key(project_root, texture)
        if key is None:
            return None
        with self._lock:
            if key in self._items:
                return self._items[key]
        relative, srgb = parse(texture)
        file = project_root / relative
        disk = content_cache.DiskCache(content_cache.cache_dir(project_root, "textures"))
        found = disk.get(content_cache.key_of("texture", content_cache.file_hash(file), srgb))
        if found is None or "ktx2" not in found[1]:
            return None
        self._remember(key, found[1]["ktx2"])
        return found[1]["ktx2"]

    def _remember(self, key: tuple, data: bytes) -> None:
        with self._lock:
            if key not in self._items and len(self._items) >= self.size:
                self._items.pop(next(iter(self._items)))
            self._items[key] = data

    def data(self, project_root: Path, texture: str) -> bytes | None:
        """The texture's KTX2 bytes, or None (not a "tex:" id, a missing or unreadable file,
        or no engine module); the reason is logged."""
        parsed = parse(texture)
        module = engine.module()
        if parsed is None or module is None or not hasattr(module, "encode_texture"):
            return None
        relative, srgb = parsed
        file = project_root / relative
        try:
            stat = file.stat()
        except OSError:
            return None
        key = (str(file), srgb, stat.st_mtime_ns, stat.st_size)
        with self._lock:
            ready = self._items.get(key)
        if ready is not None:
            return ready
        # On disk by the file's contents (build/cache/textures/): encoded once.
        disk = content_cache.DiskCache(content_cache.cache_dir(project_root, "textures"))
        content_key = content_cache.key_of("texture", content_cache.file_hash(file), srgb)
        found = disk.get(content_key)
        if found is not None and "ktx2" in found[1]:
            data = found[1]["ktx2"]
        else:
            try:
                data = module.encode_texture(file, srgb)
            except RuntimeError as e:
                log.warning("Texture %s: %s", relative, e)
                return None
            self.encodes += 1
            disk.put(content_key, {}, {"ktx2": data})
        self._remember(key, data)
        return data


CACHE = TextureCache()


def texture_data(project_root: Path, texture: str) -> bytes | None:
    return CACHE.data(project_root, texture)
