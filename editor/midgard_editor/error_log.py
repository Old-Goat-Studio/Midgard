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
  where every thread was at the end of the file;
- a freeze: if the editor stops responding for HANG_SECONDS, where every thread was stuck is
  written at the end of the file too (so it is there even if the editor is then closed by
  force), and once it carries on, that becomes a HANG error saying how long it was stuck.

The last session's file is kept as error.previous.log when the editor starts again.

Edit ▸ Preferences and each project's Project Page can turn it off (for speed): it runs only
when both say on. error.log is left out of Git (the project's .gitignore) and never cooked."""
import datetime
import faulthandler
import logging
import sys
import threading
import time
import traceback
from collections import deque
from pathlib import Path

from PySide6.QtCore import QEvent, QObject, QTimer, QtMsgType, qInstallMessageHandler
from PySide6.QtGui import QKeySequence
from PySide6.QtWidgets import QAbstractButton, QApplication, QLineEdit, QMenu, QWidget

FILE = "error.log"
PREVIOUS = "error.previous.log"  # the last session's (kept when the editor starts again)
HANG_SECONDS = 10  # stuck this long: where every thread is, into error.log
HEARTBEAT_MS = 1000
HANG = "HANG"
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
        self._written = 0  # error.log's size as last written (more: a dump appended since)
        self._started: set[Path] = set()  # folders whose last session's file is kept aside
        # The heartbeat: while the window's thread runs, a freeze's dump is put off again.
        self._beat = time.monotonic()
        self._heartbeat = QTimer(self)
        self._heartbeat.setInterval(HEARTBEAT_MS)
        self._heartbeat.timeout.connect(self._tick)

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
            self._keep_previous()
            self._open_crash_file()
            self.write()
            self._beat = time.monotonic()
            self._heartbeat.start()
            self._arm()
        else:
            self._heartbeat.stop()
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
            self._keep_previous()
            self._open_crash_file()
            self.write()
            self._arm()

    def _keep_previous(self) -> None:
        """The first time this session writes in a folder: the error.log already there (the last
        session's, with any crash or freeze at its end) kept as error.previous.log."""
        if self.folder in self._started:
            return
        self._started.add(self.folder)
        try:
            if self.path.is_file() and self.path.stat().st_size > 0:
                self.path.replace(self.folder / PREVIOUS)
        except OSError:
            pass

    def _open_crash_file(self) -> None:
        self._close_crash_file()
        self._written = 0  # (a new file, or another folder's: nothing of it written yet)
        try:
            self.folder.mkdir(parents=True, exist_ok=True)
            # Appending: a fault's report goes after what was last written.
            self._crash_file = open(self.path, "a", encoding="utf-8")  # noqa: SIM115
            faulthandler.enable(file=self._crash_file, all_threads=True)
        except OSError:
            self._crash_file = None

    def _close_crash_file(self) -> None:
        if self._crash_file is not None:
            faulthandler.cancel_dump_traceback_later()  # (not into a closed file)
            faulthandler.disable()
            self._crash_file.close()
            self._crash_file = None

    def _ignore_in_git(self) -> None:
        """A project under Git leaves its error.log out."""
        ignore = self.folder / ".gitignore"
        if self.folder != self.settings_dir and ignore.is_file():
            text = ignore.read_text("utf-8")
            missing = [f for f in (FILE, PREVIOUS) if f not in text.split()]
            if missing:
                ignore.write_text(text + ("" if text.endswith("\n") else "\n") +
                                  "".join(f + "\n" for f in missing), "utf-8")

    # ---- freezes ----------------------------------------------------------------------------
    def _arm(self) -> None:
        """Where every thread is, into error.log, if the window's thread doesn't come back to
        put it off again within HANG_SECONDS."""
        if self._crash_file is not None:
            faulthandler.dump_traceback_later(HANG_SECONDS, repeat=False, file=self._crash_file)

    def _tick(self) -> None:
        now = time.monotonic()
        stuck, self._beat = now - self._beat, now
        self._arm()
        if stuck >= HANG_SECONDS:
            self.write()  # (takes in the dump, as a HANG error)
            if self.errors and HANG in self.errors[-1].split("\n")[0]:
                self.errors[-1] = self.errors[-1].replace(
                    f"for {HANG_SECONDS} seconds or more", f"for {stuck:.0f} seconds", 1)
                self.write()

    def _take_dump(self) -> None:
        """A freeze's dump written at the end of error.log since it was last written: kept as
        an error (the file is about to be written afresh)."""
        try:
            with open(self.path, "rb") as f:
                f.seek(self._written)
                dump = f.read(ERROR_BYTES).decode("utf-8", "replace").strip()
        except OSError:
            return
        if dump:
            self.errors.append(f"{_now()}  {HANG}: the editor stopped responding for "
                               f"{HANG_SECONDS} seconds or more. Where each thread was:\n{dump}")

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
        if self._written and self.path.is_file() and self.path.stat().st_size > self._written:
            self._take_dump()
        data = self.text().encode("utf-8")
        try:
            self.folder.mkdir(parents=True, exist_ok=True)
            with open(self.path, "wb") as f:
                f.write(data)
            self._written = len(data)
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
