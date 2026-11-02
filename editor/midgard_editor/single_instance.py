# SPDX-License-Identifier: Apache-2.0
"""One editor at a time (per settings folder, so per user).

The first editor holds a lock file in Midgard's settings folder and listens on a local socket.
Starting Midgard again finds the lock taken: it tells the running editor what it was asked to
open (it comes to the front, and opens that project if it's another), then quits. A lock left
by an editor that crashed is noticed (its process is gone) and taken over."""
import contextlib
import hashlib
import json
import sys
from pathlib import Path

from PySide6.QtCore import QLockFile, QObject, Signal
from PySide6.QtNetwork import QLocalServer, QLocalSocket

LOCK_FILE = "midgard.lock"
CONNECT_MS = 3000


def server_name(settings_dir: Path) -> str:
    digest = hashlib.sha1(str(Path(settings_dir).resolve()).lower().encode("utf-8"))
    return f"midgard-editor-{digest.hexdigest()[:16]}"


class SingleInstance(QObject):
    """claim(): True for the first editor (it keeps the lock while this object lives), False
    when another is running (it has been told). The first one's `asked` signal brings what
    later starts asked for: {"project": a {project}.config path, or None}."""
    asked = Signal(dict)

    def __init__(self, settings_dir: Path, parent: QObject | None = None):
        super().__init__(parent)
        self.settings_dir = Path(settings_dir)
        self.name = server_name(self.settings_dir)
        self._lock: QLockFile | None = None
        self._server: QLocalServer | None = None
        self.reached = False  # (a later start: whether the running editor was told)

    def claim(self, project: Path | None = None) -> bool:
        self.settings_dir.mkdir(parents=True, exist_ok=True)
        lock = QLockFile(str(self.settings_dir / LOCK_FILE))
        lock.setStaleLockTime(0)  # (stale only when its editor's process has gone)
        if lock.tryLock(0):
            self._lock = lock
            self._listen()
            return True
        self.reached = self._tell(project)
        return False

    def release(self) -> None:
        if self._server is not None:
            self._server.close()
            self._server = None
        if self._lock is not None:
            self._lock.unlock()
            self._lock = None

    # ---- the first editor -------------------------------------------------------------------
    def _listen(self) -> None:
        QLocalServer.removeServer(self.name)  # (a crashed editor's, on Unix)
        self._server = QLocalServer(self)
        self._server.setSocketOptions(QLocalServer.SocketOption.UserAccessOption)
        if not self._server.listen(self.name):
            self._server = None  # (only the bringing to the front is lost)
            return
        self._server.newConnection.connect(self._connected)

    def _connected(self) -> None:
        while self._server is not None and self._server.hasPendingConnections():
            socket = self._server.nextPendingConnection()
            socket.setParent(self)
            socket.readyRead.connect(lambda s=socket: self._read(s))
            socket.disconnected.connect(socket.deleteLater)
            if socket.bytesAvailable():
                self._read(socket)

    def _read(self, socket: QLocalSocket) -> None:
        data = bytes(socket.readAll()).decode("utf-8", "replace")
        for line in data.splitlines():
            with contextlib.suppress(ValueError):
                message = json.loads(line)
                if isinstance(message, dict):
                    self.asked.emit(message)

    # ---- a later start ----------------------------------------------------------------------
    def _tell(self, project: Path | None) -> bool:
        """Tells the running editor (it may be busy: the message waits for it)."""
        if sys.platform == "win32":  # (this start may hand the front of the screen over)
            with contextlib.suppress(Exception):
                import ctypes
                ctypes.windll.user32.AllowSetForegroundWindow(-1)  # ASFW_ANY
        socket = QLocalSocket()
        socket.connectToServer(self.name)
        if not socket.waitForConnected(CONNECT_MS):
            return False
        message = {"project": str(Path(project).resolve()) if project else None}
        data = (json.dumps(message) + "\n").encode("utf-8")
        written = socket.write(data) == len(data)
        socket.flush()
        if socket.bytesToWrite() > 0:  # (False when there was nothing left to wait for)
            written = socket.waitForBytesWritten(CONNECT_MS) and written
        socket.disconnectFromServer()
        if socket.state() != QLocalSocket.LocalSocketState.UnconnectedState:
            socket.waitForDisconnected(CONNECT_MS)
        return written
