# SPDX-License-Identifier: Apache-2.0
"""The Assistant panel (roadmap §9): a conversation with Claude about the open project.

Requests run on a worker thread, so the editor never freezes; replies stream in and Stop
cancels. Each turn lists the tools the model used, and each can be expanded to show exactly
what was sent back to it. "What's Sent…" shows the system prompt (the midgard API and the
component reference) that goes with every request.
"""
import threading

from PySide6.QtCore import QObject, Qt, QThread, QTimer, Signal
from PySide6.QtGui import QKeySequence, QShortcut
from PySide6.QtWidgets import (
    QDialog,
    QDialogButtonBox,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QScrollArea,
    QStackedLayout,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

from .. import assistant
from ..project import Project
from ..settings import Settings


class AskWorker(QObject):
    """Runs one question on a worker thread."""

    text = Signal(str)
    tool = Signal(object)  # assistant.ToolCall
    done = Signal(object)  # assistant.Turn

    def __init__(self, client, project: Project, conversation: assistant.Conversation,
                 question: str, model: str, effort: str):
        super().__init__()
        self.client, self.project, self.conversation = client, project, conversation
        self.question, self.model, self.effort = question, model, effort
        self.stop = threading.Event()

    def run(self) -> None:
        turn = assistant.ask(self.client, self.project, self.conversation, self.question,
                             model=self.model, effort=self.effort, on_text=self.text.emit,
                             on_tool=self.tool.emit, cancelled=self.stop.is_set)
        self.done.emit(turn)


class ToolView(QWidget):
    """One tool call: a line that expands to show what was sent back to the model."""

    def __init__(self, call: assistant.ToolCall):
        super().__init__()
        args = ", ".join(f"{k}={v!r}" for k, v in call.arguments.items())
        self.button = QToolButton()
        self.button.setText(f"▸ {call.name}({args})")
        self.button.setToolButtonStyle(Qt.ToolButtonStyle.ToolButtonTextOnly)
        self.button.setAutoRaise(True)
        self.button.setCheckable(True)
        self.result = QPlainTextEdit(call.result)
        self.result.setReadOnly(True)
        self.result.setMaximumHeight(160)
        self.result.setVisible(False)
        self.button.toggled.connect(self._toggle)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.button, alignment=Qt.AlignmentFlag.AlignLeft)
        layout.addWidget(self.result)

    def _toggle(self, shown: bool) -> None:
        self.result.setVisible(shown)
        self.button.setText(("▾" if shown else "▸") + self.button.text()[1:])


class TurnView(QWidget):
    def __init__(self, turn: assistant.Turn):
        super().__init__()
        self.question = QLabel(turn.question)
        self.question.setWordWrap(True)
        self.question.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.question.setStyleSheet("font-weight: bold;")
        self.tools = QVBoxLayout()
        self.answer = QLabel()
        self.answer.setWordWrap(True)
        self.answer.setTextFormat(Qt.TextFormat.MarkdownText)
        self.answer.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse |
                                            Qt.TextInteractionFlag.LinksAccessibleByMouse)
        self.answer.setOpenExternalLinks(True)
        self.error = QLabel()
        self.error.setWordWrap(True)
        self.error.setStyleSheet("color: #c0392b;")
        layout = QVBoxLayout(self)
        layout.setContentsMargins(4, 8, 4, 8)
        layout.addWidget(self.question)
        layout.addLayout(self.tools)
        layout.addWidget(self.answer)
        layout.addWidget(self.error)
        self.text = turn.answer
        for call in turn.tools:
            self.add_tool(call)
        self.show_turn(turn)

    def add_tool(self, call: assistant.ToolCall) -> None:
        self.tools.addWidget(ToolView(call))

    def add_text(self, text: str) -> None:
        self.text += text

    def refresh(self) -> None:
        self.answer.setText(self.text)

    def show_turn(self, turn: assistant.Turn) -> None:
        self.text = turn.answer
        self.refresh()
        self.error.setText(turn.error)
        self.error.setVisible(bool(turn.error))


