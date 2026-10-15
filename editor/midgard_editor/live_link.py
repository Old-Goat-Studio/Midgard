# SPDX-License-Identifier: Apache-2.0
"""Play-in-editor (roadmap §2, channel 2): the editor runs the game as a separate process and
talks to it over a TCP live link, so an engine crash never takes the editor down.

    editor                                   midgard_runtime --editor-link 127.0.0.1:<port>
      listens on 127.0.0.1:<port>   <-----   connects, says hello
      entity.put / entity.delete    ----->   applies the edit to the running scene
      row.changed / asset.changed   ----->   re-reads that row / mesh from the package on disk
      pause / resume / stop         ----->
                                    <-----   log lines, stats (about once a second)

Hot reload: entity edits travel in the message itself. Scene settings and meshes are rewritten
in the cooked package first (cook.recook), then named in row.changed / asset.changed. Mesh files
in the project's assets/ folder are watched, so saving one from a modelling tool shows in the
game.

Each message is a 4-byte big-endian length followed by UTF-8 JSON
(schema/live_link.schema.json).
"""
import json
import logging
import struct
from enum import Enum
from pathlib import Path

from PySide6.QtCore import QFileSystemWatcher, QObject, QProcess, QTimer, Signal
from PySide6.QtNetwork import QHostAddress, QTcpServer, QTcpSocket

from .cook import CookError, recook, used_mesh_assets
from .project import Project
from .version import VERSION

log = logging.getLogger("midgard")
PROTOCOL = 0
MAX_MESSAGE = 16 * 1024 * 1024
LOG_LEVELS = {"debug": logging.DEBUG, "info": logging.INFO, "warn": logging.WARNING,
              "error": logging.ERROR}


class LinkError(Exception):
    pass


def encode(message: dict) -> bytes:
    body = json.dumps(message, separators=(",", ":")).encode("utf-8")
    return struct.pack(">I", len(body)) + body


class FrameReader:
    """Splits a byte stream into messages."""

    def __init__(self):
        self.buffer = bytearray()

    def feed(self, data: bytes) -> list[dict]:
        self.buffer += data
        messages = []
        while len(self.buffer) >= 4:
            size = struct.unpack_from(">I", self.buffer)[0]
            if size > MAX_MESSAGE:
                raise LinkError("live-link message too large")
            if len(self.buffer) < 4 + size:
                break
            body = bytes(self.buffer[4:4 + size])
            del self.buffer[:4 + size]
            try:
                message = json.loads(body)
            except ValueError as e:
                raise LinkError(f"malformed live-link message: {e}") from e
            if not isinstance(message, dict) or "type" not in message:
                raise LinkError("malformed live-link message")
            messages.append(message)
        return messages


def entity_message(project: Project, entity_id: str) -> dict:
    """entity.put for an entity of the project, or entity.delete if it has gone."""
    entity = project.entity(entity_id)
    if entity is None:
        return {"type": "entity.delete", "entity": entity_id}
    return {"type": "entity.put", "entity": {"id": entity.id, "name": entity.name,
                                             "parent": entity.parent,
                                             "components": entity.components}}


class State(Enum):
    STOPPED = "stopped"
    STARTING = "starting"
    PLAYING = "playing"
    PAUSED = "paused"


