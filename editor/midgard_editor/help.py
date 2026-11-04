# SPDX-License-Identifier: Apache-2.0
"""Help ▸ Midgard Help (F1): the guide (docs/help/Midgard_Help.md, written in Markdown) in its own
window, with a navigation tree made from its headings down the left, a search field, and Save as
PDF… (the whole guide, its pages numbered, with the studio's name and the date it was printed).

The guide is edited as plain Markdown and read as a document."""
import datetime
from pathlib import Path

from PySide6.QtCore import QMarginsF, QRectF, QSizeF, Qt
from PySide6.QtGui import (
    QColor,
    QDesktopServices,
    QFont,
    QFontMetricsF,
    QImage,
    QKeySequence,
    QPageLayout,
    QPageSize,
    QPainter,
    QPdfWriter,
    QPen,
    QShortcut,
    QTextCharFormat,
    QTextCursor,
    QTextDocument,
)
from PySide6.QtWidgets import (
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPushButton,
    QSplitter,
    QTextBrowser,
    QTextEdit,
    QToolButton,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

from . import BUNDLE_DIR
from .version import BUILD, VERSION

GUIDE = BUNDLE_DIR / "docs" / "help" / "Midgard_Help.md"
# The PDF's title page picture (the full-size one; the icon if it isn't there).
TITLE_PICTURE = BUNDLE_DIR / "midgard.png"
TITLE = "Midgard Help"
COMPANY = "Old Goat Studio"

# How the document looks (Qt's rich text understands this much CSS).
STYLE = """
body { font-family: 'Segoe UI', 'Noto Sans', 'DejaVu Sans', sans-serif; font-size: 10.5pt;
       color: #1d1d1f; }
h1 { font-size: 24pt; color: #7a2e12; margin-top: 18px; margin-bottom: 8px; }
h2 { font-size: 16pt; color: #7a2e12; margin-top: 22px; margin-bottom: 6px; }
h3 { font-size: 12.5pt; color: #3a3a3c; margin-top: 14px; margin-bottom: 4px; }
p, li { line-height: 135%; }
code { font-family: 'Cascadia Mono', 'Consolas', 'DejaVu Sans Mono', monospace;
       font-size: 9.5pt; background-color: #f2ece6; }
pre { font-family: 'Cascadia Mono', 'Consolas', 'DejaVu Sans Mono', monospace;
      font-size: 9.5pt; background-color: #f6f1ec; padding: 8px; }
table { border-collapse: collapse; margin-top: 4px; margin-bottom: 8px; }
th { background-color: #ead9cc; padding: 4px; text-align: left; }
td { padding: 4px; }
"""
PAPER = "QTextBrowser { background-color: #fffdfa; color: #1d1d1f; border: none; }"
MATCH = QColor("#ffe58a")  # every match of the search
CURRENT = QColor("#ffb84d")  # the one it's on


def document(guide: Path = GUIDE) -> QTextDocument:
    """The guide as a styled document (Markdown in, HTML with the house style)."""
    parsed = QTextDocument()
    parsed.setMarkdown(guide.read_text("utf-8"))
    doc = QTextDocument()
    doc.setDefaultStyleSheet(STYLE)
    doc.setDefaultFont(QFont("Segoe UI", 10))
    doc.setHtml(_styled(parsed.toHtml()))
    doc.setMetaInformation(QTextDocument.MetaInformation.DocumentTitle, TITLE)
    return doc


def headings(doc: QTextDocument, deepest: int = 3) -> list[tuple[int, str, int]]:
    """The document's headings, in order: (level 1..deepest, text, block number)."""
    found = []
    block = doc.begin()
    while block.isValid():
        level = block.blockFormat().headingLevel()
        if 0 < level <= deepest and block.text().strip():
            found.append((level, block.text().strip(), block.blockNumber()))
        block = block.next()
    return found


def printed_on(day: datetime.date | None = None) -> str:
    day = day or datetime.date.today()
    return f"Printed {day.day} {day:%B %Y}"


def footer(page: int, pages: int, day: datetime.date | None = None) -> tuple[str, str, str]:
    """A PDF page's foot: the studio's name and the date printed (both in italics) at the left
    and right, its number in the middle."""
    return COMPANY, f"Page {page} of {pages}", printed_on(day)


def make_pdf(guide: Path, out: Path, day: datetime.date | None = None) -> int:
    """The whole guide as a PDF (A4) written to `out`: a title page ("Midgard", the editor's
    version in italics, and Midgard's picture), the contents (each section and its headings with
    the page it starts on), then the guide, each page's foot with the studio's name and the date
    it was printed (in italics) and its number ("Page 3 of 40"). Returns the guide's pages (the
    title and contents pages aren't numbered)."""
    doc = document(guide)
    out.parent.mkdir(parents=True, exist_ok=True)
    writer = QPdfWriter(str(out))
    writer.setTitle(TITLE)
    writer.setCreator(f"Midgard {VERSION}+{BUILD}")
    writer.setResolution(150)
    writer.setPageLayout(QPageLayout(QPageSize(QPageSize.PageSizeId.A4),
                                     QPageLayout.Orientation.Portrait,
                                     QMarginsF(18, 14, 18, 12), QPageLayout.Unit.Millimeter))
    # Laid out for the writer (its resolution sizes the type), leaving room for the foot.
    doc.documentLayout().setPaintDevice(writer)
    width, height = writer.width(), writer.height()
    foot = int(writer.resolution() * 0.4)  # (0.4 inch)
    body = height - foot
    doc.setPageSize(QSizeF(width, body))
    pages = doc.pageCount()
    painter = QPainter(writer)

    def foot_line(number: str) -> None:
        italic = QFont("Segoe UI", 8)
        italic.setItalic(True)
        line_y = body + foot * 0.25
        painter.setPen(QColor("#b9a99c"))
        painter.drawLine(0, int(line_y), width, int(line_y))
        painter.setPen(QColor("#5a5a5e"))
        text = QRectF(0, line_y, width, foot - foot * 0.25)
        left, _middle, right = footer(1, 1, day)
        painter.setFont(italic)
        painter.drawText(text, Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter, left)
        painter.drawText(text, Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter,
                         right)
        if number:
            painter.setFont(QFont("Segoe UI", 8))
            painter.drawText(text, Qt.AlignmentFlag.AlignHCenter | Qt.AlignmentFlag.AlignVCenter,
                             number)

    try:
        # (The whole page white, margins too: some readers show an unpainted page as black.)
        layout = writer.pageLayout()
        full = layout.fullRectPixels(writer.resolution())
        paint = layout.paintRectPixels(writer.resolution())
        painter.fillRect(QRectF(full.left() - paint.left(), full.top() - paint.top(),
                                full.width(), full.height()), QColor("white"))
        _title_page(painter, width, height)
        for contents_page in _contents_pages(contents(doc, body), writer, width, body):
            writer.newPage()
            contents_page(painter)
            foot_line("")
        for page in range(pages):
            writer.newPage()
            painter.save()
            painter.translate(0, -page * body)
            doc.drawContents(painter, QRectF(0, page * body, width, body))
            painter.restore()
            foot_line(footer(page + 1, pages, day)[1])
    finally:
        painter.end()
    return pages


def contents(doc: QTextDocument, page_height: float) -> list[tuple[int, str, int]]:
    """The PDF's contents: each section (level 2) and its headings (level 3), with the page of
    the guide each starts on (1 for the guide's first page), as laid out in pages page_height
    tall. The guide's own title and its "Contents" list are left out."""
    layout = doc.documentLayout()
    found = []
    for level, title, number in headings(doc):
        if level < 2 or title == "Contents":
            continue
        block = doc.findBlockByNumber(number)
        # Where its words are (the space above a heading can fall at the foot of the page
        # before, while its words start the next).
        top = layout.blockBoundingRect(block).top()
        if block.layout() is not None and block.layout().lineCount():
            top += block.layout().lineAt(0).y()
        found.append((level, title, int(top // page_height) + 1))
    return found


def _contents_pages(entries: list[tuple[int, str, int]], device, width: int, height: float):
    """The contents laid out on pages: a function drawing each page (with a painter)."""
    heading = QFont("Segoe UI", 20)
    heading.setBold(True)
    section = QFont("Segoe UI", 10.5)
    section.setBold(True)
    entry = QFont("Segoe UI", 9.5)
    fonts = {2: section, 3: entry}
    metrics = {level: QFontMetricsF(font, device) for level, font in fonts.items()}
    indent = device.resolution() * 0.25  # (a quarter inch)
    title_height = QFontMetricsF(heading, device).height() * 1.8
    pages, page, y = [], [], title_height
    for level, title, number in entries:
        step = metrics[level].height() * (1.9 if level == 2 else 1.35)
        if y + step > height and page:
            pages.append(page)
            page, y = [], 0.0
        page.append((y + (step - metrics[level].height()), level, title, number))
        y += step
    if page:
        pages.append(page)

    def drawer(lines, first: bool):
        def draw(painter: QPainter) -> None:
            if first:
                painter.setPen(QColor("#7a2e12"))
                painter.setFont(heading)
                painter.drawText(QRectF(0, 0, width, title_height),
                                 Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignTop,
                                 "Contents")
            for y, level, title, number in lines:
                m = metrics[level]
                left = 0.0 if level == 2 else indent
                painter.setFont(fonts[level])
                painter.setPen(QColor("#1d1d1f") if level == 2 else QColor("#3a3a3c"))
                number_text = str(number)
                number_width = m.horizontalAdvance(number_text)
                room = width - left - number_width - m.horizontalAdvance("    ")
                shown = m.elidedText(title, Qt.TextElideMode.ElideRight, room)
                painter.drawText(QRectF(left, y, room, m.height()),
                                 Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
                                 shown)
                painter.drawText(QRectF(width - number_width, y, number_width, m.height()),
                                 Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter,
                                 number_text)
                # A dotted line from the heading to its page number.
                start = left + m.horizontalAdvance(shown) + m.horizontalAdvance(" ")
                end = width - number_width - m.horizontalAdvance(" ")
                if end > start:
                    pen = QPen(QColor("#9a8e84"))
                    pen.setStyle(Qt.PenStyle.DotLine)
                    pen.setWidthF(max(1.0, device.resolution() / 150))
                    painter.setPen(pen)
                    baseline = y + m.ascent() + (m.height() - m.ascent() - m.descent()) / 2
                    painter.drawLine(int(start), int(baseline), int(end), int(baseline))
        return draw

    return [drawer(lines, i == 0) for i, lines in enumerate(pages)]


def _title_page(painter: QPainter, width: int, height: int) -> None:
    """The PDF's first page: "Midgard", the editor's version in italics, and its picture, all
    centred."""
    painter.fillRect(QRectF(0, 0, width, height), QColor("white"))
    title = QFont("Segoe UI", 44)
    title.setBold(True)
    version = QFont("Segoe UI", 15)
    version.setItalic(True)
    painter.setPen(QColor("#7a2e12"))
    painter.setFont(title)
    painter.drawText(QRectF(0, height * 0.10, width, height * 0.10), Qt.AlignmentFlag.AlignCenter,
                     "Midgard")
    painter.setPen(QColor("#3a3a3c"))
    painter.setFont(version)
    painter.drawText(QRectF(0, height * 0.20, width, height * 0.05), Qt.AlignmentFlag.AlignCenter,
                     f"Version {VERSION}, build {BUILD}")
    from . import ICON_PATH
    picture = QImage(str(TITLE_PICTURE if TITLE_PICTURE.is_file() else ICON_PATH))
    if not picture.isNull():
        # (On white, without its see-through edges: PDF readers show transparency unevenly.)
        flat = QImage(picture.size(), QImage.Format.Format_RGB32)
        flat.fill(QColor("white"))
        flatten = QPainter(flat)
        flatten.drawImage(0, 0, picture)
        flatten.end()
        picture = flat
        side = min(width * 0.75, height * 0.6)
        painter.drawImage(QRectF((width - side) / 2, height * 0.30, side, side), picture)


def _styled(html: str) -> str:
    """Qt's HTML for the guide, touched up: tables with a fine border and a shaded first
    (heading) row, and code in a clearer type than Courier."""
    html = html.replace("font-family:'Courier New'", "font-family:'Consolas','Cascadia Mono',"
                        "'DejaVu Sans Mono',monospace; color:#3b2a20")
    parts = html.split("<table")
    for i in range(1, len(parts)):
        head, rest = parts[i].split(">", 1)
        head = head.replace('border="1"', 'border="0.6"')
        rest = rest.replace("<tr>", '<tr style="background-color:#ead9cc;">', 1)
        parts[i] = f'{head} style="border-color:#b9a99c;" cellspacing="0">{rest}'
    return "<table".join(parts)


class HelpWindow(QWidget):
    """The guide: the headings' tree on the left, the text on the right, a search field and Save
    as PDF… above them."""

    def __init__(self, guide: Path = GUIDE, parent: QWidget | None = None):
        super().__init__(parent, Qt.WindowType.Window)
        self.setWindowTitle(TITLE)
        self.guide = guide
        self.resize(1150, 800)

        self.search = QLineEdit()
        self.search.setPlaceholderText("Search the guide (Enter: next, Shift+Enter: previous)")
        self.search.setClearButtonEnabled(True)
        self.search.textChanged.connect(self._search_changed)
        self.search.returnPressed.connect(self.find_next)
        QShortcut(QKeySequence("Shift+Return"), self.search, self.find_previous)
        QShortcut(QKeySequence.StandardKey.Find, self, self.search.setFocus)
        self.previous_button = QToolButton()
        self.previous_button.setText("▲")
        self.previous_button.setToolTip("The match before (Shift+Enter)")
        self.previous_button.clicked.connect(self.find_previous)
        self.next_button = QToolButton()
        self.next_button.setText("▼")
        self.next_button.setToolTip("The next match (Enter)")
        self.next_button.clicked.connect(self.find_next)
        self.found = QLabel()
        self.found.setMinimumWidth(90)
        self.pdf_button = QPushButton("Save as PDF…")
        self.pdf_button.setToolTip("The whole guide as a PDF, its pages numbered, to print or "
                                   "keep")
        # (clicked() passes "checked": the save is asked for with no path, to choose one.)
        self.pdf_button.clicked.connect(lambda _checked=False: self.save_pdf())
        bar = QHBoxLayout()
        bar.addWidget(QLabel("Search"))
        bar.addWidget(self.search, 1)
        bar.addWidget(self.previous_button)
        bar.addWidget(self.next_button)
        bar.addWidget(self.found)
        bar.addStretch(0)
        bar.addWidget(self.pdf_button)

        self.tree = QTreeWidget()
        self.tree.setHeaderHidden(True)
        self.tree.setMinimumWidth(220)
        self.tree.itemClicked.connect(lambda item, _c: self.go_to(item))
        self.tree.itemActivated.connect(lambda item, _c: self.go_to(item))
        self.text = QTextBrowser()
        self.text.setOpenExternalLinks(True)
        self.text.setStyleSheet(PAPER)
        self.text.verticalScrollBar().valueChanged.connect(lambda _v: self._follow())
        splitter = QSplitter()
        splitter.addWidget(self.tree)
        splitter.addWidget(self.text)
        splitter.setStretchFactor(0, 1)
        splitter.setStretchFactor(1, 4)
        splitter.setSizes([280, 870])
        layout = QVBoxLayout(self)
        layout.addLayout(bar)
        layout.addWidget(splitter, 1)

        self.matches: list[QTextCursor] = []
        self.current = -1
        self.load()

    # ---- the guide and its headings ---------------------------------------------------------
    def load(self) -> None:
        doc = document(self.guide)
        doc.setParent(self.text)
        self.text.setDocument(doc)
        self.tree.clear()
        self.items: list[tuple[int, QTreeWidgetItem]] = []  # (block number, item), in order
        parents: dict[int, QTreeWidgetItem] = {}
        for level, title, block in headings(doc):
            if level == 1:  # (the guide's own title: the tree's top)
                item = QTreeWidgetItem(self.tree, [title])
                parents = {1: item}
            else:
                above = next((parents[k] for k in range(level - 1, 0, -1) if k in parents),
                             None)
                item = QTreeWidgetItem(above or self.tree, [title])
                parents = {k: v for k, v in parents.items() if k < level}
                parents[level] = item
            item.setData(0, Qt.ItemDataRole.UserRole, block)
            item.setToolTip(0, title)
            self.items.append((block, item))
        # Sections open, their own sections closed until wanted.
        for i in range(self.tree.topLevelItemCount()):
            self.tree.topLevelItem(i).setExpanded(True)
        self._search_changed(self.search.text())

    def section_titles(self) -> list[str]:
        """The tree's top sections (under the guide's title), as shown."""
        top = self.tree.topLevelItem(0)
        return [top.child(i).text(0) for i in range(top.childCount())] if top else []

    def item_for(self, title: str) -> QTreeWidgetItem | None:
        return next((item for _b, item in self.items if item.text(0) == title), None)

    def go_to(self, item: QTreeWidgetItem) -> None:
        """Scrolls the text so the item's heading is at the top."""
        block = self.text.document().findBlockByNumber(item.data(0, Qt.ItemDataRole.UserRole))
        top = self.text.document().documentLayout().blockBoundingRect(block).top()
        self.text.setTextCursor(QTextCursor(block))
        self.text.verticalScrollBar().setValue(int(top))
        self.tree.setCurrentItem(item)

    def heading_at(self, position: int) -> QTreeWidgetItem | None:
        """The heading (its tree item) of the section a text position is in."""
        block = self.text.document().findBlock(position).blockNumber()
        found = None
        for number, item in self.items:
            if number > block:
                break
            found = item
        return found

    def _follow(self) -> None:
        """The tree shows the section at the top of the text, as it scrolls."""
        y = self.text.verticalScrollBar().value() + 4
        layout = self.text.document().documentLayout()
        found = None
        for number, item in self.items:
            block = self.text.document().findBlockByNumber(number)
            if layout.blockBoundingRect(block).top() > y:
                break
            found = item
        if found is not None and found is not self.tree.currentItem():
            self.tree.blockSignals(True)
            self.tree.setCurrentItem(found)
            self.tree.scrollToItem(found)
            self.tree.blockSignals(False)

    # ---- search -------------------------------------------------------------------------------
    def _search_changed(self, words: str) -> None:
        """Every match shown at once; the first one after where the text is, made current."""
        self.matches = []
        self.current = -1
        doc = self.text.document()
        if words.strip():
            cursor = QTextCursor(doc)
            while True:
                cursor = doc.find(words, cursor)
                if cursor.isNull():
                    break
                self.matches.append(QTextCursor(cursor))
        if self.matches:
            here = self.text.cursorForPosition(self.text.viewport().rect().topLeft()).position()
            self.current = next((i for i, m in enumerate(self.matches)
                                 if m.selectionStart() >= here), 0)
            self._show_match()
        else:
            self._highlight()
        self.found.setText("" if not words.strip() else
                           f"{self.current + 1} of {len(self.matches)}" if self.matches
                           else "Not found")
        for b in (self.previous_button, self.next_button):
            b.setEnabled(len(self.matches) > 1)

    def find_next(self) -> None:
        if self.matches:
            self.current = (self.current + 1) % len(self.matches)
            self._show_match()

    def find_previous(self) -> None:
        if self.matches:
            self.current = (self.current - 1) % len(self.matches)
            self._show_match()

    def _show_match(self) -> None:
        match = self.matches[self.current]
        at = QTextCursor(match)  # (the cursor there, not a selection: the highlight shows)
        at.setPosition(match.selectionEnd())
        self.text.setTextCursor(at)
        # About a third of the way down the view, with what leads up to it above.
        bar = self.text.verticalScrollBar()
        y = self.text.cursorRect(at).top() + bar.value()
        bar.setValue(max(0, int(y - self.text.viewport().height() / 3)))
        self._highlight()
        self.found.setText(f"{self.current + 1} of {len(self.matches)}")
        item = self.heading_at(match.selectionStart())
        if item is not None:
            self.tree.blockSignals(True)
            self.tree.setCurrentItem(item)
            self.tree.scrollToItem(item)
            self.tree.blockSignals(False)

    def _highlight(self) -> None:
        selections = []
        for i, match in enumerate(self.matches):
            selection = QTextEdit.ExtraSelection()
            fmt = QTextCharFormat()
            fmt.setBackground(CURRENT if i == self.current else MATCH)
            selection.format = fmt
            selection.cursor = match
            selections.append(selection)
        self.text.setExtraSelections(selections)

    # ---- Save as PDF --------------------------------------------------------------------------
    def save_pdf(self, path: Path | None = None) -> Path | None:
        """The whole guide as a PDF where the user chooses (opened afterwards, if it can be)."""
        if path is None:
            from .settings import default_projects_dir
            start = default_projects_dir().parent / "Midgard_Help.pdf"
            chosen, _ = QFileDialog.getSaveFileName(self, "Save the Guide as PDF", str(start),
                                                    "PDF (*.pdf)")
            if not chosen:
                return None
            path = Path(chosen)
            if path.suffix.lower() != ".pdf":
                path = path.with_suffix(".pdf")
            open_after = True
        else:
            open_after = False
        try:
            make_pdf(self.guide, path)
        except OSError as e:
            QMessageBox.warning(self, TITLE, f"The PDF couldn't be saved: {e}")
            return None
        if open_after:
            from PySide6.QtCore import QUrl
            QDesktopServices.openUrl(QUrl.fromLocalFile(str(path)))
        return path


_window: HelpWindow | None = None


def show_help(settings_dir: Path | None = None, parent=None) -> HelpWindow | None:
    """Opens the guide's window (one at a time), or says it's missing."""
    global _window
    if not GUIDE.is_file():
        QMessageBox.warning(parent, TITLE, f"The guide is missing:\n{GUIDE}")
        return None
    if _window is None:
        _window = HelpWindow(GUIDE)
        _window.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose)
        _window.destroyed.connect(_forget)
    _window.show()
    _window.raise_()
    _window.activateWindow()
    return _window


def _forget(*_args) -> None:
    global _window
    _window = None
