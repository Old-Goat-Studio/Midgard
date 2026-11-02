# SPDX-License-Identifier: Apache-2.0
"""error.log: what led up to a problem, so a crash can be understood afterwards.

Kept in the open project's folder (with no project open, in Midgard's settings folder), and
written as things happen (so it survives a crash):

- the last 100 events: keys and shortcuts pressed and mouse clicks, each with the time, the
  window and the widget. Typing is recorded only as "typed 12 characters in Name", never the
  text, and nothing typed into a password field is recorded at all;
- errors (the last 50, and no more than 1 MB of them): Python errors no one caught (with their
  traceback), Qt's warnings, the Log's warnings and errors, and the game's while it plays;
- a hard crash (the program killed by a fault, say in the engine): Python's faulthandler writes
  where every thread was at the end of the file.

Edit ▸ Preferences and each project's Project Page can turn it off (for speed): it runs only
when both say on. error.log is left out of Git (the project's .gitignore) and never cooked."""
import datetime
import faulthandler
import logging
import sys
import threading
import traceback
from collections import deque
from pathlib import Path

from PySide6.QtCore import QEvent, QObject, QtMsgType, qInstallMessageHandler
from PySide6.QtGui import QKeySequence
from PySide6.QtWidgets import QAbstractButton, QApplication, QLineEdit, QMenu, QWidget

FILE = "error.log"
EVENTS = 100
ERRORS = 50
ERROR_BYTES = 1 << 20
PREFERENCE = "error_log"      # Settings: on unless turned off
PROJECT_OPTION = "error_log"  # a project's config: on unless turned off


def _now() -> str:
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]


def describe(widget: QWidget | None) -> str:
    """A widget as a person would point at it: 'the "Save" button in Preferences'."""
    if widget is None:
        return "nothing"
    kind = type(widget).__name__.lstrip("Q")
    label = ""
    if isinstance(widget, QAbstractButton) and widget.text():
        label = widget.text().replace("&", "")
    elif widget.accessibleName():
        label = widget.accessibleName()
    elif widget.objectName() and not widget.objectName().startswith("qt_"):
        label = widget.objectName()
    elif widget.toolTip():
        label = widget.toolTip().split("\n")[0][:40]
    window = widget.window().windowTitle() if widget.window() is not None else ""
    text = f'{kind} "{label}"' if label else kind
    return f"{text} in {window}" if window else text


