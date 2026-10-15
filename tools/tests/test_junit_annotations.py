# SPDX-License-Identifier: Apache-2.0
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import junit_annotations  # noqa: E402

REPORT = """<?xml version="1.0" encoding="utf-8"?>
<testsuites><testsuite name="pytest">
  <testcase classname="tests.test_a" name="test_ok" time="0.1"/>
  <testcase classname="tests.test_a" name="test_bad" time="0.1">
    <failure message="AssertionError: 100% wrong">line one
line two</failure>
  </testcase>
</testsuite></testsuites>
"""


def test_failures_become_annotations(tmp_path):
    report = tmp_path / "report.xml"
    report.write_text(REPORT, "utf-8")
    lines = junit_annotations.annotations(report)
    assert len(lines) == 1
    assert lines[0].startswith("::error title=tests.test_a.test_bad::")
    assert "100%25 wrong%0Aline one%0Aline two" in lines[0]
