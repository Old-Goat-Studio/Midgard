# SPDX-License-Identifier: Apache-2.0
"""The Shader editor panel (milestone 5.17 step 4): the project's surface shaders
(shaders/*.glsl) in tabs, with GLSL highlighting, line numbers, completion for what a surface
shader reads and changes, its mistakes underlined on their lines as you type, and a live preview
beside them (animated, so a shader using the time moves). New ▸ starts one from a starter
(shader_starters.py).

The preview draws what is in the editor, saved or not; a shader with a mistake keeps showing the
last version that worked. Saving tells the rest of the editor (the Viewport, the Material
editor) that the shader changed.
"""
import logging
import time
from pathlib import Path

from PySide6.QtCore import QEvent, QObject, QRegularExpression, Qt, QTimer, Signal
from PySide6.QtGui import (
    QColor,
    QKeySequence,
    QPixmap,
    QShortcut,
    QSyntaxHighlighter,
    QTextCharFormat,
    QTextCursor,
)
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QMenu,
    QMessageBox,
    QPlainTextEdit,
    QSplitter,
    QTabWidget,
    QTextEdit,
    QToolButton,
    QToolTip,
    QVBoxLayout,
    QWidget,
)

from .. import meshes, objects, shader_starters, surface_shaders
from ..project import Project
from .objects_panel import PreviewLabel
from .script_editor import COLOURS, INDENT, CodeEditor, _format

log = logging.getLogger("midgard")

GLSL_KEYWORDS = ["if", "else", "for", "while", "do", "return", "break", "continue", "discard",
                 "const", "in", "out", "inout", "uniform", "struct", "void", "true", "false",
                 "switch", "case", "default"]
GLSL_TYPES = ["float", "int", "uint", "bool", "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4",
              "mat2", "mat3", "mat4", "sampler2D"]
GLSL_FUNCTIONS = ["abs", "sign", "floor", "ceil", "fract", "mod", "min", "max", "clamp", "mix",
                  "step", "smoothstep", "length", "distance", "dot", "cross", "normalize",
                  "reflect", "refract", "pow", "exp", "log", "sqrt", "sin", "cos", "tan", "asin",
                  "acos", "atan", "radians", "degrees", "texture"]
# What a surface shader reads (i.), changes (s.) and has to hand (surface.glsl).
INPUT_FIELDS = ["uv", "position", "view", "geometry_normal", "up", "sun", "time"]
SURFACE_FIELDS = ["color", "alpha", "metallic", "roughness", "normal", "emission", "occlusion",
                  "cutout"]
MIDGARD_NAMES = ["surface", "SurfaceInput", "Surface", "setting", "hash", "noise", "fbm",
                 "srgb", "bend_normal", "base_color_map", "metallic_roughness_map",
                 "normal_map", "occlusion_map", "emissive_map"]
SETTING_TYPES = ["float", "int", "bool", "vec2", "vec3", "vec4", "color", "texture",
                 "data_texture"]
SHAPES = {"prim_sphere": "Sphere", "prim_cube": "Cube", "prim_plane": "Plane"}
PREVIEW_ID = "shader-preview"
FPS = 15


class GlslHighlighter(QSyntaxHighlighter):
    """GLSL's keywords, types and functions, Midgard's names, numbers and comments."""

    def __init__(self, document, dark: bool = False):
        super().__init__(document)
        self.dark = None
        self.block_start = QRegularExpression(r"/\*")
        self.block_end = QRegularExpression(r"\*/")
        self.set_dark(dark)

    def set_dark(self, dark: bool) -> None:
        if dark == self.dark:
            return
        self.dark = dark
        c = COLOURS[dark]

        def words(names):
            return QRegularExpression(r"\b(" + "|".join(names) + r")\b")
        self.rules = [
            (words(GLSL_KEYWORDS), _format(c["keyword"], bold=True)),
            (words(GLSL_TYPES + ["color", "texture", "data_texture"]), _format(c["builtin"])),
            (words(GLSL_FUNCTIONS), _format(c["definition"])),
            (words(MIDGARD_NAMES), _format(c["midgard"], bold=True)),
            (QRegularExpression(r"\b(i|s)\.\w+"), _format(c["midgard"])),
            (QRegularExpression(r"\b[0-9]+(\.[0-9]*)?([eE][-+]?[0-9]+)?\b"),
             _format(c["number"])),
            (QRegularExpression(r"^\s*#.*"), _format(c["builtin"], italic=True)),
            (QRegularExpression(r"//[^\n]*"), _format(c["comment"], italic=True)),
        ]
        self.comment = _format(c["comment"], italic=True)
        self.rehighlight()

    def highlightBlock(self, text: str) -> None:
        for pattern, fmt in self.rules:
            matches = pattern.globalMatch(text)
            while matches.hasNext():
                m = matches.next()
                self.setFormat(m.capturedStart(), m.capturedLength(), fmt)
        # /* ... */ comments may span lines: block state 1 means "inside one".
        self.setCurrentBlockState(0)
        start = 0
        if self.previousBlockState() != 1:
            m = self.block_start.match(text)
            start = m.capturedStart() if m.hasMatch() else -1
        while start >= 0:
            end = self.block_end.match(text, start)
            if end.hasMatch():
                length = end.capturedEnd() - start
            else:
                self.setCurrentBlockState(1)
                length = len(text) - start
            self.setFormat(start, length, self.comment)
            m = self.block_start.match(text, start + length)
            start = m.capturedStart() if m.hasMatch() else -1