class AssistantPanel(QWidget):
    def __init__(self, settings: Settings, parent: QWidget | None = None):
        super().__init__(parent)
        self.settings = settings
        self.project: Project | None = None
        self.conversation = assistant.Conversation()
        self.thread: QThread | None = None
        self.worker: AskWorker | None = None
        self.current: TurnView | None = None
        self._redraw = QTimer(self, singleShot=True, interval=60)  # streamed text, batched
        self._redraw.timeout.connect(lambda: self.current and self.current.refresh())

        # Messages for when the Assistant can't be used.
        self.message = QLabel()
        self.message.setWordWrap(True)
        self.message.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.message_button = QPushButton()
        self.message_button.clicked.connect(self._message_action)
        off = QWidget()
        off_layout = QVBoxLayout(off)
        off_layout.addStretch()
        off_layout.addWidget(self.message)
        off_layout.addWidget(self.message_button, alignment=Qt.AlignmentFlag.AlignCenter)
        off_layout.addStretch()

        # The conversation.
        self.turns = QVBoxLayout()
        self.turns.addStretch()
        holder = QWidget()
        holder.setLayout(self.turns)
        self.scroll = QScrollArea()
        self.scroll.setWidgetResizable(True)
        self.scroll.setWidget(holder)
        self.input = QPlainTextEdit()
        self.input.setPlaceholderText("Ask about your scripts or the midgard API… "
                                      "(Ctrl+Enter sends)")
        self.input.setMaximumHeight(110)
        self.send = QPushButton("Send")
        self.send.clicked.connect(self.send_or_stop)
        new = QPushButton("New Conversation")
        new.clicked.connect(self.new_conversation)
        key = QPushButton("Set API Key…")
        key.clicked.connect(self.set_api_key)
        sent = QPushButton("What's Sent…")
        sent.setToolTip("The instructions and reference sent with every question")
        sent.clicked.connect(self.show_system_prompt)
        self.status = QLabel()
        self.status.setWordWrap(True)
        top = QHBoxLayout()
        for w in (new, key, sent):
            top.addWidget(w)
        top.addStretch()
        bottom = QHBoxLayout()
        bottom.addWidget(self.input)
        bottom.addWidget(self.send, alignment=Qt.AlignmentFlag.AlignBottom)
        chat = QWidget()
        chat_layout = QVBoxLayout(chat)
        chat_layout.setContentsMargins(4, 4, 4, 4)
        chat_layout.addLayout(top)
        chat_layout.addWidget(self.scroll)
        chat_layout.addLayout(bottom)
        chat_layout.addWidget(self.status)

        self.stack = QStackedLayout(self)
        self.stack.addWidget(off)
        self.stack.addWidget(chat)
        shortcut = QShortcut(QKeySequence("Ctrl+Return"), self.input)
        shortcut.activated.connect(self.send_or_stop)
        self.refresh_state()

    # ---- state ----------------------------------------------------------------------------
    def set_project(self, project: Project | None) -> None:
        self.cancel()
        self.project = project
        self.conversation = assistant.load(project) if project else assistant.Conversation()
        self._show_conversation()
        self.refresh_state()

    def refresh_state(self) -> None:
        """Which screen to show: off, not allowed here, no key, or the conversation."""
        if not self.settings.get("assistant_enabled"):
            self._off("The Assistant is off. It sends your questions, and the project data it "
                      "looks at, to Anthropic's API, using your own API key.", "Turn On")
        elif self.project is None:
            self._off("Open a project to talk to the Assistant about it.", None)
        elif not assistant.allowed(self.project):
            self._off("This project doesn't allow the Assistant "
                      '("assistant": {"allowed": false} in its config).', None)
        elif not assistant.api_key():
            self._off("Add your Anthropic API key to start. It's kept in your system's "
                      "credential store, never in the project.", "Set API Key…")
        else:
            self.stack.setCurrentIndex(1)
            self._update_status()

    def _off(self, text: str, button: str | None) -> None:
        self.message.setText(text)
        self.message_button.setVisible(button is not None)
        if button:
            self.message_button.setText(button)
        self.stack.setCurrentIndex(0)

    def _message_action(self) -> None:
        if self.message_button.text() == "Turn On":
            self.settings.set("assistant_enabled", True)
            self.refresh_state()
        else:
            self.set_api_key()

    def _update_status(self) -> None:
        usage = self.conversation.usage()
        model = self.settings.get("assistant_model") or assistant.DEFAULT_MODEL
        effort = self.settings.get("assistant_effort") or "medium"
        tokens = (f" · {usage.get('input_tokens', 0) + usage.get('cache_read_input_tokens', 0):,}"
                  f" tokens in, {usage.get('output_tokens', 0):,} out" if usage else "")
        self.status.setText(f"{model}, {effort} effort{tokens}")

    # ---- the conversation -----------------------------------------------------------------
    def _show_conversation(self) -> None:
        while self.turns.count() > 1:
            item = self.turns.takeAt(0)
            if item.widget():
                item.widget().deleteLater()
        for turn in self.conversation.turns:
            self.turns.insertWidget(self.turns.count() - 1, TurnView(turn))

    def new_conversation(self) -> None:
        self.cancel()
        self.conversation = assistant.Conversation()
        if self.project is not None:
            assistant.save(self.project, self.conversation)
        self._show_conversation()
        self._update_status()

    @property
    def busy(self) -> bool:
        return self.thread is not None

    def send_or_stop(self) -> None:
        if self.busy:
            self.cancel()
        else:
            self.ask(self.input.toPlainText())

    def ask(self, question: str) -> None:
        question = question.strip()
        if not question or self.busy or self.project is None:
            return
        key = assistant.api_key()
        if not key:
            self.refresh_state()
            return
        import anthropic

        self.input.clear()
        self.current = TurnView(assistant.Turn(question))
        self.turns.insertWidget(self.turns.count() - 1, self.current)
        self.worker = AskWorker(anthropic.Anthropic(api_key=key), self.project, self.conversation,
                                question,
                                self.settings.get("assistant_model") or assistant.DEFAULT_MODEL,
                                self.settings.get("assistant_effort") or "medium")
        self.thread = QThread(self)
        self.worker.moveToThread(self.thread)
        self.thread.started.connect(self.worker.run)
        self.worker.text.connect(self._text)
        self.worker.tool.connect(self._tool)
        self.worker.done.connect(self._done)
        self.send.setText("Stop")
        self.status.setText("Thinking…")
        self.thread.start()

    def _text(self, text: str) -> None:
        if self.current is not None:
            self.current.add_text(text)
            if not self._redraw.isActive():
                self._redraw.start()
            self._scroll_to_end()

    def _tool(self, call: assistant.ToolCall) -> None:
        if self.current is not None:
            self.current.add_tool(call)
            self.status.setText(f"Looking at the project ({call.name})…")

    def _done(self, turn: assistant.Turn) -> None:
        if self.current is not None:
            self.current.show_turn(turn)
        if self.project is not None:
            assistant.save(self.project, self.conversation)
        self._finish_thread()
        self._update_status()
        self._scroll_to_end()

    def cancel(self) -> None:
        if self.worker is not None:
            self.worker.stop.set()
        if self.thread is not None:
            self.thread.quit()
            self.thread.wait(10000)
            self._finish_thread()

    def _finish_thread(self) -> None:
        if self.thread is not None:
            self.thread.quit()
            self.thread.wait()
            self.thread.deleteLater()
            self.worker.deleteLater()
        self.thread = self.worker = None
        self.send.setText("Send")

    def _scroll_to_end(self) -> None:
        bar = self.scroll.verticalScrollBar()
        QTimer.singleShot(0, lambda: bar.setValue(bar.maximum()))

    def ask_about(self, code: str, file: str) -> None:
        """Script editor ▸ Ask Assistant About Selection: a question ready to finish and send."""
        self.input.setPlainText(f"About this code from scripts/{file}:\n\n```python\n"
                                f"{code.rstrip()}\n```\n\n")
        cursor = self.input.textCursor()
        cursor.movePosition(cursor.MoveOperation.End)
        self.input.setTextCursor(cursor)
        self.input.setFocus()

    # ---- the key and the prompt -----------------------------------------------------------
    def set_api_key(self) -> None:
        dialog = QDialog(self)
        dialog.setWindowTitle("Anthropic API Key")
        field = QLineEdit()
        field.setEchoMode(QLineEdit.EchoMode.Password)
        field.setPlaceholderText("sk-ant-…")
        note = QLabel("Your own key, from console.anthropic.com. It is kept in your system's "
                      "credential store (not in Midgard's settings or the project). Leave it "
                      "empty to forget the stored key.")
        note.setWordWrap(True)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok |
                                   QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(dialog.accept)
        buttons.rejected.connect(dialog.reject)
        layout = QVBoxLayout(dialog)
        for w in (note, field, buttons):
            layout.addWidget(w)
        if dialog.exec() != QDialog.DialogCode.Accepted:
            return
        try:
            assistant.store_api_key(field.text().strip() or None)
        except Exception as e:  # noqa: BLE001 - no credential store
            QMessageBox.warning(self, "API Key", f"The key could not be stored: {e}\n\nYou can "
                                "set the ANTHROPIC_API_KEY environment variable instead.")
        self.refresh_state()

    def show_system_prompt(self) -> None:
        if self.project is None:
            return
        QInputDialog.getMultiLineText(self, "What's Sent", "Sent with every question (along "
                                      "with the conversation so far and what the tools return):",
                                      assistant.system_prompt(self.project))
