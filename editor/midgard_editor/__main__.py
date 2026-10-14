# SPDX-License-Identifier: Apache-2.0
"""Midgard editor entry point.

    midgard-editor                       open the editor
    midgard-editor --project X.config    open the editor with a project
    midgard-editor migrate X.db          command-line contract tools
    midgard-editor validate-manifest manifest.json
"""
import argparse
import json
import sys
from pathlib import Path

from . import contract
from .version import BUILD, VERSION


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="midgard-editor", description="Midgard editor")
    parser.add_argument("--version", action="store_true", help="print the version and exit")
    parser.add_argument("--project", type=Path, help="open this {project}.config")
    parser.add_argument("--smoke-test", type=Path, metavar="DIR",
                        help="start, open a test project in DIR, write smoke_test.json, quit")
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

    from .app import run  # the GUI; only imported when needed

    return run(project=args.project, smoke_test=args.smoke_test)


if __name__ == "__main__":
    sys.exit(main())
