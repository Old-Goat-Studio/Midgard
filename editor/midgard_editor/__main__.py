# SPDX-License-Identifier: Apache-2.0
"""Command line for the Phase 0 contract tools. The editor window arrives in Phase 1."""
import argparse
import json
import sys
from pathlib import Path

from . import contract
from .version import BUILD, VERSION


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="midgard-editor", description="Midgard editor")
    parser.add_argument("--version", action="store_true", help="print the version and exit")
    sub = parser.add_subparsers(dest="command")
    mig = sub.add_parser("migrate", help="create or upgrade a project database")
    mig.add_argument("database", type=Path)
    val = sub.add_parser("validate-manifest", help="check a package manifest.json")
    val.add_argument("manifest", type=Path)
    args = parser.parse_args(argv)

    if args.version:
        print(f"Midgard editor {VERSION} (build {BUILD})")
        return 0
    if args.command == "migrate":
        version = contract.migrate(args.database)
        print(f"{args.database}: schema version {version}")
        return 0
    if args.command == "validate-manifest":
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
        errors = contract.manifest_errors(manifest)
        for error in errors:
            print(error, file=sys.stderr)
        print(f"{args.manifest}: {'valid' if not errors else f'{len(errors)} problem(s)'}")
        return 1 if errors else 0
    parser.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
