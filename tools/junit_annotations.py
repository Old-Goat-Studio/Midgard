# SPDX-License-Identifier: Apache-2.0
"""Turn failed tests in a JUnit XML report (pytest --junitxml) into GitHub Actions annotations,
so a failure's message shows on the run page and in the public API, not only in the job log.

    python tools/junit_annotations.py report.xml
"""
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def escape(text: str) -> str:
    """GitHub's escaping for workflow command data."""
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def annotations(report: Path) -> list[str]:
    out = []
    for case in ET.parse(report).getroot().iter("testcase"):
        for problem in list(case.findall("failure")) + list(case.findall("error")):
            name = f"{case.get('classname', '')}.{case.get('name', '')}"
            detail = (problem.get("message") or "") + "\n" + (problem.text or "")
            lines = detail.strip().splitlines()
            out.append(f"::error title={escape(name)}::{escape(chr(10).join(lines[-40:]))}")
    return out


def main() -> int:
    for line in annotations(Path(sys.argv[1])):
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
