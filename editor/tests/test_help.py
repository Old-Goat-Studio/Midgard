# SPDX-License-Identifier: Apache-2.0
"""Help ▸ Midgard Help: the guide, made into a PDF and opened in the system's viewer."""
import ast
import re

from midgard_editor import help as guide


def test_the_help_menu_has_the_guide_just_above_about(qtbot, settings):
    from midgard_editor.main_window import MainWindow
    window = MainWindow(settings)
    qtbot.addWidget(window)
    help_menu = next(a.menu() for a in window.menuBar().actions() if "Help" in a.text())
    names = [a.text().replace("&", "") for a in help_menu.actions() if not a.isSeparator()]
    assert names == ["Midgard Help", "About Midgard"]
    assert window.help_action.shortcut().toString() == "F1"


def test_the_guide_opens_as_a_pdf(qtbot, settings, monkeypatch):
    from PySide6.QtGui import QDesktopServices
    opened = []
    monkeypatch.setattr(QDesktopServices, "openUrl", lambda url: opened.append(url) or True)
    pdf = guide.show_help(settings.dir)
    assert pdf is not None and pdf.is_file()
    assert pdf.read_bytes()[:5] == b"%PDF-"
    assert opened and opened[0].toLocalFile().endswith(pdf.name)
    # Made once: asked again, it's the same file (until the guide changes).
    made = pdf.stat().st_mtime_ns
    assert guide.guide_pdf(settings.dir) == pdf and pdf.stat().st_mtime_ns == made


def test_the_guide_covers_what_it_promises_and_its_examples_are_python():
    text = guide.GUIDE.read_text("utf-8")
    contents = re.findall(r"^\d+\. (.+)$", text.split("## 1.")[0], re.M)
    headings = re.findall(r"^## \d+\. (.+)$", text, re.M)
    assert len(contents) == len(headings) == 22
    for example in re.findall(r"```python\n(.*?)```", text, re.S):
        ast.parse(example)  # (a mistake in an example would teach a mistake)


def test_the_guide_names_only_what_the_scripting_api_has():
    """Every midgard.<name> and entity.<name> the guide uses is in schema/midgard.pyi."""
    from midgard_editor import SCHEMA_DIR
    api = (SCHEMA_DIR / "midgard.pyi").read_text("utf-8")
    text = guide.GUIDE.read_text("utf-8")
    for name in set(re.findall(r"\bmidgard\.([a-z_]+)", text)):
        assert re.search(rf"^(def {name}\(|{name}: )", api, re.M), f"midgard.{name}"
    for name in set(re.findall(r"\bentity\.([a-z_]+)", text)):
        assert re.search(rf"^    (def {name}\(|{name}: )", api, re.M), f"entity.{name}"
