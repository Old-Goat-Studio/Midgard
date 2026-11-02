# SPDX-License-Identifier: Apache-2.0
"""Help ▸ Midgard Help: the guide (docs/help/Midgard_Help.md, written in Markdown) shown as a
PDF in the system's PDF viewer.

The PDF is made from the Markdown the first time it's asked for (and again whenever the guide
or the editor's build changes), into Midgard's settings folder, so the guide is edited as
plain text and always read as a document."""
from pathlib import Path

from PySide6.QtCore import QMarginsF, QSizeF, QUrl
from PySide6.QtGui import (
    QDesktopServices,
    QFont,
    QPageLayout,
    QPageSize,
    QPdfWriter,
    QTextDocument,
)

from . import BUNDLE_DIR
from .version import BUILD, VERSION

GUIDE = BUNDLE_DIR / "docs" / "help" / "Midgard_Help.md"
TITLE = "Midgard Help"

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


def pdf_path(settings_dir: Path) -> Path:
    return Path(settings_dir) / "help" / f"Midgard_Help-{VERSION}+{BUILD}.pdf"


def make_pdf(guide: Path, out: Path) -> Path:
    """The guide as a PDF (A4, numbered pages), written to `out`."""
    markdown = guide.read_text("utf-8")
    # Markdown in, then HTML with the house style (the style applies to HTML).
    parsed = QTextDocument()
    parsed.setMarkdown(markdown)
    html = parsed.toHtml()
    doc = QTextDocument()
    doc.setDefaultStyleSheet(STYLE)
    doc.setDefaultFont(QFont("Segoe UI", 10))
    doc.setHtml(_styled(html))
    doc.setMetaInformation(QTextDocument.MetaInformation.DocumentTitle, TITLE)
    out.parent.mkdir(parents=True, exist_ok=True)
    writer = QPdfWriter(str(out))
    writer.setTitle(TITLE)
    writer.setCreator(f"Midgard {VERSION}+{BUILD}")
    writer.setResolution(150)
    writer.setPageLayout(QPageLayout(QPageSize(QPageSize.PageSizeId.A4),
                                     QPageLayout.Orientation.Portrait,
                                     QMarginsF(18, 16, 18, 16), QPageLayout.Unit.Millimeter))
    # Laid out for the writer (its resolution sizes the type), a page at a time.
    doc.documentLayout().setPaintDevice(writer)
    doc.setPageSize(QSizeF(writer.width(), writer.height()))
    doc.print_(writer)
    return out


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


def guide_pdf(settings_dir: Path, guide: Path = GUIDE) -> Path:
    """The guide's PDF, made if it's missing or older than the guide."""
    out = pdf_path(settings_dir)
    if not out.is_file() or out.stat().st_mtime < guide.stat().st_mtime:
        make_pdf(guide, out)
    return out


def show_help(settings_dir: Path, parent=None) -> Path | None:
    """Opens the guide as a PDF in the system's viewer (says so if it can't). Returns the PDF."""
    from PySide6.QtWidgets import QMessageBox
    if not GUIDE.is_file():
        QMessageBox.warning(parent, TITLE, f"The guide is missing:\n{GUIDE}")
        return None
    try:
        pdf = guide_pdf(settings_dir)
    except OSError as e:
        QMessageBox.warning(parent, TITLE, f"The guide couldn't be made into a PDF: {e}")
        return None
    if not QDesktopServices.openUrl(QUrl.fromLocalFile(str(pdf))):
        QMessageBox.information(parent, TITLE, f"There's no PDF viewer to open it with. The "
                                f"guide is here:\n{pdf}\n\n(and as text: {GUIDE})")
    return pdf
