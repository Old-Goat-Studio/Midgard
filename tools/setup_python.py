# SPDX-License-Identifier: Apache-2.0
"""The CPython that games embed (roadmap §1: "Python in games").

    python tools/setup_python.py [--target T]          fetch it into .tools/python/
    python tools/setup_python.py runtime --out DIR      add the game's Python to a runtime folder

Games run their scripts on a CPython of their own, separate from the editor's Python, so every
exported game carries the same interpreter whatever the player has installed. Every platform uses
the same version, pinned here with each download's SHA-256: python-build-standalone's relocatable
builds for Windows and Linux, and python.org's official Android build (Android is an official
platform from Python 3.14).

A runtime folder (the one holding midgard_runtime, or libmain.so on Android) gets the interpreter
library, the compiled standard-library modules and the pure-Python standard library zipped into
one file, without tests, tkinter, IDLE, pip and the like:

    Windows: python314.dll, python3.dll, vcruntime140*.dll    beside the runtime (it links to
             python/python314.zip, python/DLLs/*.pyd            them, and Windows looks there)
    Linux:   python/lib/libpython3.14.so.1.0                    (found through the runtime's rpath)
             python/python314.zip, python/lib/python3.14/lib-dynload/*.so
    Android: libpython3.14.so                                   (goes beside libmain.so in the APK)
             python.zip: python314.zip and lib/python3.14/lib-dynload/*.so (an APK asset that the
             runtime unpacks into the app's storage on first start)
"""
import argparse
import hashlib
import io
import os
import platform
import shutil
import struct
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / ".tools" / "python"
VERSION = "3.14.8"
MINOR = ".".join(VERSION.split(".")[:2])  # "3.14": lib/python3.14, libpython3.14.so
SHORT = MINOR.replace(".", "")  # "314": python314.dll, python314.zip
RELEASE = "20261001"  # python-build-standalone
# target -> (download URL, sha256). Desktop builds are python-build-standalone's stripped
# "install_only" archives (without debug information Linux's libpython is a tenth of the size).
_PBS = ("https://github.com/astral-sh/python-build-standalone/releases/download/"
        f"{RELEASE}/cpython-{VERSION}+{RELEASE}-{{triple}}-install_only_stripped.tar.gz")
BUILDS = {
    "windows-x86_64": (_PBS.format(triple="x86_64-pc-windows-msvc"),
                       "b7ff8b700995379e425b39abce9b087d84a45aa265ea465c0069c2747ea5b2ea"),
    "linux-x86_64": (_PBS.format(triple="x86_64-unknown-linux-gnu"),
                     "b373a4a4e4e70fc05f368c9b53d7738bf37637682b650d96c742805d2da26c32"),
    "linux-arm64": (_PBS.format(triple="aarch64-unknown-linux-gnu"),
                    "4395ae16388f9d3409cba7e20161753ed3359b13d959345415029f874f675162"),
    "android-arm64": (f"https://www.python.org/ftp/python/{VERSION}/"
                      f"python-{VERSION}-aarch64-linux-android.tar.gz",
                      "11324313957da3736f44e90257304d288864321a4dd376ed0cc35a54eacf9a33"),
}

# Not shipped in games: tests, GUI toolkits, packaging and development tools.
SKIP_PACKAGES = {"test", "tests", "idlelib", "tkinter", "turtledemo", "ensurepip", "lib2to3",
                 "venv", "pydoc_data", "site-packages", "__pycache__", "unittest", "doctest"}
# Compiled modules games don't ship: tests, Tk, terminals, network security and databases
# (scripts reach the game's data through the midgard API, not sqlite3), and modules whose
# libraries have licences a game can't carry (GNU readline and gdbm are GPL) or that would need
# more notices (bzip2, xz).
SKIP_MODULES = {"_tkinter", "_ssl", "_hashlib", "_sqlite3", "winsound", "_wmi", "xxlimited",
                "xxlimited_35", "_xxinterpchannels", "_xxsubinterpreters", "_xxinterpqueues",
                "readline", "_curses", "_curses_panel", "_dbm", "_gdbm", "nis", "_bz2", "_lzma"}
