# SPDX-License-Identifier: Apache-2.0
"""Package a runtime template from an engine build: the prebuilt engine for one target, which the
editor's Build ▸ Export copies and fills with a cooked game.

    python tools/make_template.py --preset windows-release [--out build/templates]

Writes midgard-runtime-<version>-<target>.zip containing template.json, the runtime executable,
the game's Python beside it (python314.dll and python/ on Windows, python/ on Linux; listed in
template.json as runtime_files), LICENSE, NOTICE and THIRD_PARTY_NOTICES.txt (the components
every exported game carries).

For android-arm64 the "runtime" is base.apk: an unsigned APK holding the compiled manifest
(platforms/android/app), the launcher icon, SDL's Java activity as classes.dex, the engine as
lib/arm64-v8a/libmain.so and the game's Python (lib/arm64-v8a/libpython3.14.so and
assets/python.zip). The editor adds the game's data and signs it. Building it needs the
Android SDK (ANDROID_HOME, with build-tools and a platform) and a JDK (javac).
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ANDROID_APP = ROOT / "platforms" / "android" / "app"
ANDROID_MIN_SDK = 29     # Android 10: Vulkan 1.1
ANDROID_TARGET_SDK = 35

# preset -> template target
TARGETS = {
    "windows-release": {"os": "windows", "arch": "x86_64", "runtime": "midgard_runtime.exe"},
    "linux-release": {"os": "linux", "arch": "x86_64", "runtime": "midgard_runtime"},
    "linux-arm64-release": {"os": "linux", "arch": "arm64", "runtime": "midgard_runtime"},
    "android-arm64": {"os": "android", "arch": "arm64", "runtime": "base.apk"},
}


def run(*cmd) -> None:
    print("+", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True)


def header_version(build: Path) -> dict:
    """Version facts from the build's generated version.h (for runtimes that can't run here)."""
    text = (build / "engine" / "generated" / "midgard" / "version.h").read_text("utf-8")

    def value(name: str) -> str:
        return re.search(rf'{name} = "?([^";]+)"?;', text).group(1)
    return {"engine": value("kVersion"), "engine_abi": int(value("kEngineAbi")),
            "schema_min": int(value("kSchemaMin")), "schema_max": int(value("kSchemaMax"))}


def _newest(folder: Path, pattern: str) -> Path:
    def key(p: Path) -> list[int]:
        return [int(n) for n in re.findall(r"\d+", p.name)]
    versioned = re.compile(r"[\d.]+|android-[\d.]+")  # skips previews such as 36.0.0-rc1
    found = sorted((p for p in folder.glob(pattern) if versioned.fullmatch(p.name)), key=key)
    if not found:
        sys.exit(f"Nothing matching {pattern} in {folder}")
    return found[-1]


def sdl_java_sources(build: Path, work: Path) -> Path:
    """SDL's Java activity, from the source release matching the SDL linked into libmain.so."""
    config = build / "vcpkg_installed" / "arm64-android" / "share" / "sdl3" / \
        "SDL3ConfigVersion.cmake"
    version = re.search(r'set\(PACKAGE_VERSION "([^"]+)"\)', config.read_text("utf-8")).group(1)
    archive = work / f"SDL-release-{version}.tar.gz"
    if not archive.is_file():
        url = f"https://github.com/libsdl-org/SDL/archive/refs/tags/release-{version}.tar.gz"
        print(f"Downloading {url}", flush=True)
        urllib.request.urlretrieve(url, archive)
    prefix = f"SDL-release-{version}/android-project/app/src/main/java/"
    java = work / "java"
    shutil.rmtree(java, ignore_errors=True)
    with tarfile.open(archive) as tar:
        for member in tar.getmembers():
            if member.isfile() and member.name.startswith(prefix) and member.name.endswith(".java"):
                dest = java / member.name[len(prefix):]
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(tar.extractfile(member).read())
    return java


