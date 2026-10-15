# SPDX-License-Identifier: Apache-2.0
"""The CPython that games embed (roadmap §1: "Python in games").

    python tools/setup_python.py                    fetch it for this machine into .tools/python/
    python tools/setup_python.py runtime --out DIR  add the game's Python to a runtime folder

Games run their scripts on a CPython of their own, separate from the editor's Python, so every
exported game carries the same interpreter whatever the player has installed. It comes from
python-build-standalone (relocatable builds, pinned here by release and SHA-256).

A runtime folder (the one holding midgard_runtime) gets the interpreter library, the compiled
standard-library modules and the pure-Python standard library zipped into one file, without
tests, tkinter, IDLE, pip and the like:

    Windows: python313.dll, python3.dll, vcruntime140*.dll   beside the runtime (it links to
             python/python313.zip, python/DLLs/*.pyd           them, and Windows looks there)
    Linux:   python/lib/libpython3.13.so.1.0                   (found through the runtime's rpath)
             python/python313.zip, python/lib/python3.13/lib-dynload/*.so
"""
import argparse
import hashlib
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
VERSION = "3.13.15"
RELEASE = "20260901"
SHORT = "313"  # python313.dll, python313.zip
# target -> (python-build-standalone triple, sha256 of its install_only_stripped archive).
# Stripped builds: without debug information Linux's libpython is a tenth of the size.
BUILDS = {
    "windows-x86_64": ("x86_64-pc-windows-msvc",
                       "63d263ab0162f34a241a56dc5b283c22d6e131f5516117e6a921350c69ba7d4f"),
    "linux-x86_64": ("x86_64-unknown-linux-gnu",
                     "8a689a077337bea6d1c4bc0b7df1d52fcaa28f5f67e50df8bf417c1e3f9d8874"),
    "linux-arm64": ("aarch64-unknown-linux-gnu",
                    "01ce0ce9189feaead3298abf10d4efe998c55a489b3d5d38ca4f83dda7e7977e"),
}
URL = ("https://github.com/astral-sh/python-build-standalone/releases/download/"
       "{release}/cpython-{version}+{release}-{triple}-install_only_stripped.tar.gz")

# Not shipped in games: tests, GUI toolkits, packaging and development tools.
SKIP_PACKAGES = {"test", "tests", "idlelib", "tkinter", "turtledemo", "ensurepip", "lib2to3",
                 "venv", "pydoc_data", "site-packages", "__pycache__", "unittest", "doctest",
                 "config-3.13-x86_64-linux-gnu", "config-3.13-aarch64-linux-gnu"}
# Compiled modules games don't ship: tests, Tk, terminals, network security and databases
# (scripts reach the game's data through the midgard API, not sqlite3), and modules whose
# libraries have licences a game can't carry (GNU readline and gdbm are GPL) or that would need
# more notices (bzip2, xz).
SKIP_MODULES = {"_tkinter", "_ssl", "_hashlib", "_sqlite3", "winsound", "_wmi", "xxlimited",
                "xxlimited_35", "_xxinterpchannels", "_xxsubinterpreters", "_xxinterpqueues",
                "readline", "_curses", "_curses_panel", "_dbm", "_gdbm", "nis", "_bz2", "_lzma"}
SKIP_PREFIXES = ("_test", "_ctypes_test", "_xxtestfuzz", "tcl", "tk", "_tk", "libssl",
                 "libcrypto", "sqlite3")


def _wanted(name: str) -> bool:
    stem = name.split(".")[0]
    return stem not in SKIP_MODULES and not stem.startswith(SKIP_PREFIXES)


def host_target() -> str:
    os_name = "windows" if os.name == "nt" else "linux"
    arch = "arm64" if platform.machine().lower() in ("aarch64", "arm64") else "x86_64"
    return f"{os_name}-{arch}"


def runtime_files(runtime_dir: Path) -> list[str]:
    """What make_runtime added to a runtime folder, relative to it (for templates)."""
    names = [n for n in WINDOWS_DLLS if (runtime_dir / n).is_file()]
    return names + (["python"] if (runtime_dir / "python").is_dir() else [])


def install_dir(target: str) -> Path:
    """Where the fetched CPython lives: .tools/python/<version>+<release>-<target>/python."""
    return TOOLS / f"{VERSION}+{RELEASE}-{target}" / "python"


def fetch(target: str) -> Path:
    dest = install_dir(target)
    if (dest.parent / "ok").is_file():
        return dest
    triple, sha256 = BUILDS[target]
    url = URL.format(release=RELEASE, version=VERSION, triple=triple)
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
        tar.extractall(dest.parent, filter="data")
    (dest.parent / "ok").write_text(f"{VERSION}+{RELEASE} {triple}\n", "utf-8")
    return dest


def _skipped(relative: Path) -> bool:
    return any(part in SKIP_PACKAGES for part in relative.parts)


def _zip_stdlib(lib: Path, out: Path) -> None:
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for path in sorted(lib.rglob("*.py")):
            relative = path.relative_to(lib)
            if not _skipped(relative):
                z.write(path, relative.as_posix())


WINDOWS_DLLS = (f"python{SHORT}.dll", "python3.dll", "vcruntime140.dll", "vcruntime140_1.dll")


def make_runtime(source: Path, runtime_dir: Path) -> Path:
    """Adds the game's Python to a runtime folder (see the module docstring). Returns the
    python/ folder."""
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
        library = max((p for p in lib.glob("libpython3.13*.so*") if p.is_file()),
                      key=lambda p: p.stat().st_size)
        shutil.copy2(library, out / "lib" / (elf_soname(library) or library.name))
        dynload = lib / "python3.13" / "lib-dynload"
        out_dynload = out / "lib" / "python3.13" / "lib-dynload"
        out_dynload.mkdir(parents=True)
        for path in sorted(dynload.iterdir()):
            if _wanted(path.name):
                shutil.copy2(path, out_dynload / path.name)
        _zip_stdlib(lib / "python3.13", out / f"python{SHORT}.zip")
    (out / "LICENSE-python.txt").write_text(_licence(source), "utf-8")
    return out


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
                      source / "lib" / "python3.13" / "LICENSE.txt"):
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
        print(make_runtime(source, args.out))
    elif args.command == "where":
        print(source)
    else:
        print(f"CPython {VERSION} for {args.target} is at {source}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
