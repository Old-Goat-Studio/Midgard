# SPDX-License-Identifier: Apache-2.0
import io
import json
import struct
import zipfile

import pytest

from midgard_editor import android_apk as apk
from midgard_editor import exporter
from midgard_editor.version import VERSION

TYPE_STRING = 0x03


def axml(strings: list[str], attributes: list[tuple[int, int, int, int]], utf8: bool) -> bytes:
    """A minimal compiled manifest: a string pool and one <manifest> element.
    attributes: (name index, raw value index or 0xffffffff, data type, data)."""
    pool = {"start": 0, "header_size": 28, "size": 0, "flags": apk.UTF8_FLAG if utf8 else 0,
            "style_count": 0, "strings_start": 0, "styles_start": 0, "utf8": utf8}
    string_pool = apk._write_pool(strings, pool, b"")
    attrs = b"".join(struct.pack("<IIIHBBI", 0xFFFFFFFF, name, raw, 8, 0, kind, data)
                     for name, raw, kind, data in attributes)
    ext = struct.pack("<IIHHHHHH", 0xFFFFFFFF, strings.index("manifest"), 20, 20,
                      len(attributes), 0, 0, 0)
    element = struct.pack("<HHIII", apk.RES_XML_START_ELEMENT_TYPE, 16, 16 + len(ext) + len(attrs),
                          1, 0xFFFFFFFF) + ext + attrs
    body = string_pool + element
    return struct.pack("<HHI", apk.RES_XML_TYPE, 8, 8 + len(body)) + body


def template_manifest(utf8: bool = False) -> bytes:
    strings = ["package", "versionCode", "label", "versionName", apk.PLACEHOLDER_PACKAGE,
               apk.PLACEHOLDER_TITLE, apk.PLACEHOLDER_VERSION, "manifest"]
    return axml(strings, [(0, 4, TYPE_STRING, 4), (1, 0xFFFFFFFF, apk.TYPE_INT_DEC, 1),
                          (2, 5, TYPE_STRING, 5), (3, 6, TYPE_STRING, 6)], utf8)


@pytest.mark.parametrize("utf8", [False, True])
def test_the_manifest_is_patched(utf8):
    patched = apk.patch_manifest(
        template_manifest(utf8),
        {apk.PLACEHOLDER_PACKAGE: "com.oldgoat.hellocube", apk.PLACEHOLDER_TITLE: "Hellö Cube",
         apk.PLACEHOLDER_VERSION: "1.2.3"},
        {"versionCode": apk.version_code("1.2.3")})
    strings = apk.manifest_strings(patched)
    assert "com.oldgoat.hellocube" in strings and "Hellö Cube" in strings and "1.2.3" in strings
    assert apk.PLACEHOLDER_PACKAGE not in strings
    assert apk.int_attributes(patched, "versionCode") == [1002003]
    assert struct.unpack_from("<I", patched, 4)[0] == len(patched)
    assert len(patched) % 4 == 0


def test_package_names_and_version_codes():
    assert apk.valid_package_name("com.oldgoat.hellocube")
    assert not apk.valid_package_name("hellocube")
    assert not apk.valid_package_name("com.1game")
    assert apk.version_code("0.1.0") == 1000
    assert apk.version_code("2.0") == 2000000
    assert apk.version_code("") == 1


def data_offset(archive: bytes, info: zipfile.ZipInfo) -> int:
    name_len, extra_len = struct.unpack_from("<HH", archive, info.header_offset + 26)
    return info.header_offset + 30 + name_len + extra_len


def test_stored_entries_are_aligned():
    entries = [apk.Entry("AndroidManifest.xml", b"x" * 13), apk.Entry("a", b"yy"),
               apk.Entry("resources.arsc", b"arsc", compress=False),
               apk.Entry("b", b"z", compress=False)]
    archive = apk.write_zip(entries)
    with zipfile.ZipFile(io.BytesIO(archive)) as z:
        for info in z.infolist():
            if info.compress_type == zipfile.ZIP_STORED:
                assert data_offset(archive, info) % 4 == 0
        assert z.read("resources.arsc") == b"arsc"


@pytest.fixture
def key(tmp_path):
    return apk.development_key(tmp_path / "settings")[0]


def test_signing_and_verifying(key, tmp_path):
    unsigned = apk.write_zip([apk.Entry("AndroidManifest.xml", template_manifest()),
                              apk.Entry("resources.arsc", b"arsc", compress=False)])
    signed = apk.sign_v2(unsigned, key)
    assert apk.verify_v2(signed).subject == key.certificate.subject
    with zipfile.ZipFile(io.BytesIO(signed)) as z:  # still an ordinary zip
        assert z.read("resources.arsc") == b"arsc"
    tampered = bytearray(signed)
    tampered[40] ^= 0xFF
    with pytest.raises(apk.ApkError):
        apk.verify_v2(bytes(tampered))
    with pytest.raises(apk.ApkError, match="not signed"):
        apk.verify_v2(unsigned)


def test_the_development_key_is_kept(tmp_path):
    first, folder = apk.development_key(tmp_path / "settings")
    second, _ = apk.development_key(tmp_path / "settings")
    assert first.certificate == second.certificate
    assert (folder / "midgard-dev-key.pem").is_file()