class ErrorLog(QObject):
    """The editor's error.log (one for the editor; it follows the open project)."""

    def __init__(self, settings_dir: Path, parent: QObject | None = None):
        super().__init__(parent)
        self.settings_dir = Path(settings_dir)
        self.folder = self.settings_dir
        self.events: deque[str] = deque(maxlen=EVENTS)
        self.errors: deque[str] = deque(maxlen=ERRORS)
        self.enabled = False
        self._typing: tuple[int, int] | None = None  # (widget id, characters) while typing
        self._crash_file = None
        self._previous_hooks = None
        self._handler = _LogHandler(self)

    @property
    def path(self) -> Path:
        return self.folder / FILE

    # ---- on and off -------------------------------------------------------------------------
    def set_enabled(self, on: bool) -> None:
        if on == self.enabled:
            return
        self.enabled = on
        app = QApplication.instance()
        if on:
            if app is not None:
                app.installEventFilter(self)
            logging.getLogger().addHandler(self._handler)
            editor_log = logging.getLogger("midgard")  # (the game's lines are INFO)
            if editor_log.getEffectiveLevel() > logging.INFO:
                editor_log.setLevel(logging.INFO)
            self._previous_hooks = (sys.excepthook, threading.excepthook)
            sys.excepthook = self._python_error
            threading.excepthook = self._thread_error
            qInstallMessageHandler(self._qt_message)
            self._open_crash_file()
            self.write()
        else:
            if app is not None:
                app.removeEventFilter(self)
            logging.getLogger().removeHandler(self._handler)
            if self._previous_hooks is not None:
                sys.excepthook, threading.excepthook = self._previous_hooks
            qInstallMessageHandler(None)
            self._close_crash_file()

    def set_folder(self, folder: Path | None) -> None:
        """Where error.log goes: a project's folder (None: Midgard's settings folder)."""
        folder = Path(folder) if folder is not None else self.settings_dir
        if folder == self.folder:
            return
        self.folder = folder
        self._ignore_in_git()
        if self.enabled:
            self._open_crash_file()
            self.write()

    def _open_crash_file(self) -> None:
        self._close_crash_file()
        try:
            self.folder.mkdir(parents=True, exist_ok=True)
            # Appending: a fault's report goes after what was last written.
            self._crash_file = open(self.path, "a", encoding="utf-8")  # noqa: SIM115
            faulthandler.enable(file=self._crash_file, all_threads=True)
        except OSError:
            self._crash_file = None

    def _close_crash_file(self) -> None:
        if self._crash_file is not None:
            faulthandler.disable()
            self._crash_file.close()
            self._crash_file = None

    def _ignore_in_git(self) -> None:
        """A project under Git leaves its error.log out."""
        ignore = self.folder / ".gitignore"
        if self.folder != self.settings_dir and ignore.is_file():
            text = ignore.read_text("utf-8")
            if FILE not in text.split():
                ignore.write_text(text + ("" if text.endswith("\n") else "\n") + FILE + "\n",
                                  "utf-8")

    # ---- what it records --------------------------------------------------------------------
    def record_event(self, text: str) -> None:
        self.events.append(f"{_now()}  {text}")
        self.write()  # (at once: a crash may come next)

    def record_error(self, level: str, text: str) -> None:
        self.errors.append(f"{_now()}  {level}: {text.rstrip()}")
        while sum(len(e) for e in self.errors) > ERROR_BYTES and len(self.errors) > 1:
            self.errors.popleft()
        self.write()  # (at once: it may be the last thing written)

    def eventFilter(self, obj: QObject, event: QEvent) -> bool:  # noqa: N802 (Qt's name)
        kind = event.type()
        # (A key goes to the focused widget, then up to its parents if it ignores it: only the
        # focused one, in its own window, counts it.)
        if kind == QEvent.Type.KeyPress and isinstance(obj, QWidget) and \
                obj is obj.window().focusWidget():
            self._key(obj, event)
        elif kind == QEvent.Type.MouseButtonPress and isinstance(obj, QWidget) and \
                event.spontaneous() and obj is QApplication.widgetAt(
                    event.globalPosition().toPoint()):
            self._click(obj, event)
        return False

    def _key(self, widget: QWidget, event) -> None:
        if isinstance(widget, QLineEdit) and widget.echoMode() != QLineEdit.EchoMode.Normal:
            return  # (a password: nothing at all)
        mods = event.modifiers()
        commanding = mods & (mods.ControlModifier | mods.AltModifier | mods.MetaModifier)
        if event.text() and event.text().isprintable() and not commanding:
            # Typing: counted, never kept.
            if self._typing is not None and self._typing[0] == id(widget) and self.events:
                count = self._typing[1] + 1
                self.events[-1] = self.events[-1].rsplit("typed ", 1)[0] + \
                    f"typed {count} characters in {describe(widget)}"
                self._typing = (id(widget), count)
                self.write()
            else:
                self._typing = (id(widget), 1)
                self.record_event(f"typed 1 character in {describe(widget)}")
            return
        self._typing = None
        combo = QKeySequence(event.keyCombination()).toString()
        if combo:
            self.record_event(f"key {combo} in {describe(widget)}")

    def _click(self, widget: QWidget, event) -> None:
        self._typing = None
        button = {1: "left", 2: "right", 4: "middle"}.get(event.button().value, "a")
        target = describe(widget)
        if isinstance(widget, QMenu):
            action = widget.actionAt(event.position().toPoint())
            if action is not None:
                target = f'menu item "{action.text().replace("&", "")}"'
        self.record_event(f"{button} click on {target}")

    def _python_error(self, kind, value, tb) -> None:
        self.record_error("ERROR", "Python: " + "".join(traceback.format_exception(kind, value,
                                                                                    tb)))
        hook = self._previous_hooks[0] if self._previous_hooks else sys.__excepthook__
        hook(kind, value, tb)

    def _thread_error(self, args) -> None:
        self.record_error("ERROR", f"Python (thread {args.thread.name if args.thread else '?'}"
                          "): " + "".join(traceback.format_exception(
                              args.exc_type, args.exc_value, args.exc_traceback)))
        hook = self._previous_hooks[1] if self._previous_hooks else threading.__excepthook__
        hook(args)

    def _qt_message(self, kind, context, message: str) -> None:
        level = {QtMsgType.QtWarningMsg: "WARNING", QtMsgType.QtCriticalMsg: "ERROR",
                 QtMsgType.QtFatalMsg: "FATAL"}.get(kind)
        if level is not None:
            self.record_error(level, f"Qt: {message}")

    # ---- the file ---------------------------------------------------------------------------
    def text(self) -> str:
        from .version import BUILD, VERSION
        lines = [f"Midgard {VERSION}+{BUILD}: error log, written {_now()}", "",
                 f"== The last {EVENTS} events (newest last) =="]
        lines += list(self.events) or ["(none yet)"]
        lines += ["", f"== Errors (the last {ERRORS}, newest last) =="]
        lines += list(self.errors) or ["(none)"]
        return "\n".join(lines) + "\n\n"

    def write(self) -> None:
        """error.log as it is now (in place: a fault's report is appended to the same file)."""
        if not self.enabled:
            return
        try:
            self.folder.mkdir(parents=True, exist_ok=True)
            with open(self.path, "w", encoding="utf-8") as f:
                f.write(self.text())
        except OSError:
            pass  # (nowhere to write: nothing to be done about it here)


class _LogHandler(logging.Handler):
    """The Log's warnings and errors, and the game's, into error.log."""

    def __init__(self, error_log: ErrorLog):
        super().__init__(logging.INFO)
        self.error_log = error_log

    def emit(self, record: logging.LogRecord) -> None:
        try:
            message = record.getMessage()
        except Exception:  # noqa: BLE001 (a badly formatted message is still worth keeping)
            message = str(record.msg)
        if record.levelno >= logging.WARNING:
            self.error_log.record_error(record.levelname, message)
        elif message.startswith("game: ") and any(
                word in message.lower() for word in ("error", "warning", "fatal", "crash")):
            self.error_log.record_error("GAME", message[len("game: "):])
        elif message.startswith("The game exited (") and not message.startswith(
                "The game exited (0)"):
            self.error_log.record_error("GAME", message)
