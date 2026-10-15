# SPDX-License-Identifier: Apache-2.0
import json
import tarfile
import zipfile

import pytest

from midgard_editor import exporter
from midgard_editor.version import VERSION


@pytest.fixture(autouse=True)
def no_local_engine(tmp_path, monkeypatch):
    """Export only from installed templates, never from this machine's own engine build."""
    monkeypatch.setattr(exporter, "BUNDLE_DIR", tmp_path / "no_bundle")


def fake_template(tmp_path, os_name: str, version: str = VERSION):
    runtime = "midgard_runtime.exe" if os_name == "windows" else "midgard_runtime"
    archive = tmp_path / f"midgard-runtime-{version}-{os_name}-x86_64.zip"
    with zipfile.ZipFile(archive, "w") as z:
        z.writestr("template.json", json.dumps({
            "format": "midgard-runtime-template", "format_version": 1,
            "target": {"os": os_name, "arch": "x86_64"}, "runtime": runtime,
            "engine": version, "engine_abi": 0}))
        z.writestr(runtime, b"fake runtime")
        z.writestr("THIRD_PARTY_NOTICES.txt", "notices")
        z.writestr("LICENSE", "Apache")
        z.writestr("NOTICE", "Old Goat Studio")
    return archive


def test_installed_templates_are_found(tmp_path):
    settings = tmp_path / "settings"
    assert exporter.find_template("linux-x86_64", settings) is None
    folder = exporter.install_template(fake_template(tmp_path, "linux"), settings)
    assert folder == settings / "templates" / VERSION / "linux-x86_64"
    template = exporter.find_template("linux-x86_64", settings)
    assert template is not None and template.runtime.read_bytes() == b"fake runtime"


def test_a_template_for_another_version_is_not_used(tmp_path):
    settings = tmp_path / "settings"
    exporter.install_template(fake_template(tmp_path, "linux", "9.9.9"), settings)
    assert exporter.find_template("linux-x86_64", settings) is None


def test_bad_archives_are_refused(tmp_path):
    bad = tmp_path / "bad.zip"
    bad.write_bytes(b"not a zip")
    with pytest.raises(exporter.ExportError):
        exporter.install_template(bad, tmp_path / "settings")


def test_windows_export(project, tmp_path):
    settings = tmp_path / "settings"
    exporter.install_template(fake_template(tmp_path, "windows"), settings)
    archive = exporter.export(project, "windows-x86_64", tmp_path / "out", settings)
    assert archive.name == "HelloCube-0.1.0-windows-x86_64.zip"
    with zipfile.ZipFile(archive) as z:
        names = set(z.namelist())
        manifest = json.loads(z.read("HelloCube/data/manifest.json"))
        readme = z.read("HelloCube/README.txt").decode()
    assert {"HelloCube/HelloCube.exe", "HelloCube/data/game.pak", "HelloCube/data/HelloCube.db",
            "HelloCube/THIRD_PARTY_NOTICES.txt", "HelloCube/MIDGARD_NOTICE.txt"} <= names
    assert manifest["target"] == {"os": "windows", "arch": "x86_64", "role": "game"}
    assert "SmartScreen" in readme
    assert (tmp_path / "out" / "HelloCube-windows-x86_64" / "HelloCube" / "HelloCube.exe").is_file()


def test_linux_export_keeps_the_executable_bit(project, tmp_path):
    settings = tmp_path / "settings"
    exporter.install_template(fake_template(tmp_path, "linux"), settings)
    archive = exporter.export(project, "linux-x86_64", tmp_path / "out", settings)
    assert archive.name == "HelloCube-0.1.0-linux-x86_64.tar.gz"
    with tarfile.open(archive) as tar:
        members = {m.name: m for m in tar.getmembers()}
        manifest = json.loads(tar.extractfile("HelloCube/data/manifest.json").read())
        desktop = tar.extractfile("HelloCube/HelloCube.desktop").read().decode()
    assert members["HelloCube/HelloCube"].mode == 0o755
    assert members["HelloCube/data/manifest.json"].mode == 0o644
    assert manifest["target"]["os"] == "linux"
    assert "Exec=./HelloCube" in desktop


def test_missing_template_is_explained(project, tmp_path):
    with pytest.raises(exporter.ExportError, match="Install Template"):
        exporter.export(project, "linux-x86_64", tmp_path / "out", tmp_path / "settings")
