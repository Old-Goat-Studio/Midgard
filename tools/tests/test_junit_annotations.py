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


def test_without_a_report_the_log_is_shown(tmp_path):
    log = tmp_path / "pytest.txt"
    log.write_text("collecting...\nFatal Python error: Segmentation fault\n", "utf-8")
    lines = junit_annotations.annotations(tmp_path / "missing.xml", log)
    assert lines[0].startswith("::error title=Errors in pytest.txt::")
    assert "Segmentation fault" in lines[0] and "Segmentation fault" in lines[-1]
    assert junit_annotations.annotations(tmp_path / "missing.xml") == []


def test_a_build_logs_errors_are_found_before_its_tail(tmp_path):
    log = tmp_path / "build.log"
    log.write_text("\n".join(
        ["[1/9] Building a.cpp.o",
         "FAILED: engine/a.cpp.o",
         "a.cpp:3:1: error: unknown type name 'Foo'",
         "    3 | Foo x;",
         "      | ^"]
        + [f"warning {i}" for i in range(100)]), "utf-8")
    lines = junit_annotations.annotations(tmp_path / "missing.xml", log)
    assert len(lines) == 2
    assert "FAILED: engine/a.cpp.o" in lines[0] and "unknown type name" in lines[0]
    assert "3 | Foo x;" in lines[0]
    assert "unknown type name" not in lines[1] and "warning 99" in lines[1]


def test_long_log_annotations_keep_under_githubs_limit(tmp_path):
    log = tmp_path / "build.log"
    log.write_text("\n".join(f"a.cpp:{i}: error: {'x' * 200}" for i in range(100)), "utf-8")
    first, last = junit_annotations.annotations(tmp_path / "missing.xml", log)
    assert "a.cpp:0: error" in first and len(first) < 4096
    assert "a.cpp:99: error" in last and len(last) < 4096