class PlaySession(QObject):
    """One run of the game under the editor: the runtime process and its live link."""

    state_changed = Signal(object)    # State
    stats_received = Signal(dict)     # {"fps": …, "draw_calls": …, "paused": …}
    message_received = Signal(dict)   # every message from the game (tests and tools)

    def __init__(self, project: Project, parent: QObject | None = None):
        super().__init__(parent)
        self.project = project
        self.state = State.STOPPED
        self.server = QTcpServer(self)
        self.server.newConnection.connect(self._accept)
        self.socket: QTcpSocket | None = None
        self.reader = FrameReader()
        self.process: QProcess | None = None
        self.package: Path | None = None
        self._known: set[str] = set()   # entity ids the running game has
        self._cooked_assets: set[str] = set()  # content ids in the package's game.pak
        self.watcher = QFileSystemWatcher(self)
        self.watcher.directoryChanged.connect(self._assets_touched)
        self.watcher.fileChanged.connect(self._assets_touched)
        self._asset_timer = QTimer(self, singleShot=True, interval=300)  # saves come in bursts
        self._asset_timer.timeout.connect(self._reload_assets)
        project.entities_changed.connect(self._entities_changed)
        project.scene_changed.connect(self._scene_changed)

    # ---- control ------------------------------------------------------------------------
    def start(self, runtime: Path | list[str], package: Path) -> None:
        """Starts the game on a cooked package and waits for it to connect. `runtime` is the
        executable, or a command line to put before the runtime's arguments."""
        self.stop()
        if not self.server.listen(QHostAddress.SpecialAddress.LocalHost, 0):
            raise LinkError(f"cannot open the live link: {self.server.errorString()}")
        self._known = {e.id for e in self.project.entities()}
        self.package = package
        self._cooked_assets = self._package_assets()
        self._watch_assets()
        process = QProcess(self)
        process.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
        process.readyReadStandardOutput.connect(self._process_output)
        process.finished.connect(self._process_finished)
        process.errorOccurred.connect(self._process_error)
        self.process = process
        self._set_state(State.STARTING)
        command = [str(runtime)] if isinstance(runtime, Path) else list(runtime)
        process.start(command[0], [*command[1:], "--package", str(package), "--editor-link",
                                   f"127.0.0.1:{self.server.serverPort()}"])

    def stop(self) -> None:
        """Asks the game to quit, and makes sure it does."""
        if self.process is not None:
            process, self.process = self.process, None
            self.send({"type": "stop"})
            if not process.waitForFinished(3000):
                process.kill()
                process.waitForFinished(1000)
            process.deleteLater()
        self._drop_socket()
        self.server.close()
        self._unwatch_assets()
        self._set_state(State.STOPPED)

    def pause(self) -> None:
        if self.state == State.PLAYING:
            self.send({"type": "pause"})
            self._set_state(State.PAUSED)

    def resume(self) -> None:
        if self.state == State.PAUSED:
            self.send({"type": "resume"})
            self._set_state(State.PLAYING)

    def send(self, message: dict) -> bool:
        if self.socket is None or self.socket.state() != QTcpSocket.SocketState.ConnectedState:
            return False
        self.socket.write(encode(message))
        self.socket.flush()
        return True

    @property
    def running(self) -> bool:
        return self.state != State.STOPPED

    # ---- the link -----------------------------------------------------------------------
    def _accept(self) -> None:
        socket = self.server.nextPendingConnection()
        if socket is None:
            return
        if self.socket is not None:  # one game per session
            socket.abort()
            return
        self.socket = socket
        self.reader = FrameReader()
        socket.readyRead.connect(self._read)
        socket.disconnected.connect(self._socket_closed)
        self.send({"type": "hello", "role": "editor", "version": VERSION, "protocol": PROTOCOL})

    def _read(self) -> None:
        if self.socket is None:
            return
        try:
            messages = self.reader.feed(bytes(self.socket.readAll()))
        except LinkError as e:
            log.error("Live link: %s", e)
            self._drop_socket()
            return
        for message in messages:
            self._handle(message)

    def _handle(self, message: dict) -> None:
        kind = message.get("type")
        if kind == "hello":
            if message.get("protocol") != PROTOCOL:
                log.warning("The game speaks live-link protocol %s; this editor speaks %s.",
                            message.get("protocol"), PROTOCOL)
            log.info("Game connected (engine %s).", message.get("version", "?"))
            self._set_state(State.PLAYING)
        elif kind == "log":
            log.log(LOG_LEVELS.get(message.get("level"), logging.INFO), "game: %s",
                    message.get("text", ""))
        elif kind == "state":  # paused or resumed from the game window (F6)
            if self.state in (State.PLAYING, State.PAUSED):
                self._set_state(State.PAUSED if message.get("state") == "paused"
                                else State.PLAYING)
        elif kind == "stats":
            self.stats_received.emit(message)
        self.message_received.emit(message)

    def _drop_socket(self) -> None:
        if self.socket is not None:
            socket, self.socket = self.socket, None
            socket.readyRead.disconnect(self._read)
            socket.disconnected.disconnect(self._socket_closed)
            socket.abort()
            socket.deleteLater()

    def _socket_closed(self) -> None:
        self._drop_socket()
        # The game quits when its link closes; the process watcher finishes the session.
        QTimer.singleShot(3000, self._kill_if_still_running)

    def _kill_if_still_running(self) -> None:
        if self.process is not None and self.socket is None:
            self.stop()

    # ---- the process --------------------------------------------------------------------
    def _process_output(self) -> None:
        if self.process is None:
            return
        text = bytes(self.process.readAllStandardOutput()).decode("utf-8", errors="replace")
        for line in text.splitlines():
            if line.strip():
                log.info("game: %s", line)

    def _process_finished(self, code: int, _status) -> None:
        if self.process is None:
            return  # stopped from the editor
        self._process_output()
        log.info("The game exited (%d).", code)
        self.process.deleteLater()
        self.process = None
        self._drop_socket()
        self.server.close()
        self._unwatch_assets()
        self._set_state(State.STOPPED)

    def _process_error(self, error) -> None:
        if error == QProcess.ProcessError.FailedToStart and self.process is not None:
            log.error("The game could not start: %s", self.process.errorString())
            self.process.deleteLater()
            self.process = None
            self.server.close()
            self._set_state(State.STOPPED)

    # ---- live editing -------------------------------------------------------------------
    def _entities_changed(self, entity_id: str) -> None:
        if self.socket is None:
            return
        # A mesh the game's package doesn't have yet (a new shape, a file from assets/) is
        # cooked into game.pak before the entity that uses it is sent.
        if used_mesh_assets(self.project) - self._cooked_assets:
            self._reload_assets()
        if entity_id:
            self.send(entity_message(self.project, entity_id))
            self._known.add(entity_id)
            return
        # Entities were added or removed: bring the game's set of entities up to date.
        current = {e.id for e in self.project.entities()}
        for gone in sorted(self._known - current):
            self.send({"type": "entity.delete", "entity": gone})
        for added in sorted(current - self._known):
            self.send(entity_message(self.project, added))
        self._known = current

    def _scene_changed(self, scene_id: str) -> None:
        if self.socket is None or self.package is None:
            return
        try:
            recook(self.project, self.package, database=True)
        except (CookError, OSError) as e:
            log.error("Hot reload of the scene settings failed: %s", e)
            return
        self.send({"type": "row.changed", "table": "scene", "key": scene_id})

    def _reload_assets(self) -> None:
        """Re-cooks game.pak and tells the game which meshes changed."""
        if self.socket is None or self.package is None:
            return
        try:
            result, changed = recook(self.project, self.package, assets=True)
        except (CookError, OSError) as e:
            log.error("Hot reload of the assets failed: %s", e)
            return
        for warning in result.warnings:
            log.warning(warning)
        self._cooked_assets = {c["id"] for c in result.manifest["content"]}
        for asset in sorted(changed & self._cooked_assets):
            log.info("Reloading %s", asset)
            self.send({"type": "asset.changed", "id": asset})
        self._watch_assets()

    def _package_assets(self) -> set[str]:
        try:
            manifest = json.loads((self.package / "manifest.json").read_text("utf-8"))
        except (OSError, ValueError, TypeError):
            return set()
        return {c["id"] for c in manifest.get("content", [])}

    def _watch_assets(self) -> None:
        """Watches the project's assets/ folder, its subfolders and its .glb files. Editors
        often replace a file when saving it, which drops it from the watch list, so the list
        is rebuilt after every reload."""
        assets = self.project.root / "assets"
        paths = [assets] + [p for p in assets.rglob("*") if p.is_dir() or
                            p.suffix.lower() == ".glb"] if assets.is_dir() else []
        wanted = {str(p) for p in paths}
        current = set(self.watcher.files()) | set(self.watcher.directories())
        if current - wanted:
            self.watcher.removePaths(sorted(current - wanted))
        if wanted - current:
            self.watcher.addPaths(sorted(wanted - current))

    def _unwatch_assets(self) -> None:
        paths = self.watcher.files() + self.watcher.directories()
        if paths:
            self.watcher.removePaths(paths)
        self._asset_timer.stop()

    def _assets_touched(self, _path: str) -> None:
        if self.socket is not None:
            self._asset_timer.start()

    def _set_state(self, state: State) -> None:
        if state != self.state:
            self.state = state
            self.state_changed.emit(state)
