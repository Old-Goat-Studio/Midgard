# SPDX-License-Identifier: Apache-2.0
"""Help ▸ Midgard Help: the guide in its window (headings as a tree, search, Save as PDF)."""
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


def test_the_guide_opens_in_its_window_with_its_headings_as_a_tree(qtbot):
    window = guide.show_help()
    qtbot.addWidget(window)
    assert window.isVisible() and guide.show_help() is window  # (one window)
    sections = window.section_titles()
    assert sections[0] == "Contents"
    numbered = [t for t in sections if re.match(r"\d+\. ", t)]
    assert len(numbered) == 23 and numbered[0] == "1. Welcome"
    # A section's own headings sit under it.
    colours = window.item_for("9. Colours, materials and textures")
    children = [colours.child(i).text(0) for i in range(colours.childCount())]
    assert "The Material editor" in children
    # Clicking one shows it at the top of the text, and the tree follows the scrolling.
    item = window.item_for("Surface shaders: your own look for a material")
    window.go_to(item)
    block = window.text.document().findBlockByNumber(item.data(0, 0x0100))
    top = window.text.document().documentLayout().blockBoundingRect(block).top()
    bar = window.text.verticalScrollBar()
    assert bar.value() == min(int(top), bar.maximum())
    window.go_to(window.item_for("3. Your first five minutes"))
    window._follow()
    assert window.tree.currentItem().text(0) == "3. Your first five minutes"


def test_searching_finds_every_match_in_turn(qtbot):
    window = guide.HelpWindow()
    qtbot.addWidget(window)
    text = guide.GUIDE.read_text("utf-8").lower()
    window.search.setText("shader")
    count = len(window.matches)
    assert count >= 10 and abs(count - text.count("shader")) <= text.count("shader") // 4
    first = window.current
    window.find_next()
    assert window.current == (first + 1) % count
    window.find_previous()
    window.find_previous()
    assert window.current == (first - 1) % count
    assert window.found.text() == f"{window.current + 1} of {count}"
    # The tree shows the section the match is in.
    match = window.matches[window.current]
    assert window.tree.currentItem() is window.heading_at(match.selectionStart())
    window.search.setText("zzqqxx")
    assert window.matches == [] and window.found.text() == "Not found"
    window.search.clear()
    assert window.found.text() == ""


def test_the_guide_saves_as_a_pdf_with_its_footer(qtbot, tmp_path):
    import datetime

    from PySide6.QtPdf import QPdfDocument
    pages = guide.make_pdf(guide.GUIDE, tmp_path / "guide.pdf", datetime.date(2026, 11, 4))
    assert (tmp_path / "guide.pdf").read_bytes()[:5] == b"%PDF-"
    pdf = QPdfDocument()
    pdf.load(str(tmp_path / "guide.pdf"))
    # The title page, two or more pages of contents, then the guide.
    assert pages > 10 and pdf.pageCount() >= pages + 3
    # Each page's foot (offscreen, Qt draws text as shapes, so it's checked as written).
    assert guide.footer(3, pages, datetime.date(2026, 11, 4)) == (
        "Old Goat Studio", f"Page 3 of {pages}", "Printed 4 November 2026")
    window = guide.HelpWindow()
    qtbot.addWidget(window)
    assert window.save_pdf(tmp_path / "again.pdf") == tmp_path / "again.pdf"


def test_the_save_as_pdf_button_asks_where_then_saves_and_opens_it(qtbot, tmp_path,
                                                                    monkeypatch):
    from PySide6.QtCore import Qt
    from PySide6.QtGui import QDesktopServices
    from PySide6.QtWidgets import QFileDialog
    chosen = tmp_path / "My Guide"  # (".pdf" is added)
    monkeypatch.setattr(QFileDialog, "getSaveFileName", lambda *a, **k: (str(chosen), ""))
    opened = []
    monkeypatch.setattr(QDesktopServices, "openUrl", lambda url: opened.append(url) or True)
    window = guide.HelpWindow()
    qtbot.addWidget(window)
    qtbot.mouseClick(window.pdf_button, Qt.MouseButton.LeftButton)
    saved = tmp_path / "My Guide.pdf"
    assert saved.is_file() and saved.read_bytes()[:5] == b"%PDF-"
    assert opened and opened[0].toLocalFile() == str(saved).replace("\\", "/")


def test_the_guide_covers_what_it_promises_and_its_examples_are_python():
    text = guide.GUIDE.read_text("utf-8")
    contents = re.findall(r"^\d+\. (.+)$", text.split("## 1.")[0], re.M)
    headings = re.findall(r"^## \d+\. (.+)$", text, re.M)
    assert len(contents) == len(headings) == 23
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


def test_the_pdfs_contents_lists_each_section_and_heading_with_its_page():
    from PySide6.QtCore import QSizeF
    doc = guide.document()
    doc.setPageSize(QSizeF(800, 1000))
    entries = guide.contents(doc, 1000)
    titles = [t for _level, t, _page in entries]
    assert titles[0] == "1. Welcome" and "Contents" not in titles
    assert "The Material editor" in titles and "23. Keyboard shortcuts" in titles
    assert {level for level, _t, _p in entries} == {2, 3}
    numbers = [page for _l, _t, page in entries]
    assert numbers[0] == 1 and numbers == sorted(numbers)
    assert numbers[-1] <= doc.pageCount()
