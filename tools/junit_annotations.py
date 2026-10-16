# SPDX-License-Identifier: Apache-2.0
"""Turn failed tests in a JUnit XML report (pytest --junitxml) into GitHub Actions annotations,
so a failure's message shows on the run page and in the public API, not only in the job log.

    python tools/junit_annotations.py report.xml [--log pytest-output.txt]

If the report is missing or names no failures (pytest crashed, or a step before the tests
failed), the log is annotated instead: its error lines (a compiler's or linker's, which a
parallel build prints long before it stops) and its last lines.
"""
import argparse
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def escape(text: str) -> str:
    """GitHub's escaping for workflow command data."""
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


# Compiler, linker, Ninja and CMake errors, and Python's fatal ones.
ERROR = re.compile(r"\berror\b:|^FAILED:|CMake Error|undefined (reference|symbol)", re.IGNORECASE)


MAX_CHARS = 3500


def log_errors(text: list[str], limit: int = 30, after: int = 2) -> list[str]:
    """The log's error lines, each with the few lines after it (the compiler's context)."""
    out: list[str] = []
    shown = -1
    for i, line in enumerate(text):
        if ERROR.search(line) and i > shown:
            out.extend(text[i:i + after + 1])
            shown = i + after
            if len(out) >= limit * (after + 1):
                break
    return out


def log_tail(log: Path, lines: int = 30) -> list[str]:
    try:
        text = log.read_text("utf-8", errors="replace").strip().splitlines()
    except OSError:
        return []
    out = []
    errors = log_errors(text)
    if errors:  # GitHub keeps only the first ~4 KB of an annotation
        shown = escape(chr(10).join(errors)[:MAX_CHARS])
        out.append(f"::error title=Errors in {log.name}::{shown}")
    shown = escape(chr(10).join(text[-lines:])[-MAX_CHARS:])
    out.append(f"::error title=Last lines of {log.name}::{shown}")
    return out


def annotations(report: Path, log: Path | None = None) -> list[str]:
    out = []
    try:
        cases = list(ET.parse(report).getroot().iter("testcase"))
    except (OSError, ET.ParseError):
        cases = []
    for case in cases:
        for problem in list(case.findall("failure")) + list(case.findall("error")):
            name = f"{case.get('classname', '')}.{case.get('name', '')}"
            detail = (problem.get("message") or "") + "\n" + (problem.text or "")
            lines = detail.strip().splitlines()
            out.append(f"::error title={escape(name)}::{escape(chr(10).join(lines[-40:]))}")
    if not out and log is not None:
        out = log_tail(log)
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("report", type=Path)
    parser.add_argument("--log", type=Path)
    args = parser.parse_args()
    for line in annotations(args.report, args.log):
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
