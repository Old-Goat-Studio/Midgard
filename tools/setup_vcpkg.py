# SPDX-License-Identifier: Apache-2.0
"""Fetch vcpkg at the exact commit pinned by vcpkg.json's builtin-baseline, into .tools/vcpkg.

Local builds and CI both use this copy, so they always build the same dependency versions.
The vcpkg bundled with Visual Studio is not used: it lags behind the pinned port files.

    python tools/setup_vcpkg.py
"""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VCPKG_DIR = ROOT / ".tools" / "vcpkg"
REPO = "https://github.com/microsoft/vcpkg"


def git(*args: str, cwd: Path | None = None, check: bool = True) -> str:
    return subprocess.run(["git", *args], cwd=cwd, check=check, capture_output=True,
                          text=True).stdout.strip()


def main() -> int:
    baseline = json.loads((ROOT / "vcpkg.json").read_text(encoding="utf-8"))["builtin-baseline"]
    if not (VCPKG_DIR / ".git").exists():
        VCPKG_DIR.parent.mkdir(parents=True, exist_ok=True)
        print(f"Cloning vcpkg into {VCPKG_DIR} …")
        # Blobless clone: full history (vcpkg's versioning needs it), file contents on demand.
        git("clone", "--filter=blob:none", "--no-checkout", REPO, str(VCPKG_DIR))
    checked_out = (VCPKG_DIR / "scripts" / "buildsystems" / "vcpkg.cmake").exists()
    if not checked_out or git("rev-parse", "HEAD", cwd=VCPKG_DIR) != baseline:
        if git("cat-file", "-t", baseline, cwd=VCPKG_DIR, check=False) != "commit":
            git("fetch", "--filter=blob:none", "origin", cwd=VCPKG_DIR)
        git("-c", "advice.detachedHead=false", "checkout", "--force", baseline, cwd=VCPKG_DIR)
    tool = VCPKG_DIR / ("vcpkg.exe" if os.name == "nt" else "vcpkg")
    if not tool.exists():
        script = "bootstrap-vcpkg.bat" if os.name == "nt" else "bootstrap-vcpkg.sh"
        subprocess.run([str(VCPKG_DIR / script), "-disableMetrics"], cwd=VCPKG_DIR, check=True,
                       shell=os.name == "nt")
    print(f"vcpkg ready at {VCPKG_DIR} (baseline {baseline[:12]})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
