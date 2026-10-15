# SPDX-License-Identifier: Apache-2.0
"""The game's images: a logo, and an application icon (also written as a Windows .ico).

Chosen images (PNG, JPEG or BMP) are copied into the project's branding/ folder as PNG, under
names that include a hash of their contents, so undoing a change still finds the old file. The
icon is made square (centred on a transparent square) and also saved as a multi-size .ico. Their
paths are kept in {project}.config under "branding":

    "branding": {"logo": "branding/logo-1a2b3c4d.png",
                 "icon": "branding/icon-5e6f7a8b.png", "icon_ico": "branding/icon-5e6f7a8b.ico"}
"""
import hashlib
import struct
from pathlib import Path

from PySide6.QtCore import QBuffer, QByteArray, QIODevice, Qt
from PySide6.QtGui import QColor, QImage, QPainter

from .project import Project

FOLDER = "branding"
KINDS = {"logo": "Logo", "icon": "Application icon"}
IMAGE_FILTER = "Images (*.png *.jpg *.jpeg *.bmp)"
ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)
ICON_SMALLEST = 256  # smaller icons are accepted, with a warning: they blur when scaled up
LOGO_LARGEST = 4096  # larger logos are scaled down to this


class BrandingError(Exception):
    """Shown to the user."""


def png_bytes(image: QImage) -> bytes:
    data = QByteArray()
    buffer = QBuffer(data)
    buffer.open(QIODevice.OpenModeFlag.WriteOnly)
    image.save(buffer, "PNG")
    buffer.close()
    return bytes(data)


def square(image: QImage) -> QImage:
    """The image centred on a transparent square."""
    side = max(image.width(), image.height())
    if image.width() == image.height():
        return image.convertToFormat(QImage.Format.Format_ARGB32)
    out = QImage(side, side, QImage.Format.Format_ARGB32)
    out.fill(QColor(0, 0, 0, 0))
    painter = QPainter(out)
    painter.drawImage((side - image.width()) // 2, (side - image.height()) // 2, image)
    painter.end()
    return out


def ico_bytes(image: QImage, sizes: tuple[int, ...] = ICO_SIZES) -> bytes:
    """A Windows .ico holding the image at each size, every entry stored as PNG (which Windows
    has read since Vista)."""
    image = square(image)
    entries = []
    for size in sizes:
        scaled = image.scaled(size, size, Qt.AspectRatioMode.IgnoreAspectRatio,
                              Qt.TransformationMode.SmoothTransformation)
        entries.append((size, png_bytes(scaled)))
    header = struct.pack("<HHH", 0, 1, len(entries))  # reserved, type 1 = icon, count
    offset = 6 + 16 * len(entries)
    directory, images = b"", b""
    for size, data in entries:
        # width, height (0 means 256), colours, reserved, planes, bits per pixel, size, offset
        directory += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data),
                                 offset)
        images += data
        offset += len(data)
    return header + directory + images


def load(source: Path) -> QImage:
    image = QImage(str(source))
    if image.isNull():
        raise BrandingError(f"{source.name} can't be read as an image (use PNG, JPEG or BMP).")
    return image


def _store(project: Project, name: str, data: bytes, suffix: str) -> str:
    digest = hashlib.sha256(data).hexdigest()[:8]
    relative = f"{FOLDER}/{name}-{digest}{suffix}"
    target = project.root / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.is_file():
        target.write_bytes(data)
    return relative


def import_image(project: Project, kind: str, source: Path) -> tuple[dict, list[str]]:
    """Copies an image into branding/ (and, for the icon, writes its .ico). Returns the new
    "branding" config value and any warnings. Use SetConfigCommand to apply it."""
    if kind not in KINDS:
        raise BrandingError(f"Unknown image kind {kind}.")
    image = load(Path(source))
    warnings: list[str] = []
    branding = dict(project.config.get("branding") or {})
    if kind == "logo":
        if max(image.width(), image.height()) > LOGO_LARGEST:
            image = image.scaled(LOGO_LARGEST, LOGO_LARGEST, Qt.AspectRatioMode.KeepAspectRatio,
                                 Qt.TransformationMode.SmoothTransformation)
        branding["logo"] = _store(project, "logo", png_bytes(image), ".png")
    else:
        if image.width() != image.height():
            warnings.append(f"The icon is {image.width()} × {image.height()}; it was centred on "
                            "a transparent square.")
        image = square(image)
        if image.width() < ICON_SMALLEST:
            warnings.append(f"The icon is only {image.width()} pixels across; "
                            f"{ICON_SMALLEST} or more looks sharper at large sizes.")
        branding["icon"] = _store(project, "icon", png_bytes(image), ".png")
        branding["icon_ico"] = _store(project, "icon", ico_bytes(image), ".ico")
    return branding, warnings


def removed(project: Project, kind: str) -> dict:
    """The "branding" config value without this image (its files stay, for Undo)."""
    branding = dict(project.config.get("branding") or {})
    branding.pop(kind, None)
    if kind == "icon":
        branding.pop("icon_ico", None)
    return branding


def path(project: Project, key: str) -> Path | None:
    """The file for "logo", "icon" or "icon_ico", if the project has one."""
    relative = (project.config.get("branding") or {}).get(key)
    if not relative:
        return None
    file = project.root / relative
    return file if file.is_file() else None
