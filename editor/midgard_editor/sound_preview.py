# SPDX-License-Identifier: Apache-2.0
"""Hearing sounds in the editor (milestone 5.14): the Audio panel's Play buttons.

The engine's own mixer, sound events, music and ambience (_midgard.AudioPreview), so what the
editor plays is what the game would. It's given the project's sound files as they are (the
engine reads WAV, FLAC, MP3, Ogg Vorbis and Opus: nothing to cook first) and its events,
playlists and ambience sets, and a timer moves it on while something plays. Without the engine
module, or a sound device, nothing is heard (available() says so).
"""
import json
import logging
from pathlib import Path

from PySide6.QtCore import QObject, QTimer, Signal

from . import ambience, engine, music, sound_events, sounds
from .project import Project

log = logging.getLogger(__name__)
TICK_MS = 30


class SoundPreview(QObject):
    """One per editor: plays a project's sounds as its games would."""
    stopped = Signal()  # nothing is playing any more

    def __init__(self, parent: QObject | None = None, device: bool = True):
        super().__init__(parent)
        module = engine.module()
        self.preview = None
        if module is not None and hasattr(module, "AudioPreview"):
            try:
                self.preview = module.AudioPreview(device=device)
            except RuntimeError as e:
                log.warning("No sound preview: %s", e)
        self._sent: dict[str, tuple[int, int]] = {}  # sound -> (mtime, size) given to it
        self._timer = QTimer(self)
        self._timer.setInterval(TICK_MS)
        self._timer.timeout.connect(self._tick)

    def available(self) -> bool:
        return self.preview is not None

    def _sync(self, project: Project) -> None:
        """The project's sounds, events, playlists and ambience, as they are now."""
        p = self.preview
        events = sound_events.load_all(project)
        for e in events:
            for sound in sound_events.sounds_of(e):
                self._send_sound(project, sound)
            try:
                p.set_event(e["id"], e["name"], json.dumps(e["event"]))
            except RuntimeError as err:
                log.warning("Sound %s: %s", e["name"], err)
        for pl in music.load_all(project):
            try:
                p.set_playlist(pl["id"], pl["name"], json.dumps(pl["playlist"]))
            except RuntimeError as err:
                log.warning("Playlist %s: %s", pl["name"], err)
        for a in ambience.load_all(project):
            try:
                p.set_ambience(a["id"], a["name"], json.dumps(a["ambience"]))
            except RuntimeError as err:
                log.warning("Ambience %s: %s", a["name"], err)

    def _send_sound(self, project: Project, sound: str) -> None:
        relative = sounds.parse(sound)
        if relative is None:
            return
        file = Path(project.root) / relative
        try:
            stat = file.stat()
        except OSError:
            return
        key = (stat.st_mtime_ns, stat.st_size)
        if self._sent.get(sound) != key:
            self.preview.set_sound(sound, file.read_bytes())
            self._sent[sound] = key

    def _start(self) -> None:
        if not self._timer.isActive():
            self._timer.start()

    def play_event(self, project: Project, event_id: str) -> bool:
        """Plays a sound event (one of its sounds, its volume and pitch as the game picks)."""
        if self.preview is None:
            return False
        self._sync(project)
        voice = self.preview.play(event_id)
        for problem in self.preview.problems():
            log.warning("Preview: %s", problem)
        self._start()
        return voice != 0

    def play_playlist(self, project: Project, playlist_id: str) -> bool:
        if self.preview is None:
            return False
        self._sync(project)
        self.preview.play_music(playlist_id)
        self._start()
        return True

    def play_ambience(self, project: Project, set_id: str, time: str = "day") -> bool:
        if self.preview is None:
            return False
        self._sync(project)
        self.preview.play_ambience(set_id, time)
        self._start()
        return True

    def stop(self) -> None:
        if self.preview is not None:
            self.preview.stop_all()

    def playing(self) -> bool:
        p = self.preview
        return p is not None and bool(p.playing_events or p.playlist or p.layers)

    def _tick(self) -> None:
        p = self.preview
        if p is None:
            self._timer.stop()
            return
        if p.has_device:  # (without one, tests mix it themselves)
            p.update(TICK_MS / 1000.0)
        if not self.playing():
            self._timer.stop()
            self.stopped.emit()


_shared: SoundPreview | None = None


def shared() -> SoundPreview:
    """The editor's one preview (made when first needed)."""
    global _shared
    if _shared is None:
        _shared = SoundPreview()
    return _shared
