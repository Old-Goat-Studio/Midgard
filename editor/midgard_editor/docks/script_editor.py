# SPDX-License-Identifier: Apache-2.0
"""The Script editor panel: the project's game scripts (scripts/*.py) in tabs, with Python
highlighting, line numbers, completion for the midgard API (Ctrl+Space, or after a dot), error
markers (syntax errors on saving, and errors from the running game), and snippets.

Saving a script while the game plays reloads it in the game (the Play session watches the
scripts folder).
"""
import keyword
import logging
from pathlib import Path

from PySide6.QtCore import (
    QEvent,
    QRect,
    QRegularExpression,
    QSize,
    QStringListModel,
    Qt,
    Signal,
)
from PySide6.QtGui import (
    QColor,
    QFont,
    QFontDatabase,
    QKeySequence,
    QPainter,
    QShortcut,
    QSyntaxHighlighter,
    QTextCharFormat,
    QTextCursor,
    QTextFormat,
)
from PySide6.QtWidgets import (
    QCompleter,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QMenu,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QStackedLayout,
    QTabWidget,
    QTextEdit,
    QToolButton,
    QToolTip,
    QVBoxLayout,
    QWidget,
)

from .. import scripting
from ..project import Project
from ..settings import Settings

log = logging.getLogger("midgard")
INDENT = "    "


def _format(color: str, bold: bool = False, italic: bool = False) -> QTextCharFormat:
    f = QTextCharFormat()
    f.setForeground(QColor(color))
    if bold:
        f.setFontWeight(QFont.Weight.Bold)
    f.setFontItalic(italic)
    return f


class PythonHighlighter(QSyntaxHighlighter):
    """Keywords, built-ins, the midgard module, strings, numbers, comments and decorators."""

    def __init__(self, document):
        super().__init__(document)
        words = r"\b(" + "|".join(keyword.kwlist + keyword.softkwlist) + r")\b"
        builtins = r"\b(print|len|range|min|max|abs|round|int|float|str|list|dict|set|tuple|" \
                   r"getattr|setattr|isinstance|enumerate|zip|sorted|sum|any|all)\b"
        self.rules = [
            (QRegularExpression(words), _format("#3b6fb6", bold=True)),
            (QRegularExpression(builtins), _format("#7a4fa0")),
            (QRegularExpression(r"\bmidgard\b"), _format("#b5651d", bold=True)),
            (QRegularExpression(r"\b(def|class)\s+(\w+)"), _format("#1f7a5c", bold=True)),
            (QRegularExpression(r"\b[0-9]+(\.[0-9]*)?\b"), _format("#a23b72")),
            (QRegularExpression(r"@\w+"), _format("#7a4fa0")),
            (QRegularExpression(r"[rbfu]?\"[^\"\\\\]*(\\\\.[^\"\\\\]*)*\""), _format("#2e8b57")),
            (QRegularExpression(r"[rbfu]?'[^'\\\\]*(\\\\.[^'\\\\]*)*'"), _format("#2e8b57")),
            (QRegularExpression(r"#[^\n]*"), _format("#888888", italic=True)),
        ]
        self.docstring = _format("#2e8b57", italic=True)
        self.triple = QRegularExpression(r"\"\"\"|'''")

    def highlightBlock(self, text: str) -> None:
        for pattern, fmt in self.rules:
            matches = pattern.globalMatch(text)
            while matches.hasNext():
                m = matches.next()
                self.setFormat(m.capturedStart(), m.capturedLength(), fmt)
        # Triple-quoted strings may span lines: block state 1 means "inside one".
        self.setCurrentBlockState(0)
        start = 0 if self.previousBlockState() == 1 else -1
        if start == -1:
            m = self.triple.match(text)
            start = m.capturedStart() if m.hasMatch() else -1
            skip = 3
        else:
            skip = 0
        while start >= 0:
            m = self.triple.match(text, start + skip)
            if m.hasMatch():
                end = m.capturedEnd()
                self.setFormat(start, end - start, self.docstring)
                n = self.triple.match(text, end)
                start, skip = (n.capturedStart(), 3) if n.hasMatch() else (-1, 0)
            else:
                self.setCurrentBlockState(1)
                self.setFormat(start, len(text) - start, self.docstring)
                break