def _png(size: int, colour: str) -> bytes:
    from PySide6.QtGui import QColor, QImage

    from midgard_editor.branding import png_bytes

    image = QImage(size, size, QImage.Format.Format_ARGB32)
    image.fill(QColor(colour))
    return png_bytes(image)


def android_template(tmp_path):
    base = io.BytesIO()
    with zipfile.ZipFile(base, "w") as z:
        z.writestr("AndroidManifest.xml", template_manifest())
        z.writestr("resources.arsc", b"arsc")
        z.writestr("classes.dex", b"dex")
        z.writestr("lib/arm64-v8a/libmain.so", b"engine")
        z.writestr("res/mipmap-xxxhdpi-v4/ic_launcher.png", _png(192, "#2e8b57"))
    archive = tmp_path / f"midgard-runtime-{VERSION}-android-arm64.zip"
    with zipfile.ZipFile(archive, "w") as z:
        z.writestr("template.json", json.dumps({
            "format": "midgard-runtime-template", "format_version": 1,
            "target": {"os": "android", "arch": "arm64"}, "runtime": "base.apk",
            "engine": VERSION, "engine_abi": 0}))
        z.writestr("base.apk", base.getvalue())
        z.writestr("THIRD_PARTY_NOTICES.txt", "notices")
        z.writestr("LICENSE", "Apache")
        z.writestr("NOTICE", "Old Goat Studio")
    return archive


@pytest.fixture(autouse=True)
def no_local_engine(tmp_path, monkeypatch):
    monkeypatch.setattr(exporter, "BUNDLE_DIR", tmp_path / "no_bundle")


def test_android_export(project, tmp_path):
    settings = tmp_path / "settings"
    exporter.install_template(android_template(tmp_path), settings)
    archive = exporter.export(project, "android-arm64", tmp_path / "out", settings)
    assert archive.name == "HelloCube-0.1.0-android-arm64.apk"
    signed = archive.read_bytes()
    apk.verify_v2(signed)
    with zipfile.ZipFile(archive) as z:
        names = set(z.namelist())
        strings = apk.manifest_strings(z.read("AndroidManifest.xml"))
        manifest = json.loads(z.read("assets/data/manifest.json"))
        arsc = z.getinfo("resources.arsc")
        assert arsc.compress_type == zipfile.ZIP_STORED and data_offset(signed, arsc) % 4 == 0
    assert {"classes.dex", "lib/arm64-v8a/libmain.so", "assets/data/HelloCube.db",
            "assets/licenses/THIRD_PARTY_NOTICES.txt",
            "assets/licenses/MIDGARD_NOTICE.txt"} <= names
    assert "com.example.hellocube" in strings and "Hello Cube" in strings
    assert manifest["target"] == {"os": "android", "arch": "arm64", "role": "game"}


def test_android_package_comes_from_the_project(project, tmp_path):
    assert exporter.android_package(project) == "com.example.hellocube"
    project.set_config("android_package", "com.oldgoat.cube")
    assert exporter.android_package(project) == "com.oldgoat.cube"


def test_command_line_export(project, tmp_path, capsys):
    from midgard_editor.__main__ import main

    code = main(["export", str(project.config_path), "--target", "android-arm64",
                 "--out", str(tmp_path / "out"), "--template", str(android_template(tmp_path)),
                 "--settings", str(tmp_path / "settings")])
    assert code == 0, capsys.readouterr().err
    apk.verify_v2((tmp_path / "out" / "HelloCube-0.1.0-android-arm64.apk").read_bytes())


def test_the_export_dialog_asks_for_the_android_package(project, settings, qtbot):
    from midgard_editor.export_dialog import ExportDialog

    dialog = ExportDialog(project, settings)
    qtbot.addWidget(dialog)
    dialog.target.setCurrentIndex(dialog.target.findData("windows-x86_64"))
    assert not dialog.form.isRowVisible(dialog.package)
    dialog.target.setCurrentIndex(dialog.target.findData("android-arm64"))
    assert dialog.form.isRowVisible(dialog.package)
    assert dialog.package.text() == "com.example.hellocube"


def test_the_android_launcher_icon_is_the_games(project, tmp_path):
    from PySide6.QtGui import QImage

    from midgard_editor import branding
    from midgard_editor.undo import SetConfigCommand

    (tmp_path / "icon.png").write_bytes(_png(512, "#c0392b"))
    value, _ = branding.import_image(project, "icon", tmp_path / "icon.png")
    project.undo_stack.push(SetConfigCommand(project, "branding", value, "icon"))
    settings = tmp_path / "settings"
    exporter.install_template(android_template(tmp_path), settings)
    archive = exporter.export(project, "android-arm64", tmp_path / "out", settings)
    with zipfile.ZipFile(archive) as z:
        icon = QImage.fromData(z.read("res/mipmap-xxxhdpi-v4/ic_launcher.png"))
        assert "assets/data/icon.png" in z.namelist()
    assert icon.width() == 192 and icon.pixelColor(96, 96).name() == "#c0392b"