class ShaderCodeEditor(CodeEditor):
    """A GLSL editor: as the Script editor's, with GLSL highlighting and completion, braces
    indenting, and every mistake underlined on its line (hover for what it is)."""

    def __init__(self, path: Path, project: Project | None, parent: QWidget | None = None):
        super().__init__(path, project, parent)
        self.highlighter.setDocument(None)
        self.highlighter = GlslHighlighter(self.document())
        self.mistakes: dict[int, list[str]] = {}  # line -> what is wrong there

    # ---- mistakes ---------------------------------------------------------------------------
    def mark_mistakes(self, messages: list[dict]) -> None:
        self.mistakes = {}
        for m in messages:
            self.mistakes.setdefault(max(int(m.get("line", 0)), 1), []).append(
                m.get("message", ""))
        selections = []
        for line, texts in self.mistakes.items():
            block = self.document().findBlockByNumber(line - 1)
            if not block.isValid():
                continue
            selection = QTextEdit.ExtraSelection()
            selection.format.setUnderlineStyle(QTextCharFormat.UnderlineStyle.WaveUnderline)
            selection.format.setUnderlineColor(QColor("#e74c3c"))
            selection.format.setToolTip("\n".join(texts))
            cursor = QTextCursor(block)
            text = block.text()
            cursor.setPosition(block.position() + len(text) - len(text.lstrip()))
            cursor.setPosition(block.position() + max(len(text.rstrip()), 1),
                               QTextCursor.MoveMode.KeepAnchor)
            selection.cursor = cursor
            selections.append(selection)
        self.setExtraSelections(selections)
        first = min(self.mistakes) if self.mistakes else 0
        self.error_line = first
        self.error_message = "\n".join(self.mistakes.get(first, []))
        self.numbers.update()

    def viewportEvent(self, e) -> bool:
        """Hovering over a line with a mistake shows what it is."""
        if e.type() == QEvent.Type.ToolTip and self.mistakes:
            line = self.cursorForPosition(e.pos()).blockNumber() + 1
            if line in self.mistakes:
                QToolTip.showText(e.globalPos(), "\n".join(self.mistakes[line]), self)
                return True
            QToolTip.hideText()
        return super().viewportEvent(e)

    # ---- typing -----------------------------------------------------------------------------
    def keyPressEvent(self, e) -> None:
        popup = self.completer.popup()
        if popup.isVisible() and e.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter,
                                             Qt.Key.Key_Tab, Qt.Key.Key_Escape):
            e.ignore()
            return
        if e.key() == Qt.Key.Key_Space and e.modifiers() & Qt.KeyboardModifier.ControlModifier:
            self.complete()
            return
        if e.key() == Qt.Key.Key_Tab and not e.modifiers():
            self.textCursor().insertText(INDENT)
            return
        if e.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
            line = self.textCursor().block().text()[:self.textCursor().positionInBlock()]
            indent = line[:len(line) - len(line.lstrip())]
            if line.rstrip().endswith("{"):
                indent += INDENT
            self.textCursor().insertText("\n" + indent)
            return
        if e.text() == "}":
            cursor = self.textCursor()
            before = cursor.block().text()[:cursor.positionInBlock()]
            if before.strip() == "" and before.endswith(INDENT):
                for _ in INDENT:
                    cursor.deletePreviousChar()
        QPlainTextEdit.keyPressEvent(self, e)  # (not the Script editor's Python rules)
        if e.text() == ".":
            self.complete()
        elif popup.isVisible():
            self._refresh_prefix()

    def words(self) -> list[str]:
        """What completion offers here: after "s." or "i." their fields, else every name."""
        cursor = self.textCursor()
        text = cursor.block().text()[:cursor.positionInBlock()]
        word = self._word_before_cursor()
        before = text[:len(text) - len(word)]
        if before.endswith("s."):
            return SURFACE_FIELDS
        if before.endswith("i."):
            return INPUT_FIELDS
        if before.endswith("."):
            return []
        if before.strip() == "setting":
            return SETTING_TYPES
        own = QRegularExpression(r"\bsetting\s+\w+\s+(\w+)").globalMatch(self.toPlainText())
        settings = []
        while own.hasNext():
            settings.append(own.next().captured(1))
        return sorted(set(settings + MIDGARD_NAMES + GLSL_FUNCTIONS + GLSL_TYPES +
                          GLSL_KEYWORDS))

    def complete(self) -> None:
        prefix = self._word_before_cursor()
        found = [w for w in self.words() if w.lower().startswith(prefix.lower())]
        if not found or found == [prefix]:
            self.completer.popup().hide()
            return
        self._inserts = {w: w[len(prefix):] for w in found}
        self.completer.model().setStringList(found)
        self.completer.setCompletionPrefix(prefix)
        rect = self.cursorRect()
        rect.setWidth(self.completer.popup().sizeHintForColumn(0) + 30)
        self.completer.complete(rect)