class LineNumbers(QWidget):
    def __init__(self, editor: "CodeEditor"):
        super().__init__(editor)
        self.editor = editor

    def sizeHint(self) -> QSize:
        return QSize(self.editor.line_number_width(), 0)

    def paintEvent(self, event) -> None:
        self.editor.paint_line_numbers(event)


class CodeEditor(QPlainTextEdit):
    """A Python editor: line numbers, indentation, completion and error markers."""

    def __init__(self, path: Path, project: Project | None, parent: QWidget | None = None):
        super().__init__(parent)
        self.path, self.project = path, project
        self.error_line = 0
        self.error_message = ""
        font = QFontDatabase.systemFont(QFontDatabase.SystemFont.FixedFont)
        self.setFont(font)
        self.setTabStopDistance(self.fontMetrics().horizontalAdvance(" ") * 4)
        self.setLineWrapMode(QPlainTextEdit.LineWrapMode.NoWrap)
        self.highlighter = PythonHighlighter(self.document())
        self.numbers = LineNumbers(self)
        self.blockCountChanged.connect(lambda _n: self._update_margin())
        self.updateRequest.connect(self._scroll_numbers)
        self._update_margin()
        self.completer = QCompleter(self)
        self.completer.setWidget(self)
        self.completer.setCompletionMode(QCompleter.CompletionMode.PopupCompletion)
        self.completer.setCaseSensitivity(Qt.CaseSensitivity.CaseInsensitive)
        self.completer.activated.connect(self._insert_completion)
        self.completer.setModel(QStringListModel(self))
        self._inserts: dict[str, str] = {}

    # ---- line numbers ---------------------------------------------------------------------
    def line_number_width(self) -> int:
        digits = len(str(max(1, self.blockCount())))
        return 12 + self.fontMetrics().horizontalAdvance("9") * max(digits, 3)

    def _update_margin(self) -> None:
        self.setViewportMargins(self.line_number_width(), 0, 0, 0)

    def _scroll_numbers(self, rect: QRect, dy: int) -> None:
        if dy:
            self.numbers.scroll(0, dy)
        else:
            self.numbers.update(0, rect.y(), self.numbers.width(), rect.height())

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        r = self.contentsRect()
        self.numbers.setGeometry(QRect(r.left(), r.top(), self.line_number_width(), r.height()))

    def paint_line_numbers(self, event) -> None:
        painter = QPainter(self.numbers)
        painter.fillRect(event.rect(), self.palette().alternateBase())
        block = self.firstVisibleBlock()
        top = round(self.blockBoundingGeometry(block).translated(self.contentOffset()).top())
        height = round(self.blockBoundingRect(block).height())
        while block.isValid() and top <= event.rect().bottom():
            number = block.blockNumber() + 1
            if block.isVisible():
                painter.setPen(QColor("#c0392b") if number == self.error_line
                               else self.palette().placeholderText().color())
                painter.drawText(0, top, self.numbers.width() - 6, height,
                                 Qt.AlignmentFlag.AlignRight, str(number))
            block = block.next()
            top += height

    # ---- errors ---------------------------------------------------------------------------
    def mark_error(self, line: int, message: str) -> None:
        self.error_line, self.error_message = line, message
        selection = QTextEdit.ExtraSelection()
        selection.format.setBackground(QColor(231, 76, 60, 60))
        selection.format.setProperty(QTextFormat.Property.FullWidthSelection, True)
        selection.format.setToolTip(message)
        block = self.document().findBlockByNumber(max(line - 1, 0))
        selection.cursor = QTextCursor(block)
        self.setExtraSelections([selection])
        self.numbers.update()

    def clear_error(self) -> None:
        self.error_line, self.error_message = 0, ""
        self.setExtraSelections([])
        self.numbers.update()

    def viewportEvent(self, e) -> bool:
        """Hovering over the marked line shows the error."""
        if e.type() == QEvent.Type.ToolTip and self.error_line:
            if self.cursorForPosition(e.pos()).blockNumber() + 1 == self.error_line:
                QToolTip.showText(e.globalPos(), self.error_message, self)
                return True
            QToolTip.hideText()
        return super().viewportEvent(e)

    # ---- typing ---------------------------------------------------------------------------
    def keyPressEvent(self, e) -> None:
        popup = self.completer.popup()
        if popup.isVisible() and e.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter,
                                             Qt.Key.Key_Tab, Qt.Key.Key_Escape):
            e.ignore()  # the completer handles these
            return
        if e.key() == Qt.Key.Key_Space and e.modifiers() & Qt.KeyboardModifier.ControlModifier:
            self.complete()
            return
        if e.key() == Qt.Key.Key_Tab and not e.modifiers():
            self.textCursor().insertText(INDENT)
            return
        if e.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
            line = self.textCursor().block().text()
            indent = line[:len(line) - len(line.lstrip())]
            if line.rstrip().endswith(":"):
                indent += INDENT
            super().keyPressEvent(e)
            self.textCursor().insertText(indent)
            return
        super().keyPressEvent(e)
        if e.text() == ".":
            self.complete()
        elif popup.isVisible():
            self._refresh_prefix()

    def _word_before_cursor(self) -> str:
        cursor = self.textCursor()
        text = cursor.block().text()[:cursor.positionInBlock()]
        i = len(text)
        while i and (text[i - 1].isalnum() or text[i - 1] == "_"):
            i -= 1
        return text[i:]

    def complete(self) -> None:
        cursor = self.textCursor()
        found = scripting.completions(self.toPlainText(), cursor.blockNumber() + 1,
                                      cursor.positionInBlock(), self.path, self.project)
        if not found:
            self.completer.popup().hide()
            return
        prefix = self._word_before_cursor()
        self._inserts = {c.name: c.name[len(prefix):] for c in found}
        self.completer.model().setStringList([c.name for c in found])
        self.completer.setCompletionPrefix(prefix)
        rect = self.cursorRect()
        rect.setWidth(self.completer.popup().sizeHintForColumn(0) + 30)
        self.completer.complete(rect)

    def _refresh_prefix(self) -> None:
        self.completer.setCompletionPrefix(self._word_before_cursor())
        self.completer.popup().setCurrentIndex(self.completer.completionModel().index(0, 0))

    def _insert_completion(self, name: str) -> None:
        prefix = self._word_before_cursor()
        self.textCursor().insertText(name[len(prefix):])