def make_android_apk(build: Path, work: Path) -> Path:
    """The unsigned template APK (see the module docstring)."""
    sdk = Path(os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT") or "")
    if not (sdk / "build-tools").is_dir():
        sys.exit("Set ANDROID_HOME to an Android SDK with build-tools and a platform installed")
    tools = _newest(sdk / "build-tools", "*")
    android_jar = _newest(sdk / "platforms", "android-*") / "android.jar"
    bat = ".bat" if os.name == "nt" else ""
    exe = ".exe" if os.name == "nt" else ""
    libmain = build / "engine" / "libmain.so"
    if not libmain.is_file():
        sys.exit(f"{libmain} not found: build the engine with the android-arm64 preset first")
    work.mkdir(parents=True, exist_ok=True)
    for old in ("classes", "dex"):
        shutil.rmtree(work / old, ignore_errors=True)

    java_dirs = [sdl_java_sources(build, work), ANDROID_APP / "java"]
    sources = sorted(str(p) for d in java_dirs for p in d.rglob("*.java"))
    run("javac", "-encoding", "UTF-8", "-source", "11", "-target", "11", "-nowarn",
        "-classpath", android_jar, "-d", work / "classes", *sources)
    (work / "dex").mkdir()
    classes = sorted(str(p) for p in (work / "classes").rglob("*.class"))
    run(tools / f"d8{bat}", "--release", "--min-api", ANDROID_MIN_SDK, "--lib", android_jar,
        "--output", work / "dex", *classes)

    run(tools / f"aapt2{exe}", "compile", "--dir", ANDROID_APP / "res", "-o", work / "res.zip")
    apk = work / "base.apk"
    run(tools / f"aapt2{exe}", "link", "-o", apk, "-I", android_jar,
        "--manifest", ANDROID_APP / "AndroidManifest.xml",
        "--min-sdk-version", ANDROID_MIN_SDK, "--target-sdk-version", ANDROID_TARGET_SDK,
        "--version-code", "1", "--version-name", "@@MIDGARD_GAME_VERSION@@", work / "res.zip")
    with zipfile.ZipFile(apk, "a", zipfile.ZIP_DEFLATED) as z:
        z.write(work / "dex" / "classes.dex", "classes.dex")
        z.write(libmain, "lib/arm64-v8a/libmain.so")
        # The game's Python (tools/setup_python.py, written beside libmain.so by the build).
        sys.path.insert(0, str(ROOT / "tools"))
        from setup_python import ANDROID_ASSET, ANDROID_LIBRARY

        if (libmain.parent / ANDROID_LIBRARY).is_file():
            z.write(libmain.parent / ANDROID_LIBRARY, f"lib/arm64-v8a/{ANDROID_LIBRARY}")
            z.write(libmain.parent / ANDROID_ASSET, f"assets/{ANDROID_ASSET}")
    run(tools / f"aapt2{exe}", "dump", "badging", apk)
    return apk


def runtime_version(runtime: Path) -> dict:
    out = subprocess.run([str(runtime), "--version"], capture_output=True, text=True, timeout=30,
                         check=True).stdout
    # "Midgard runtime 0.1.0 (engine ABI 0, package format 1.x, schema 1-4)"
    words = out.replace("(", " ").replace(")", " ").replace(",", " ").split()
    version = words[words.index("runtime") + 1]
    abi = int(words[words.index("ABI") + 1])
    schema = words[words.index("schema") + 1].split("-")
    return {"engine": version, "engine_abi": abi,
            "schema_min": int(schema[0]), "schema_max": int(schema[1])}


def make_template(preset: str, out_dir: Path) -> Path:
    target = TARGETS[preset]
    build = ROOT / "build" / preset
    if target["os"] == "android":
        runtime = make_android_apk(build, build / "android-template")
        version = header_version(build)
    else:
        runtime = build / "engine" / target["runtime"]
        if not runtime.is_file():
            sys.exit(f"{runtime} not found: build the engine with the {preset} preset first")
        version = runtime_version(runtime)
    info = {"format": "midgard-runtime-template", "format_version": 1,
            "target": {"os": target["os"], "arch": target["arch"]},
            "runtime": target["runtime"], **version}
    notices = subprocess.run([sys.executable, str(ROOT / "tools" / "licenses.py"), "notices",
                              "--role", "runtime"], capture_output=True, text=True,
                             check=True).stdout
    out_dir.mkdir(parents=True, exist_ok=True)
    name = f"midgard-runtime-{info['engine']}-{target['os']}-{target['arch']}.zip"
    archive = out_dir / name
    extra: list[str] = []
    if target["os"] != "android":
        sys.path.insert(0, str(ROOT / "tools"))
        from setup_python import runtime_files

        extra = runtime_files(runtime.parent)
    info["runtime_files"] = extra
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("template.json", json.dumps(info, indent=2) + "\n")
        exe = zipfile.ZipInfo(target["runtime"])
        exe.external_attr = 0o100755 << 16  # keep the executable bit for Linux
        exe.compress_type = zipfile.ZIP_DEFLATED
        z.writestr(exe, runtime.read_bytes())
        for name in extra:
            path = runtime.parent / name
            for file in sorted(path.rglob("*")) if path.is_dir() else [path]:
                if file.is_file():
                    z.write(file, file.relative_to(runtime.parent).as_posix())
        for doc in ("LICENSE", "NOTICE"):
            z.write(ROOT / doc, doc)
        z.writestr("THIRD_PARTY_NOTICES.txt", notices)
    return archive


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--preset", required=True, choices=sorted(TARGETS))
    parser.add_argument("--out", type=Path, default=ROOT / "build" / "templates")
    args = parser.parse_args()
    print(make_template(args.preset, args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
