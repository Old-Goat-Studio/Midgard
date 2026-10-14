# SPDX-License-Identifier: Apache-2.0
from midgard_editor import about, version


def test_parse_runtime_version():
    info = about.parse_runtime_version(
        "Midgard runtime 0.1.0 (engine ABI 0, package format 1.x, schema 1-1)\n")
    assert info == about.EngineInfo("0.1.0", 0, 1)
    assert about.parse_runtime_version("something else") is None


def test_engine_line():
    text, ok = about.engine_line(about.EngineInfo(version.VERSION, 0, 1))
    assert ok and text == f"Engine v{version.VERSION} · package format 1.x"
    text, ok = about.engine_line(about.EngineInfo("9.9.9", 0, 1))
    assert not ok and text.endswith("(mismatch)")
    assert about.engine_line(None) == ("Engine not found", False)


def test_build_date_is_written_out():
    assert about.build_date_text("2026-10-09") == "October 9, 2026"
    assert about.build_date_text(None) is None


def test_about_box_follows_the_golden_ratio(qtbot):
    dialog = about.AboutDialog(engine=about.EngineInfo(version.VERSION, 0, 1))
    qtbot.addWidget(dialog)
    size = dialog.size()
    assert dialog.windowTitle() == "About Midgard"
    assert size.width() >= round(size.height() * 1.6)
    assert dialog.engine_label.isEnabled()
    assert dialog.version_label.text() == f"v{version.VERSION} (build {version.BUILD})"


def test_mismatched_engine_is_shown_disabled(qtbot):
    dialog = about.AboutDialog(engine=about.EngineInfo("0.0.1", 0, 1))
    qtbot.addWidget(dialog)
    assert not dialog.engine_label.isEnabled()