class ScriptEditorPanel(QWidget):
    open_requested = Signal(str)  # a module, from elsewhere in the editor

    def __init__(self, settings: Settings, parent: QWidget | None = None):
        super().__init__(parent)
        self.settings = settings
        self.project: Project | None = None
        self.tabs = QTabWidget()
        self.tabs.setTabsClosable(True)
        self.tabs.setDocumentMode(True)
        self.tabs.tabCloseRequested.connect(self.close_tab)
        self.status = QLabel()
        self.status.setWordWrap(True)

        new = QPushButton("New Script…")
        new.clicked.connect(self.new_script)
        self.open_button = QToolButton()
        self.open_button.setText("Open")
        self.open_button.setPopupMode(QToolButton.ToolButtonPopupMode.InstantPopup)
        self.open_menu = QMenu(self.open_button)
        self.open_menu.aboutToShow.connect(self._fill_open_menu)
        self.open_button.setMenu(self.open_menu)
        save = QPushButton("Save")
        save.setToolTip("Save (Ctrl+S). While the game plays, it reloads the script.")
        save.clicked.connect(self.save_current)
        self.snippets_button = QToolButton()
        self.snippets_button.setText("Snippets")
        self.snippets_button.setPopupMode(QToolButton.ToolButtonPopupMode.InstantPopup)
        self.snippets_menu = QMenu(self.snippets_button)
        self.snippets_menu.aboutToShow.connect(self._fill_snippets_menu)
        self.snippets_button.setMenu(self.snippets_menu)
        bar = QHBoxLayout()
        for w in (new, self.open_button, save, self.snippets_button):
            bar.addWidget(w)
        bar.addStretch()

        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(4, 4, 4, 4)
        layout.addLayout(bar)
        layout.addWidget(self.tabs)
        layout.addWidget(self.status)
        self.empty = QLabel("Open a project to edit its scripts.")
        self.empty.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.stack = QStackedLayout(self)
        self.stack.addWidget(self.empty)
        self.stack.addWidget(page)
        shortcut = QShortcut(QKeySequence.StandardKey.Save, self)
        shortcut.setContext(Qt.ShortcutContext.WidgetWithChildrenShortcut)
        shortcut.activated.connect(self.save_current)
        self.open_requested.connect(self.open_module)
        self.set_project(None)

    # ---- project and tabs -----------------------------------------------------------------
    def set_project(self, project: Project | None) -> None:
        while self.tabs.count():
            self.close_tab(0, ask=False)
        self.project = project
        self.stack.setCurrentIndex(0 if project is None else 1)
        self.status.clear()

    def editors(self) -> list[CodeEditor]:
        return [self.tabs.widget(i) for i in range(self.tabs.count())]

    def current(self) -> CodeEditor | None:
        return self.tabs.currentWidget()

    def open_file(self, path: Path) -> CodeEditor | None:
        for i, editor in enumerate(self.editors()):
            if editor.path.resolve() == path.resolve():
                self.tabs.setCurrentIndex(i)
                return editor
        try:
            text = path.read_text("utf-8")
        except OSError as e:
            QMessageBox.warning(self, "Open Script", str(e))
            return None
        editor = CodeEditor(path, self.project)
        editor.setPlainText(text)
        editor.document().setModified(False)
        editor.document().modificationChanged.connect(lambda _m: self._update_titles())
        self.tabs.addTab(editor, path.name)
        self.tabs.setCurrentWidget(editor)
        self._update_titles()
        return editor

    def open_module(self, module: str) -> CodeEditor | None:
        if self.project is None:
            return None
        file = scripting.module_file(self.project, module)
        if not file.is_file():
            scripting.create_script(self.project, module)
        return self.open_file(file)

    def _update_titles(self) -> None:
        for i, editor in enumerate(self.editors()):
            star = "*" if editor.document().isModified() else ""
            self.tabs.setTabText(i, editor.path.name + star)
            self.tabs.setTabToolTip(i, str(editor.path))

    def close_tab(self, index: int, ask: bool = True) -> bool:
        editor = self.tabs.widget(index)
        if editor is None:
            return True
        if ask and editor.document().isModified():
            answer = QMessageBox.question(
                self, "Close Script", f"Save the changes to {editor.path.name}?",
                QMessageBox.StandardButton.Save | QMessageBox.StandardButton.Discard |
                QMessageBox.StandardButton.Cancel)
            if answer == QMessageBox.StandardButton.Cancel:
                return False
            if answer == QMessageBox.StandardButton.Save and not self.save(editor):
                return False
        self.tabs.removeTab(index)
        editor.deleteLater()
        return True

    def _fill_open_menu(self) -> None:
        self.open_menu.clear()
        found = scripting.modules(self.project) if self.project else {}
        if not found:
            self.open_menu.addAction("(no scripts yet)").setEnabled(False)
        for module, path in found.items():
            self.open_menu.addAction(module, lambda p=path: self.open_file(p))

    # ---- saving and errors ----------------------------------------------------------------
    def save(self, editor: CodeEditor) -> bool:
        source = editor.toPlainText()
        try:
            editor.path.parent.mkdir(parents=True, exist_ok=True)
            editor.path.write_text(source, "utf-8")
        except OSError as e:
            QMessageBox.warning(self, "Save Script", str(e))
            return False
        editor.document().setModified(False)
        error = scripting.check_syntax(source, editor.path.name)
        if error:
            editor.mark_error(*error)
            self.status.setText(f"{editor.path.name}, line {error[0]}: {error[1]}")
        else:
            editor.clear_error()
            self.status.setText(f"Saved {editor.path.name}.")
        return True

    def save_current(self) -> None:
        if (editor := self.current()) is not None:
            self.save(editor)

    def save_all(self) -> None:
        for editor in self.editors():
            if editor.document().isModified():
                self.save(editor)

    def show_error(self, file: str, line: int, message: str) -> None:
        """An error from the running game: file is relative to scripts/."""
        if self.project is None or not file:
            self.status.setText(message.strip().splitlines()[-1] if message.strip() else "")
            return
        editor = self.open_file(scripting.scripts_dir(self.project) / file)
        if editor is None:
            return
        last = message.strip().splitlines()[-1] if message.strip() else "error"
        editor.mark_error(line, message.strip())
        cursor = QTextCursor(editor.document().findBlockByNumber(max(line - 1, 0)))
        editor.setTextCursor(cursor)
        editor.centerCursor()
        self.status.setText(f"In the game: {file}, line {line}: {last}")

    # ---- new scripts ----------------------------------------------------------------------
    def new_script(self) -> None:
        if self.project is None:
            return
        name, ok = QInputDialog.getText(self, "New Script", "Script name (for example door):")
        if not ok or not name.strip():
            return
        module = scripting.new_module_name(self.project, name)
        self.open_file(scripting.create_script(self.project, module))

    # ---- snippets -------------------------------------------------------------------------
    def _fill_snippets_menu(self) -> None:
        menu = self.snippets_menu
        menu.clear()
        groups = {"built-in": "Starter snippets", "project": "This project",
                  "user": "My snippets"}
        snippets = scripting.all_snippets(self.project, self.settings)
        for source, title in groups.items():
            items = [s for s in snippets if s.source == source]
            if not items:
                continue
            sub = menu.addMenu(title)
            for s in items:
                action = sub.addAction(s.name, lambda s=s: self.insert_snippet(s))
                action.setToolTip(s.body)
        menu.addSeparator()
        has_selection = self.current() is not None and self.current().textCursor().hasSelection()
        save_project = menu.addAction("Save Selection to This Project…",
                                      lambda: self.save_selection("project"))
        save_user = menu.addAction("Save Selection to My Snippets…",
                                   lambda: self.save_selection("user"))
        for action in (save_project, save_user):
            action.setEnabled(has_selection and self.project is not None)
        own = [s for s in snippets if s.source in ("project", "user")]
        if own:
            delete = menu.addMenu("Delete Snippet")
            for s in own:
                delete.addAction(f"{s.name} ({groups[s.source].lower()})",
                                 lambda s=s: self.delete_snippet(s))

    def insert_snippet(self, snippet: scripting.Snippet) -> None:
        editor = self.current()
        if editor is None:
            return
        cursor = editor.textCursor()
        indent = cursor.block().text()[:cursor.positionInBlock()]
        indent = indent if not indent.strip() else ""
        lines = snippet.body.rstrip("\n").split("\n")
        cursor.insertText(lines[0] + "".join("\n" + indent + line if line else "\n"
                                             for line in lines[1:]))
        editor.setFocus()

    def save_selection(self, where: str) -> None:
        editor = self.current()
        if editor is None or self.project is None:
            return
        body = editor.textCursor().selectedText().replace(" ", "\n")
        if not body.strip():
            return
        name, ok = QInputDialog.getText(self, "Save Snippet", "Snippet name:")
        if not ok or not name.strip():
            return
        if where == "project":
            scripting.add_project_snippet(self.project, name.strip(), body)
        else:
            scripting.add_user_snippet(self.settings, name.strip(), body)
        self.status.setText(f"Saved the snippet '{name.strip()}'.")

    def delete_snippet(self, snippet: scripting.Snippet) -> None:
        if snippet.source == "project" and self.project is not None:
            scripting.delete_project_snippet(self.project, snippet.id)
        elif snippet.source == "user":
            scripting.delete_user_snippet(self.settings, snippet.id)
