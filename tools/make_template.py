# SPDX-License-Identifier: Apache-2.0
"""Package a runtime template from an engine build: the prebuilt engine for one target, which the
editor's Build ▸ Export copies and fills with a cooked game.

    python tools/make_template.py --preset windows-release [--out build/templates]

Writes midgard-runtime-<version>-<target>.zip containing template.json, the runtime executable,
LICENSE, NOTICE and THIRD_PARTY_NOTICES.txt (the components every exported game carries).
"""
import argparse
import json
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# preset -> template target
TARGETS = {
    "windows-release": {"os": "windows", "arch": "x86_64", "runtime": "midgard_runtime.exe"},
    "linux-release": {"os": "linux", "arch": "x86_64", "runtime": "midgard_runtime"},
    "linux-arm64-release": {"os": "linux", "arch": "arm64", "runtime": "midgard_runtime"},
}


def runtime_version(runtime: Path) -> dict:
    out = subprocess.run([str(runtime), "--version"], capture_output=True, text=True, timeout=30,
                         check=True).stdout
    # "Midgard runtime 0.1.0 (engine ABI 0, package format 1.x, schema 1-3)"
    words = out.replace("(", " ").replace(")", " ").replace(",", " ").split()
    version = words[words.index("runtime") + 1]
    abi = int(words[words.index("ABI") + 1])
    schema = words[words.index("schema") + 1].split("-")
    return {"engine": version, "engine_abi": abi,
            "schema_min": int(schema[0]), "schema_max": int(schema[1])}


def make_template(preset: str, out_dir: Path) -> Path:
    target = TARGETS[preset]
    runtime = ROOT / "build" / preset / "engine" / target["runtime"]
    if not runtime.is_file():
        sys.exit(f"{runtime} not found: build the engine with the {preset} preset first")
    info = {"format": "midgard-runtime-template", "format_version": 1,
            "target": {"os": target["os"], "arch": target["arch"]},
            "runtime": target["runtime"], **runtime_version(runtime)}
    notices = subprocess.run([sys.executable, str(ROOT / "tools" / "licenses.py"), "notices",
                              "--role", "runtime"], capture_output=True, text=True,
                             check=True).stdout
    out_dir.mkdir(parents=True, exist_ok=True)
    name = f"midgard-runtime-{info['engine']}-{target['os']}-{target['arch']}.zip"
    archive = out_dir / name
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("template.json", json.dumps(info, indent=2) + "\n")
        exe = zipfile.ZipInfo(target["runtime"])
        exe.external_attr = 0o100755 << 16  # keep the executable bit for Linux
        exe.compress_type = zipfile.ZIP_DEFLATED
        z.writestr(exe, runtime.read_bytes())
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
