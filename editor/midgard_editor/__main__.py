# SPDX-License-Identifier: Apache-2.0
"""Midgard editor entry point.

    midgard-editor                       open the editor
    midgard-editor --project X.config    open the editor with a project
    midgard-editor migrate X.db          command-line contract tools
    midgard-editor validate-manifest manifest.json
    midgard-editor export X.config --target android-arm64 --out DIR [--template T.zip]
                   [--selftest]           the engine's self-test APK (Android)
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
    exp = sub.add_parser("export", help="export a game without opening the editor")
    exp.add_argument("project", type=Path, help="the {project}.config")
    exp.add_argument("--target", required=True, help="windows-x86_64, linux-x86_64 or "
                     "android-arm64")
    exp.add_argument("--out", type=Path, required=True, help="output folder")
    exp.add_argument("--template", type=Path, help="install this runtime template zip first")
    exp.add_argument("--settings", type=Path, help="editor settings folder (templates, keys)")
    exp.add_argument("--selftest", action="store_true",
                     help="Android: the engine's self-test APK, which installs beside the game")
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

    if args.command == "export":
        return export_command(args)

    from .app import run  # the GUI; only imported when needed

    return run(project=args.project, smoke_test=args.smoke_test)


def export_command(args: argparse.Namespace) -> int:
    from PySide6.QtCore import QCoreApplication

    from .cook import CookError
    from .exporter import TARGETS, ExportError, export, install_template
    from .project import Project, ProjectError
    from .settings import Settings

    if args.target not in TARGETS:
        print(f"unknown target {args.target}: use one of {', '.join(TARGETS)}", file=sys.stderr)
        return 2
    app = QCoreApplication.instance() or QCoreApplication([])
    app.setApplicationName("Midgard")
    settings = Settings(args.settings)
    try:
        if args.template:
            print(f"Installed {install_template(args.template, settings.dir)}")
        archive = export(Project.open(args.project), args.target, args.out, settings.dir,
                         selftest=args.selftest)
    except (ExportError, CookError, ProjectError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(f"Exported {archive}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