class _Requests(QObject):
    """Asks, from anywhere (the Material editor), for a shader to be shown in the panel."""
    open = Signal(str)  # its content id ("" : just the panel)


_requests: _Requests | None = None


def requests() -> _Requests:
    global _requests
    if _requests is None:
        _requests = _Requests()
    return _requests


class ShaderEditorPanel(QWidget):
    """Tabs of surface shaders, their mistakes, and a live preview."""
    shader_saved = Signal(str)  # its content id ("shd:<name>")

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.project: Project | None = None
        self._started = time.monotonic()
        self._last_good: dict[str, bytes] = {}  # file -> the SPIR-V that last compiled
        self._settings: dict[str, list[dict]] = {}  # file -> its settings, as last compiled

        bar = QHBoxLayout()
        self.new_button = QToolButton()
        self.new_button.setText("New")
        self.new_button.setToolTip("A new surface shader from a starter")
        self.new_button.setPopupMode(QToolButton.ToolButtonPopupMode.InstantPopup)
        new_menu = QMenu(self.new_button)
        for key, (label, _code) in shader_starters.STARTERS.items():
            new_menu.addAction(label, lambda k=key: self.new_shader(k))
        self.new_button.setMenu(new_menu)
        self.open_button = QToolButton()
        self.open_button.setText("Open")
        self.open_button.setPopupMode(QToolButton.ToolButtonPopupMode.InstantPopup)
        self.open_menu = QMenu(self.open_button)
        self.open_menu.aboutToShow.connect(self._fill_open_menu)
        self.open_button.setMenu(self.open_menu)
        self.save_button = QToolButton()
        self.save_button.setText("Save")
        self.save_button.setToolTip("Save the shader (Ctrl+S)")
        self.save_button.clicked.connect(self.save_current)
        for b in (self.new_button, self.open_button, self.save_button):
            bar.addWidget(b)
        bar.addStretch(1)

        self.tabs = QTabWidget()
        self.tabs.setTabsClosable(True)
        self.tabs.setDocumentMode(True)
        self.tabs.tabCloseRequested.connect(self.close_tab)
        self.tabs.currentChanged.connect(lambda _i: self._compile_now())
        self.problems = QListWidget()
        self.problems.setMaximumHeight(110)
        self.problems.itemActivated.connect(self._go_to_problem)
        self.problems.itemClicked.connect(self._go_to_problem)
        code = QWidget()
        code_layout = QVBoxLayout(code)
        code_layout.setContentsMargins(0, 0, 0, 0)
        code_layout.addWidget(self.tabs, 1)
        code_layout.addWidget(self.problems)

        self.preview = PreviewLabel(260)
        self.preview.changed.connect(self.render_preview)
        self.shape = QComboBox()
        for asset, label in SHAPES.items():
            self.shape.addItem(label, asset)
        self.shape.currentIndexChanged.connect(lambda _i: self.render_preview())
        self.animate = QCheckBox("Animate")
        self.animate.setToolTip("Let the time run, for shaders that move")
        self.animate.setChecked(True)
        self.animate.toggled.connect(self._animate_toggled)
        self.preview_note = QLabel()
        self.preview_note.setWordWrap(True)
        side = QWidget()
        side_layout = QVBoxLayout(side)
        side_layout.setContentsMargins(0, 0, 0, 0)
        side_layout.addWidget(self.preview)
        row = QHBoxLayout()
        row.addWidget(self.shape, 1)
        row.addWidget(self.animate)
        side_layout.addLayout(row)
        side_layout.addWidget(self.preview_note)
        side_layout.addStretch(1)

        splitter = QSplitter()
        splitter.addWidget(code)
        splitter.addWidget(side)
        splitter.setStretchFactor(0, 3)
        splitter.setStretchFactor(1, 2)
        layout = QVBoxLayout(self)
        layout.addLayout(bar)
        layout.addWidget(splitter, 1)

        self._compile_timer = QTimer(self, singleShot=True, interval=350)
        self._compile_timer.timeout.connect(self._compile_now)
        self._frame_timer = QTimer(self, interval=1000 // FPS)
        self._frame_timer.timeout.connect(self.render_preview)
        QShortcut(QKeySequence.StandardKey.Save, self, self.save_current,
                  context=Qt.ShortcutContext.WidgetWithChildrenShortcut)
        self.set_project(None)

    # ---- the project ------------------------------------------------------------------------
    def set_project(self, project: Project | None) -> None:
        for editor in self.editors():
            self.tabs.removeTab(self.tabs.indexOf(editor))
            editor.deleteLater()
        self.project = project
        self._last_good.clear()
        self._settings.clear()
        for w in (self.new_button, self.open_button, self.save_button):
            w.setEnabled(project is not None)
        self.problems.clear()
        self.preview.clear()
        self.preview.setText("Open or make a shader to see it here" if project else "")
        self._animate_toggled(self.animate.isChecked())

    def editors(self) -> list[ShaderCodeEditor]:
        return [self.tabs.widget(i) for i in range(self.tabs.count())]

    def current(self) -> ShaderCodeEditor | None:
        return self.tabs.currentWidget()

    # ---- files ------------------------------------------------------------------------------
    def new_shader(self, starter: str = "blank",
                   name: str | None = None) -> ShaderCodeEditor | None:
        if self.project is None:
            return None
        label, code = shader_starters.STARTERS[starter]
        if name is None:
            name, ok = QInputDialog.getText(self, "New Shader", "Name:",
                                            text=label.lower().replace(" ", "_"))
            if not ok or not name.strip():
                return None
        shader = surface_shaders.new_shader(self.project.root, name.strip(), code)
        return self.open_shader(shader)

    def open_shader(self, shader: str) -> ShaderCodeEditor | None:
        """Shows a shader (by content id) in a tab, opening it if it isn't open."""
        if self.project is None:
            return None
        path = surface_shaders.source_path(self.project.root, shader)
        if path is None or not path.is_file():
            QMessageBox.warning(self, "Shader", f"The shader '{shader}' isn't in the project.")
            return None
        for editor in self.editors():
            if editor.path == path:
                self.tabs.setCurrentWidget(editor)
                return editor
        editor = ShaderCodeEditor(path, self.project)
        editor.setPlainText(path.read_text(encoding="utf-8"))
        editor.document().setModified(False)
        editor.refresh_git()
        editor.textChanged.connect(self._compile_timer.start)
        editor.document().modificationChanged.connect(lambda _m: self._update_titles())
        self.tabs.addTab(editor, path.stem)
        self.tabs.setCurrentWidget(editor)
        self._update_titles()
        self._compile_now()
        return editor

    def _fill_open_menu(self) -> None:
        self.open_menu.clear()
        names = surface_shaders.names(self.project.root) if self.project else []
        for name in names:
            self.open_menu.addAction(name, lambda n=name: self.open_shader(
                surface_shaders.ref(n)))
        if not names:
            self.open_menu.addAction("(no shaders yet: New makes one)").setEnabled(False)

    def _update_titles(self) -> None:
        for i, editor in enumerate(self.editors()):
            self.tabs.setTabText(i, editor.path.stem +
                                 (" *" if editor.document().isModified() else ""))

    def save(self, editor: ShaderCodeEditor) -> bool:
        try:
            editor.path.write_text(editor.toPlainText(), encoding="utf-8")
        except OSError as e:
            QMessageBox.warning(self, "Save", f"The shader couldn't be saved: {e}")
            return False
        editor.document().setModified(False)
        editor.refresh_git()
        self._update_titles()
        self.shader_saved.emit(surface_shaders.ref(editor.path.stem))
        return True

    def save_current(self) -> None:
        if (editor := self.current()) is not None:
            self.save(editor)

    def save_all(self) -> None:
        for editor in self.editors():
            if editor.document().isModified():
                self.save(editor)

    def close_tab(self, index: int, ask: bool = True) -> bool:
        editor = self.tabs.widget(index)
        if editor is None:
            return False
        if ask and editor.document().isModified():
            answer = QMessageBox.question(
                self, "Close", f"Save the changes to {editor.path.stem}?",
                QMessageBox.StandardButton.Save | QMessageBox.StandardButton.Discard |
                QMessageBox.StandardButton.Cancel)
            if answer == QMessageBox.StandardButton.Cancel:
                return False
            if answer == QMessageBox.StandardButton.Save and not self.save(editor):
                return False
        self.tabs.removeTab(index)
        editor.deleteLater()
        return True

    # ---- compiling as you type --------------------------------------------------------------
    def _compile_now(self) -> None:
        editor = self.current()
        self.problems.clear()
        if editor is None:
            return
        result = surface_shaders.compile_code(editor.toPlainText(), editor.path.stem)
        mistakes = [m for m in result.messages if m.get("error", True)]
        editor.mark_mistakes(mistakes)
        for m in result.messages:
            text = surface_shaders.describe(m)
            item = QListWidgetItem(("" if m.get("error", True) else "(warning) ") + text)
            item.setData(Qt.ItemDataRole.UserRole, int(m.get("line", 0)))
            item.setForeground(QColor("#e74c3c" if m.get("error", True) else "#d08030"))
            self.problems.addItem(item)
        key = str(editor.path)
        if result.ok:
            self._last_good[key] = result.spirv
            self._settings[key] = result.settings
            self.problems.addItem("The shader compiles." if not result.messages
                                  else "The shader compiles (with warnings).")
            self.preview_note.setText("")
        elif key in self._last_good:
            self.preview_note.setText("The preview shows the last version that worked.")
        else:
            self.preview_note.setText("The preview shows the shader once it compiles.")
        self.render_preview()

    def _go_to_problem(self, item: QListWidgetItem) -> None:
        editor, line = self.current(), item.data(Qt.ItemDataRole.UserRole)
        if editor is None or not line:
            return
        block = editor.document().findBlockByNumber(int(line) - 1)
        editor.setTextCursor(QTextCursor(block))
        editor.setFocus()

    # ---- the preview -------------------------------------------------------------------------
    def _animate_toggled(self, on: bool) -> None:
        if on and self.isVisible():
            self._frame_timer.start()
        else:
            self._frame_timer.stop()

    def showEvent(self, event) -> None:
        super().showEvent(event)
        self._animate_toggled(self.animate.isChecked())

    def hideEvent(self, event) -> None:
        super().hideEvent(event)
        self._frame_timer.stop()

    def clock(self) -> float:
        return (time.monotonic() - self._started) if self.animate.isChecked() else 0.0

    def render_preview(self) -> None:
        editor = self.current()
        if editor is None or self.project is None:
            return
        spirv = self._last_good.get(str(editor.path))
        if spirv is None:
            self.preview.clear()
            self.preview.setText("No preview yet: the shader has a mistake")
            return
        renderer = objects.shared_renderer()
        asset = self.shape.currentData()
        shader = f"shd:{PREVIEW_ID}/{editor.path.stem}"
        values, maps = surface_shaders.pack(self._settings.get(str(editor.path), []), {})
        material = {"color": [0.8, 0.8, 0.8, 1.0], "roughness": 0.5, "shader": shader}
        if values:
            material["shader_values"] = values
        parts = [{"name": "preview", "components": {"mesh": {"asset": asset},
                                                    "material": material}}]
        try:
            renderer.send(asset, meshes.to_glb(meshes.primitive(asset)))
            renderer.send(shader, spirv)
        except RuntimeError as e:
            self.preview_note.setText(str(e))
            return
        width = min(max(self.preview.width(), 160), 1024)
        image = renderer.render(PREVIEW_ID, parts, [-0.5, -0.5, -0.5, 0.5, 0.5, 0.5], width,
                                self.preview.height(), self.preview.yaw, self.preview.pitch,
                                self.preview.zoom, key_light=True, clock=self.clock())
        if image is None:
            self.preview.clear()
            self.preview.setText(f"No preview: {renderer.error or 'nothing to draw'}")
            self._frame_timer.stop()
        else:
            self.preview.setPixmap(QPixmap.fromImage(image))