SKIP_PREFIXES = ("_test", "_ctypes_test", "_xxtestfuzz", "tcl", "tk", "_tk", "libssl",
                 "libcrypto", "sqlite3")
WINDOWS_DLLS = (f"python{SHORT}.dll", "python3.dll", "vcruntime140.dll", "vcruntime140_1.dll")
ANDROID_LIBRARY = f"libpython{MINOR}.so"
ANDROID_ASSET = "python.zip"


def _wanted(name: str) -> bool:
    stem = name.split(".")[0]
    return stem not in SKIP_MODULES and not stem.startswith(SKIP_PREFIXES)


def _skipped(relative: Path) -> bool:
    return any(part in SKIP_PACKAGES or part.startswith("config-") for part in relative.parts)


def host_target() -> str:
    os_name = "windows" if os.name == "nt" else "linux"
    arch = "arm64" if platform.machine().lower() in ("aarch64", "arm64") else "x86_64"
    return f"{os_name}-{arch}"


def runtime_files(runtime_dir: Path) -> list[str]:
    """What make_runtime added to a runtime folder, relative to it (for templates)."""
    names = [n for n in (*WINDOWS_DLLS, ANDROID_LIBRARY, ANDROID_ASSET)
             if (runtime_dir / n).is_file()]
    return names + (["python"] if (runtime_dir / "python").is_dir() else [])


def install_dir(target: str) -> Path:
    """Where a fetched CPython lives: .tools/python/<version>+<release>-<target>/, holding
    python/ (python-build-standalone) or prefix/ (python.org's Android build)."""
    folder = TOOLS / f"{VERSION}+{RELEASE}-{target}"
    return folder / ("prefix" if target.startswith("android") else "python")


def fetch(target: str) -> Path:
    dest = install_dir(target)
    if (dest.parent / "ok").is_file():
        return dest
    url, sha256 = BUILDS[target]
    archive = TOOLS / url.rsplit("/", 1)[1]
    TOOLS.mkdir(parents=True, exist_ok=True)
    if not archive.is_file():
        print(f"Downloading {url}", flush=True)
        tmp = archive.with_suffix(".part")
        urllib.request.urlretrieve(url, tmp)
        tmp.replace(archive)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != sha256:
        archive.unlink()
        sys.exit(f"{archive.name}: SHA-256 {digest} does not match the pinned {sha256}")
    shutil.rmtree(dest.parent, ignore_errors=True)
    dest.parent.mkdir(parents=True)
    with tarfile.open(archive) as tar:
        if target.startswith("android"):  # only the prefix: not the test app or its tools
            members = [m for m in tar.getmembers() if m.name.startswith("./prefix/")]
            tar.extractall(dest.parent, members=members, filter="data")
        else:
            tar.extractall(dest.parent, filter="data")
    (dest.parent / "ok").write_text(f"{VERSION}+{RELEASE} {url}\n", "utf-8")
    return dest


def _zip_stdlib(lib: Path, out, compression: int = zipfile.ZIP_DEFLATED) -> None:
    with zipfile.ZipFile(out, "w", compression) as z:
        for path in sorted(lib.rglob("*.py")):
            relative = path.relative_to(lib)
            if not _skipped(relative):
                z.write(path, relative.as_posix())


