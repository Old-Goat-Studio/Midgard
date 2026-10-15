# SPDX-License-Identifier: Apache-2.0
"""A game's icon in its Windows executable, put there without any Windows tools.

The runtime is built with an icon resource whose images are uncompressed 32-bit DIBs at 16, 24,
32, 48, 64, 128 and 256 pixels (engine/runtime/midgard.ico, made by tools/make_runtime_icon.py).
An uncompressed DIB's size depends only on its width and height, so exporting can overwrite each
image in place with the game's icon at the same size: nothing in the executable moves, and any
editor host (Windows or Linux) can do it.
"""
import struct

from PySide6.QtCore import Qt
from PySide6.QtGui import QImage

from .branding import square

RT_ICON, RT_GROUP_ICON = 3, 14
ICON_SIZES = (16, 24, 32, 48, 64, 128, 256)


class IconError(Exception):
    """Shown to the user."""


def dib(image: QImage, size: int) -> bytes:
    """An icon image as a 32-bit DIB: BITMAPINFOHEADER, BGRA rows bottom-up, then an all-clear
    AND mask (transparency comes from the alpha channel)."""
    scaled = square(image).scaled(size, size, Qt.AspectRatioMode.IgnoreAspectRatio,
                                  Qt.TransformationMode.SmoothTransformation)
    scaled = scaled.convertToFormat(QImage.Format.Format_ARGB32)  # BGRA in memory, little-endian
    mask_row = (size + 31) // 32 * 4
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0,
                         size * size * 4 + mask_row * size, 0, 0, 0, 0)
    rows = b"".join(bytes(scaled.constScanLine(y))[:size * 4] for y in range(size - 1, -1, -1))
    return header + rows + bytes(mask_row * size)


def dib_size(size: int) -> int:
    return 40 + size * size * 4 + (size + 31) // 32 * 4 * size


def ico_dib_bytes(image: QImage, sizes: tuple[int, ...] = ICON_SIZES) -> bytes:
    """A .ico whose entries are DIBs (for the runtime's built-in icon resource)."""
    entries = [(size, dib(image, size)) for size in sizes]
    out = struct.pack("<HHH", 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    for size, data in entries:
        out += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    return out + b"".join(data for _size, data in entries)


# ---- the executable ----------------------------------------------------------------------------
class _PE:
    """Just enough of a PE file to find its resources."""

    def __init__(self, data: bytes):
        if data[:2] != b"MZ":
            raise IconError("not a Windows executable")
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise IconError("not a Windows executable")
        sections, optional_size = struct.unpack_from("<H12xH", data, pe + 6)
        optional = pe + 24
        magic = struct.unpack_from("<H", data, optional)[0]
        directories = optional + (112 if magic == 0x20B else 96)  # PE32+ or PE32
        self.rsrc_rva, self.rsrc_size = struct.unpack_from("<II", data, directories + 2 * 8)
        self.sections = []
        table = optional + optional_size
        for i in range(sections):
            vsize, vaddr, raw_size, raw_ptr = struct.unpack_from("<IIII", data,
                                                                 table + 40 * i + 8)
            self.sections.append((vaddr, max(vsize, raw_size), raw_ptr))
        self.data = data

    def offset(self, rva: int) -> int:
        for vaddr, size, raw in self.sections:
            if vaddr <= rva < vaddr + size:
                return rva - vaddr + raw
        raise IconError("damaged resource table")

    def _entries(self, directory: int) -> list[tuple[int, int, bool]]:
        """(id, offset in the resource section, is a subdirectory) for a resource directory."""
        named, ids = struct.unpack_from("<12xHH", self.data, directory)
        out = []
        for i in range(named + ids):
            name, target = struct.unpack_from("<II", self.data, directory + 16 + 8 * i)
            out.append((name, target & 0x7FFFFFFF, bool(target & 0x80000000)))
        return out

    def resources(self, kind: int) -> dict[int, tuple[int, int]]:
        """Resources of a type: id -> (file offset, size), the first language of each."""
        if not self.rsrc_rva:
            return {}
        base = self.offset(self.rsrc_rva)
        found = {}
        for type_id, type_dir, is_dir in self._entries(base):
            if type_id != kind or not is_dir:
                continue
            for res_id, lang_dir, is_dir2 in self._entries(base + type_dir):
                languages = self._entries(base + lang_dir) if is_dir2 else []
                if not languages:
                    continue
                _lang, entry, _ = languages[0]
                rva, size = struct.unpack_from("<II", self.data, base + entry)
                found[res_id] = (self.offset(rva), size)
        return found


def icon_images(exe: bytes) -> dict[int, tuple[int, int]]:
    """The executable's icon images: size in pixels -> (file offset, length), from its first
    icon group."""
    pe = _PE(exe)
    groups, icons = pe.resources(RT_GROUP_ICON), pe.resources(RT_ICON)
    if not groups:
        return {}
    offset, _length = groups[min(groups)]
    _r, _t, count = struct.unpack_from("<HHH", exe, offset)
    images = {}
    for i in range(count):
        width, _h, _c, _r2, _planes, _bits, _length2, icon_id = struct.unpack_from(
            "<BBBBHHIH", exe, offset + 6 + 14 * i)
        if icon_id in icons:
            images[width or 256] = icons[icon_id]
    return images


def set_icon(exe: bytes, image: QImage) -> bytes:
    """The executable with its icon images replaced by `image`. Raises IconError if it has no
    icon images that can be replaced in place."""
    out = bytearray(exe)
    replaced = 0
    for size, (offset, length) in icon_images(exe).items():
        if length != dib_size(size) or exe[offset:offset + 4] != b"\x28\0\0\0":
            continue  # a compressed (PNG) image: it can't be replaced in place
        out[offset:offset + length] = dib(image, size)
        replaced += 1
    if not replaced:
        raise IconError("the runtime has no icon that can be replaced (build it again)")
    return bytes(out)
