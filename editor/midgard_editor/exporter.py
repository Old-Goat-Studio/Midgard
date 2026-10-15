# SPDX-License-Identifier: Apache-2.0
"""Build ▸ Export: a finished game for one target, made from a runtime template plus the cooked
package (roadmap §2, "Target-only exports"). Nothing is compiled, so any editor host can export
to any target it has a template for.

    Windows:  <Game>-<version>-windows-x86_64.zip  ->  <Game>/<Game>.exe, data/, notices, README
    Linux:    <Game>-<version>-linux-x86_64.tar.gz ->  <Game>/<Game> (0755), data/, .desktop, ...

Desktop games also carry their Python (the template's runtime_files: python313.dll and python/
on Windows, python/ on Linux) for game scripts.
    Android:  <Game>-<version>-android-arm64.apk   ->  signed APK, the package in assets/data/

Templates come from Midgard's CI (tools/make_template.py) and are installed into the editor's
settings folder: <settings>/templates/<engine version>/<os>-<arch>/.
"""
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile
from dataclasses import dataclass
from pathlib import Path

from . import BUNDLE_DIR, android_apk
from .cook import cook
from .project import Project
from .version import VERSION

TARGETS = {
    "windows-x86_64": {"os": "windows", "arch": "x86_64", "label": "Windows (x86_64)",
                       "preset": "windows-release", "runtime": "midgard_runtime.exe"},
    "linux-x86_64": {"os": "linux", "arch": "x86_64", "label": "Linux (x86_64)",
                     "preset": "linux-release", "runtime": "midgard_runtime"},
    "android-arm64": {"os": "android", "arch": "arm64", "label": "Android (arm64)",
                      "preset": "android-arm64", "runtime": "base.apk"},
}

# The game's Python beside a desktop runtime (tools/setup_python.py).
PYTHON_FILES = ("python313.dll", "python3.dll", "vcruntime140.dll", "vcruntime140_1.dll", "python")

README_WINDOWS = """{title}

Run {name}.exe to play.

Windows may show a SmartScreen message ("Windows protected your PC") the first time, because
this build is not code-signed yet. Choose "More info", then "Run anyway".

Made with Midgard. See THIRD_PARTY_NOTICES.txt for the components it includes.
"""
README_LINUX = """{title}

Run ./{name} to play. A Vulkan driver is required (for example mesa-vulkan-drivers).
{name}.desktop is a desktop launcher: edit its Exec and Icon paths if you move the game.

Made with Midgard. See THIRD_PARTY_NOTICES.txt for the components it includes.
"""


class ExportError(Exception):
    """The game can't be exported. The message is shown to the user."""


@dataclass
class Template:
    folder: Path
    runtime: Path
    info: dict


def templates_root(settings_dir: Path) -> Path:
    return settings_dir / "templates"


def install_template(archive: Path, settings_dir: Path) -> Path:
    """Unpack a midgard-runtime-*.zip into the templates folder. Returns its folder."""
    try:
        with zipfile.ZipFile(archive) as z:
            info = json.loads(z.read("template.json"))
            if info.get("format") != "midgard-runtime-template":
                raise ExportError(f"{archive.name} is not a Midgard runtime template.")
            target = f"{info['target']['os']}-{info['target']['arch']}"
            dest = templates_root(settings_dir) / info["engine"] / target
            if dest.exists():
                shutil.rmtree(dest)
            dest.mkdir(parents=True)
            z.extractall(dest)
    except (zipfile.BadZipFile, KeyError, ValueError) as e:
        raise ExportError(f"{archive.name} is not a valid runtime template: {e}") from e
    return dest


def find_template(target: str, settings_dir: Path) -> Template | None:
    """An installed template for this editor's version, or this machine's own engine build."""
    spec = TARGETS[target]
    installed = templates_root(settings_dir) / VERSION / target
    if (installed / "template.json").is_file():
        info = json.loads((installed / "template.json").read_text("utf-8"))
        return Template(installed, installed / info["runtime"], info)
    host = f"{'windows' if os.name == 'nt' else 'linux'}-x86_64"
    if target == host:
        for runtime in (BUNDLE_DIR / "engine" / spec["runtime"],  # packaged editor
                        BUNDLE_DIR / "build" / spec["preset"] / "engine" / spec["runtime"]):
            if runtime.is_file():
                files = [n for n in PYTHON_FILES if (runtime.parent / n).exists()]
                return Template(runtime.parent, runtime,
                                {"engine": VERSION, "runtime": spec["runtime"],
                                 "runtime_files": files,
                                 "target": {"os": spec["os"], "arch": spec["arch"]}})
    return None


