# SPDX-License-Identifier: Apache-2.0
"""Licence checks for CI, and THIRD_PARTY_NOTICES.txt for exports.

    python tools/licenses.py check      # fail if anything is unlisted, unlicensed or not allowed
    python tools/licenses.py notices [--role runtime|editor] [--out FILE]
"""
import argparse
import json
import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
THIRD_PARTY = ROOT / "tools" / "third_party.json"
SPDX = "SPDX-License-Identifier: Apache-2.0"

# Source files that must start with an SPDX header (within their first three lines).
HEADER_GLOBS = ["**/*.py", "**/*.cpp", "**/*.h", "**/*.h.in", "**/*.sql", "**/CMakeLists.txt"]
SKIP_DIRS = {".git", ".venv", ".tools", "build", "out", "vcpkg_installed", "node_modules"}


def load() -> dict:
    return json.loads(THIRD_PARTY.read_text(encoding="utf-8"))


def _skipped(path: Path) -> bool:
    return any(part in SKIP_DIRS for part in path.relative_to(ROOT).parts)


def missing_headers() -> list[str]:
    missing = []
    for pattern in HEADER_GLOBS:
        for path in ROOT.glob(pattern):
            if _skipped(path):
                continue
            head = path.read_text(encoding="utf-8", errors="replace").splitlines()[:3]
            if not any(SPDX in line for line in head):
                missing.append(str(path.relative_to(ROOT)))
    return sorted(set(missing))


def declared_dependencies() -> dict[str, set[str]]:
    """Dependency names from vcpkg.json and editor/pyproject.toml."""
    vcpkg = json.loads((ROOT / "vcpkg.json").read_text(encoding="utf-8"))
    cpp = {d if isinstance(d, str) else d["name"] for d in vcpkg.get("dependencies", [])}
    pyproject = tomllib.loads((ROOT / "editor" / "pyproject.toml").read_text(encoding="utf-8"))
    project = pyproject["project"]
    reqs = list(project.get("dependencies", []))
    for extra in project.get("optional-dependencies", {}).values():
        reqs += extra
    py = {re.split(r"[<>=!~\[ ;]", r, maxsplit=1)[0].strip() for r in reqs}
    return {"vcpkg": cpp, "python": py}


def problems() -> list[str]:
    data = load()
    allowed = data["allowed"]
    listed = {(c["source"], c["name"].lower()): c for c in data["components"]}
    found = []
    for source, names in declared_dependencies().items():
        for name in sorted(names):
            if (source, name.lower()) not in listed:
                found.append(f"{name} ({source}) is not listed in tools/third_party.json")
    for c in data["components"]:
        role, lic = c["ships_in"], c["license"]
        if role not in allowed:
            found.append(f"{c['name']}: unknown ships_in '{role}'")
            continue
        # "A AND B": every part must be allowed. "A OR B": at least one part must be.
        terms = [t.strip().strip("()") for t in lic.split(" AND ")]
        for term in terms:
            choices = [c_.strip().strip("()") for c_ in term.split(" OR ")]
            if not any(choice in allowed[role] for choice in choices):
                found.append(f"{c['name']}: licence {term} is not allowed for '{role}'")
    found += [f"{p}: missing '{SPDX}' header" for p in missing_headers()]
    return found


def notices(role: str) -> str:
    """The text of THIRD_PARTY_NOTICES.txt for a game export ('runtime') or the editor."""
    roles = {"runtime"} if role == "runtime" else {"runtime", "editor"}
    parts = [
        "THIRD-PARTY NOTICES",
        "",
        (ROOT / "NOTICE").read_text(encoding="utf-8").strip(),
        "",
        "This software includes the following third-party components:",
        "",
    ]
    for c in sorted(load()["components"], key=lambda c: c["name"].lower()):
        if c["ships_in"] in roles:
            parts += [f"- {c['name']} ({c['license']})", f"  {c['url']}", f"  {c['copyright']}", ""]
    return "\n".join(parts)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("check")
    n = sub.add_parser("notices")
    n.add_argument("--role", choices=["runtime", "editor"], default="runtime")
    n.add_argument("--out", type=Path)
    args = parser.parse_args(argv)

    if args.command == "check":
        found = problems()
        for p in found:
            print(p, file=sys.stderr)
        print("licence check: " + ("OK" if not found else f"{len(found)} problem(s)"))
        return 1 if found else 0
    text = notices(args.role)
    if args.out:
        args.out.write_text(text, encoding="utf-8")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
