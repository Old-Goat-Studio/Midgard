# SPDX-License-Identifier: Apache-2.0
"""The Log panel: messages from the editor (and, from Phase 3, from the running game)."""
import logging

from PySide6.QtCore import QObject, Signal
from PySide6.QtGui import QFontDatabase
from PySide6.QtWidgets import (
    QComboBox,
    QHBoxLayout,
    QPlainTextEdit,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

LOGGER_NAME = "midgard"
LEVELS = {"Debug": logging.DEBUG, "Info": logging.INFO, "Warnings": logging.WARNING,
          "Errors": logging.ERROR}


class _Emitter(QObject):
    record = Signal(int, str)


class QtLogHandler(logging.Handler):
    """Sends log records to the Log panel through a Qt signal (safe from any thread)."""

    def __init__(self):
        super().__init__()
        self.emitter = _Emitter()
        self.setFormatter(logging.Formatter("%(asctime)s  %(levelname)-7s  %(message)s",
                                            "%H:%M:%S"))

    def emit(self, record: logging.LogRecord) -> None:
        self.emitter.record.emit(record.levelno, self.format(record))


class LogPanel(QWidget):
    MAX_LINES = 5000

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.level = QComboBox()
        self.level.addItems(LEVELS)
        self.level.setCurrentText("Info")
        clear = QPushButton("Clear")
        self.text = QPlainTextEdit(readOnly=True)
        self.text.setMaximumBlockCount(self.MAX_LINES)
        self.text.setFont(QFontDatabase.systemFont(QFontDatabase.SystemFont.FixedFont))
        clear.clicked.connect(self.text.clear)

        bar = QHBoxLayout()
        bar.addWidget(self.level)
        bar.addStretch()
        bar.addWidget(clear)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(4, 4, 4, 4)
        layout.addLayout(bar)
        layout.addWidget(self.text)

        self.handler = QtLogHandler()
        self.handler.emitter.record.connect(self.add)
        logger = logging.getLogger(LOGGER_NAME)
        logger.setLevel(logging.DEBUG)
        logger.addHandler(self.handler)
        self.destroyed.connect(lambda: logger.removeHandler(self.handler))

    def add(self, level: int, line: str) -> None:
        if level >= LEVELS[self.level.currentText()]:
            self.text.appendPlainText(line)