def android_package(project: Project) -> str:
    """The game's Android package name (application ID), from the project config."""
    slug = re.sub(r"[^a-z0-9_]", "", project.name.lower())
    if not slug[:1].isalpha():
        slug = "game" + slug
    return project.config.get("android_package") or f"com.example.{slug}"


def _notices(template: Template) -> str:
    bundled = template.folder / "THIRD_PARTY_NOTICES.txt"
    if bundled.is_file():
        return bundled.read_text("utf-8")
    tool = BUNDLE_DIR / "tools" / "licenses.py"
    return subprocess.run([sys.executable, str(tool), "notices", "--role", "runtime"],
                          capture_output=True, text=True, check=True).stdout


def export(project: Project, target: str, out_dir: Path, settings_dir: Path) -> Path:
    """Export the project for a target. Returns the archive; the unpacked game is beside it."""
    spec = TARGETS[target]
    template = find_template(target, settings_dir)
    if template is None:
        raise ExportError(f"No runtime template for {spec['label']} is installed. Download "
                          f"midgard-runtime-{VERSION}-{target}.zip from Midgard's CI or "
                          "releases and choose Install Template…")
    name = project.name
    version = project.config.get("game_version", "0.1.0")
    if spec["os"] == "android":
        return _export_android(project, template, out_dir, settings_dir, target, version)
    game_dir = out_dir / f"{name}-{target}" / name
    if game_dir.parent.exists():
        shutil.rmtree(game_dir.parent)
    game_dir.mkdir(parents=True)

    cook(project, game_dir / "data", {"os": spec["os"], "arch": spec["arch"], "role": "game"})
    exe = game_dir / (f"{name}.exe" if spec["os"] == "windows" else name)
    shutil.copy2(template.runtime, exe)
    for extra in template.info.get("runtime_files", []):
        source = template.folder / extra
        if source.is_dir():
            shutil.copytree(source, game_dir / extra)
        elif source.is_file():
            shutil.copy2(source, game_dir / extra)
    (game_dir / "THIRD_PARTY_NOTICES.txt").write_text(_notices(template), "utf-8")
    for doc in ("LICENSE", "NOTICE"):
        src = template.folder / doc if (template.folder / doc).is_file() else BUNDLE_DIR / doc
        if src.is_file():
            shutil.copy2(src, game_dir / f"MIDGARD_{doc}.txt")
    readme = README_WINDOWS if spec["os"] == "windows" else README_LINUX
    (game_dir / "README.txt").write_text(readme.format(title=project.game_title, name=name),
                                         "utf-8")
    if spec["os"] == "linux":
        (game_dir / f"{name}.desktop").write_text(
            "[Desktop Entry]\nType=Application\n"
            f"Name={project.game_title}\nExec=./{name}\nTerminal=false\nCategories=Game;\n",
            "utf-8")

    stem = f"{name}-{version}-{target}"
    if spec["os"] == "windows":
        archive = out_dir / f"{stem}.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
            for path in sorted(game_dir.rglob("*")):
                if path.is_file():
                    z.write(path, Path(name) / path.relative_to(game_dir))
    else:
        archive = out_dir / f"{stem}.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            for path in sorted(game_dir.rglob("*")):
                info = tar.gettarinfo(str(path), str(Path(name) / path.relative_to(game_dir)))
                # Set permissions explicitly, so a Windows host still produces a runnable game.
                info.mode = 0o755 if (path.is_dir() or path == exe) else 0o644
                info.uid = info.gid = 0
                info.uname = info.gname = ""
                if path.is_file():
                    with path.open("rb") as f:
                        tar.addfile(info, io.BytesIO(f.read()))
                else:
                    tar.addfile(info)
    return archive


def _export_android(project: Project, template: Template, out_dir: Path, settings_dir: Path,
                    target: str, version: str) -> Path:
    spec = TARGETS[target]
    work = out_dir / f"{project.name}-{target}"
    if work.exists():
        shutil.rmtree(work)
    cook(project, work / "data", {"os": spec["os"], "arch": spec["arch"], "role": "game"})
    assets = {"licenses/THIRD_PARTY_NOTICES.txt": _notices(template).encode("utf-8")}
    for doc in ("LICENSE", "NOTICE"):
        src = template.folder / doc if (template.folder / doc).is_file() else BUNDLE_DIR / doc
        if src.is_file():
            assets[f"licenses/MIDGARD_{doc}.txt"] = src.read_bytes()
    key, _ = android_apk.development_key(settings_dir)
    try:
        apk = android_apk.build_apk(template.runtime, work / "data", assets,
                                    android_package(project), project.game_title, version, key)
    except android_apk.ApkError as e:
        raise ExportError(f"The Android export failed: {e}") from e
    archive = out_dir / f"{project.name}-{version}-{target}.apk"
    archive.write_bytes(apk)
    return archive