def make_runtime(source: Path, runtime_dir: Path, target: str | None = None) -> Path:
    """Adds the game's Python to a runtime folder (see the module docstring). Returns what was
    written: the python/ folder, or on Android the python.zip asset."""
    if (target or "").startswith("android"):
        return _make_android(source, runtime_dir)
    out = runtime_dir / "python"
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    if (source / f"python{SHORT}.dll").is_file():  # Windows
        for name in WINDOWS_DLLS:
            if (source / name).is_file():
                shutil.copy2(source / name, runtime_dir / name)
        (out / "DLLs").mkdir()
        for path in sorted((source / "DLLs").iterdir()):
            if path.suffix in (".pyd", ".dll") and _wanted(path.name):
                shutil.copy2(path, out / "DLLs" / path.name)
        _zip_stdlib(source / "Lib", out / f"python{SHORT}.zip")
    else:  # Linux
        lib = source / "lib"
        (out / "lib").mkdir()
        # One copy of libpython, under its soname: the name the runtime was linked against.
        library = max((p for p in lib.glob(f"libpython{MINOR}*.so*") if p.is_file()),
                      key=lambda p: p.stat().st_size)
        shutil.copy2(library, out / "lib" / (elf_soname(library) or library.name))
        dynload = lib / f"python{MINOR}" / "lib-dynload"
        out_dynload = out / "lib" / f"python{MINOR}" / "lib-dynload"
        out_dynload.mkdir(parents=True)
        for path in sorted(dynload.iterdir()):
            if _wanted(path.name):
                shutil.copy2(path, out_dynload / path.name)
        _zip_stdlib(lib / f"python{MINOR}", out / f"python{SHORT}.zip")
    (out / "LICENSE-python.txt").write_text(_licence(source), "utf-8")
    return out


def _make_android(prefix: Path, runtime_dir: Path) -> Path:
    """libpython beside libmain.so, and python.zip for the APK's assets."""
    lib = prefix / "lib"
    runtime_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(lib / ANDROID_LIBRARY, runtime_dir / ANDROID_LIBRARY)
    # Android has zlib as a module of its own, which can't load until something is imported:
    # the standard library zip is therefore stored, not deflated (the APK compresses it).
    stdlib = io.BytesIO()
    _zip_stdlib(lib / f"python{MINOR}", stdlib, zipfile.ZIP_STORED)
    asset = runtime_dir / ANDROID_ASSET
    with zipfile.ZipFile(asset, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("VERSION", f"{VERSION}+{hashlib.sha256(stdlib.getvalue()).hexdigest()[:12]}\n")
        z.writestr(f"python{SHORT}.zip", stdlib.getvalue())
        dynload = lib / f"python{MINOR}" / "lib-dynload"
        for path in sorted(dynload.iterdir()):
            if path.suffix == ".so" and _wanted(path.name):
                z.write(path, f"lib/python{MINOR}/lib-dynload/{path.name}")
        z.writestr("LICENSE-python.txt", _licence(prefix))
    return asset


def elf_soname(path: Path) -> str | None:
    """DT_SONAME of a 64-bit little-endian ELF shared library, or None."""
    data = path.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        return None
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize)
                for i in range(shnum)]
    for _name, kind, _flags, _addr, offset, size, link, _info, _align, entsize in sections:
        if kind != 6:  # SHT_DYNAMIC
            continue
        strtab = sections[link][4]
        for i in range(0, size, entsize or 16):
            tag, value = struct.unpack_from("<qQ", data, offset + i)
            if tag == 14:  # DT_SONAME
                end = data.index(b"\0", strtab + value)
                return data[strtab + value:end].decode()
    return None


def _licence(source: Path) -> str:
    for candidate in (source / "LICENSE.txt", source / "Lib" / "LICENSE.txt",
                      source / "lib" / f"python{MINOR}" / "LICENSE.txt"):
        if candidate.is_file():
            return candidate.read_text("utf-8", errors="replace")
    return ("Python Software Foundation License Version 2 (PSF-2.0). "
            "See https://docs.python.org/3/license.html\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", nargs="?", default="fetch",
                        choices=["fetch", "runtime", "where"])
    parser.add_argument("--target", default=host_target(), choices=sorted(BUILDS))
    parser.add_argument("--out", type=Path, help="runtime: the folder holding the runtime")
    args = parser.parse_args()
    source = fetch(args.target)
    if args.command == "runtime":
        if not args.out:
            parser.error("runtime needs --out")
        print(make_runtime(source, args.out, args.target))
    elif args.command == "where":
        print(source)
    else:
        print(f"CPython {VERSION} for {args.target} is at {source}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
