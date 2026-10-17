# SPDX-License-Identifier: Apache-2.0
"""Package the Midgard editor (a one-folder PyInstaller build) following the About Scheme.

    python tools/package.py                 bump the build number, build the engine and the editor
    python tools/package.py --skip-engine   editor only (the About box then says "Engine not found")
    python tools/package.py --no-bump       keep the current build number (CI)

The result is build/dist/Midgard/. The build number is bumped first; if anything fails, the
previous editor/midgard_editor/version.py is put back, so a failed build never uses up a number.
One-folder mode keeps the Qt libraries as separate files, as the LGPL requires.
"""
import argparse
import os
import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VERSION_PY = ROOT / "editor" / "midgard_editor" / "version.py"
DIST = ROOT / "build" / "dist"
APP = DIST / "Midgard"
EXE = ".exe" if os.name == "nt" else ""


def engine_preset() -> str:
    if os.name == "nt":
        return "windows-release"
    return "linux-arm64-release" if platform.machine().lower() in ("aarch64", "arm64") \
        else "linux-release"


def run(*cmd: str | Path) -> None:
    print("+", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], cwd=ROOT, check=True)


def build_engine() -> None:
    if os.name == "nt" and shutil.which("cl") is None:
        sys.exit("The engine build needs the MSVC compiler: run this from a Visual Studio "
                 "developer prompt, or pass --skip-engine.")
    preset = engine_preset()
    run(sys.executable, ROOT / "tools" / "setup_vcpkg.py")
    run("cmake", "--preset", preset)
    run("cmake", "--build", "--preset", preset)


def build_editor() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        # Record this project so the packaged editor uses its live Themes folder (Theme Scheme).
        source_info = Path(tmp) / "source_dir.txt"
        source_info.write_text(str(ROOT), encoding="utf-8")

        def data(src: Path, dest: str) -> list[str]:
            return ["--add-data", f"{src}{os.pathsep}{dest}"]

        run(sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--windowed",
            "--onedir", "--name", "Midgard", "--icon", ROOT / "midgard512.png",
            "--distpath", DIST, "--workpath", ROOT / "build" / "pyinstaller",
            "--specpath", ROOT / "build" / "pyinstaller",
            "--paths", ROOT / "editor",
            "--collect-all", "PySide6QtAds",
            *data(ROOT / "Themes", "Themes"),
            *data(ROOT / "schema", "schema"),
            *data(ROOT / "midgard512.png", "."),
            *data(ROOT / "icons", "icons"),
            *data(source_info, "."),
            ROOT / "tools" / "midgard_launcher.py")

    internal = APP / "_internal"
    engine_dir = ROOT / "build" / engine_preset() / "engine"
    for program in ("midgard_runtime", "midgard_server"):
        built = engine_dir / f"{program}{EXE}"
        if built.is_file():
            (internal / "engine").mkdir(exist_ok=True)
            shutil.copy2(built, internal / "engine" / built.name)
    # The game's Python beside the runtime (tools/setup_python.py).
    sys.path.insert(0, str(ROOT / "tools"))
    from setup_python import runtime_files

    for name in runtime_files(engine_dir) if (internal / "engine").is_dir() else []:
        source = engine_dir / name
        if source.is_dir():
            shutil.copytree(source, internal / "engine" / name, dirs_exist_ok=True)
        else:
            shutil.copy2(source, internal / "engine" / name)
    # The _midgard engine module, built for this Python (the one PyInstaller bundles).
    for module in engine_dir.glob("_midgard*.pyd" if os.name == "nt" else "_midgard*.so"):
        (internal / "engine").mkdir(exist_ok=True)
        shutil.copy2(module, internal / "engine" / module.name)
    if (internal / "engine").is_dir():  # the host's runtime template, for Build ▸ Export
        run(sys.executable, ROOT / "tools" / "licenses.py", "notices", "--role", "runtime",
            "--out", internal / "engine" / "THIRD_PARTY_NOTICES.txt")
        for name in ("LICENSE", "NOTICE"):
            shutil.copy2(ROOT / name, internal / "engine" / name)
    for name in ("LICENSE", "NOTICE"):
        shutil.copy2(ROOT / name, APP / name)
    run(sys.executable, ROOT / "tools" / "licenses.py", "notices", "--role", "editor",
        "--out", APP / "THIRD_PARTY_NOTICES.txt")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--skip-engine", action="store_true")
    parser.add_argument("--no-bump", action="store_true")
    args = parser.parse_args()

    backup = VERSION_PY.with_name("version.py.bak")
    if not args.no_bump:
        shutil.copy2(VERSION_PY, backup)
    try:
        if not args.no_bump:
            run(sys.executable, ROOT / "bump_build.py")
        if not args.skip_engine:
            build_engine()
        build_editor()
    except BaseException:
        if backup.exists():
            shutil.move(backup, VERSION_PY)  # a failed build never uses up a build number
        raise
    backup.unlink(missing_ok=True)
    print(f"Packaged editor: {APP / ('Midgard' + EXE)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
