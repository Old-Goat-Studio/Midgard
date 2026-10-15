# SPDX-License-Identifier: Apache-2.0
"""The game's icon in packages and exports."""
import json
import struct
import tarfile
import zipfile

import pytest
from PySide6.QtGui import QColor, QImage

from midgard_editor import BUNDLE_DIR, branding, exporter, winicon
from midgard_editor.cook import cook
from midgard_editor.undo import SetConfigCommand
from midgard_editor.version import VERSION

RUNTIME = BUNDLE_DIR / "build" / "windows-release" / "engine" / "midgard_runtime.exe"


@pytest.fixture
def red_icon(project, tmp_path, qapp):
    image = QImage(512, 512, QImage.Format.Format_ARGB32)
    image.fill(QColor("#c0392b"))
    image.save(str(tmp_path / "icon.png"))
    value, _ = branding.import_image(project, "icon", tmp_path / "icon.png")
    project.undo_stack.push(SetConfigCommand(project, "branding", value, "icon"))
    return project


def test_the_built_in_icon_can_be_replaced_in_place():
    data = (BUNDLE_DIR / "engine" / "runtime" / "midgard.ico").read_bytes()
    _r, _t, count = struct.unpack_from("<HHH", data)
    sizes = []
    for i in range(count):
        width, _h, _c, _r2, _p, bits, length, offset = struct.unpack_from("<BBBBHHII", data,
                                                                          6 + 16 * i)
        size = width or 256
        assert bits == 32 and length == winicon.dib_size(size)
        assert data[offset:offset + 4] == b"\x28\0\0\0"  # a DIB, not PNG
        sizes.append(size)
    assert sizes == list(winicon.ICON_SIZES)


def test_the_package_carries_the_icon(red_icon, tmp_path):
    package = cook(red_icon, tmp_path / "pkg").out_dir
    icon = QImage(str(package / "icon.png"))
    assert icon.width() == 256 and icon.pixelColor(128, 128).name() == "#c0392b"
    red_icon.undo_stack.undo()  # no icon: none in the package
    assert not (cook(red_icon, tmp_path / "pkg").out_dir / "icon.png").exists()


def test_linux_exports_get_a_launcher_icon(red_icon, tmp_path, monkeypatch):
    monkeypatch.setattr(exporter, "BUNDLE_DIR", tmp_path / "no_bundle")
    archive = tmp_path / f"midgard-runtime-{VERSION}-linux-x86_64.zip"
    with zipfile.ZipFile(archive, "w") as z:
        z.writestr("template.json", json.dumps({
            "format": "midgard-runtime-template", "format_version": 1,
            "target": {"os": "linux", "arch": "x86_64"}, "runtime": "midgard_runtime",
            "engine": VERSION, "engine_abi": 0}))
        z.writestr("midgard_runtime", b"fake runtime")
        z.writestr("THIRD_PARTY_NOTICES.txt", "notices")
    exporter.install_template(archive, tmp_path / "settings")
    result = exporter.export(red_icon, "linux-x86_64", tmp_path / "out", tmp_path / "settings")
    with tarfile.open(result) as tar:
        names = set(tar.getnames())
        desktop = tar.extractfile("HelloCube/HelloCube.desktop").read().decode()
    assert {"HelloCube/HelloCube.png", "HelloCube/data/icon.png"} <= names
    assert "Icon=./HelloCube.png" in desktop


@pytest.mark.skipif(not RUNTIME.is_file(), reason="the Windows runtime is not built")
def test_windows_exports_get_the_icon_in_the_executable(red_icon, tmp_path, monkeypatch):
    monkeypatch.setattr(exporter, "BUNDLE_DIR", BUNDLE_DIR)  # this machine's runtime
    exporter.export(red_icon, "windows-x86_64", tmp_path / "out", tmp_path / "settings")
    exe = (tmp_path / "out" / "HelloCube-windows-x86_64" / "HelloCube" / "HelloCube.exe")
    data = exe.read_bytes()
    assert len(data) == RUNTIME.stat().st_size  # replaced in place: nothing moved
    offset, _length = winicon.icon_images(data)[32]
    assert data[offset + 40:offset + 44] == bytes([0x2B, 0x39, 0xC0, 0xFF])  # BGRA #c0392b


def test_the_android_launcher_icon(qapp):
    from midgard_editor import android_apk

    base = QImage(192, 192, QImage.Format.Format_ARGB32)
    base.fill(QColor("#2e8b57"))
    red = QImage(300, 300, QImage.Format.Format_ARGB32)
    red.fill(QColor("#c0392b"))
    out = android_apk.launcher_icon(branding.png_bytes(red), 192)
    image = QImage.fromData(out)
    assert image.width() == 192 and image.pixelColor(96, 96).name() == "#c0392b"
    with pytest.raises(android_apk.ApkError):
        android_apk.launcher_icon(b"not a png", 192)
