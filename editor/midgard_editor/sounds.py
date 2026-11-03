# SPDX-License-Identifier: Apache-2.0
"""Sounds a game uses, from the project's files (milestone 5.14).

A sound is named by a content id, "snd:<path>", its file relative to the project folder: one in
the Asset Library (library/effects/..., library/music/...) or in the project's audio/ folder.
The cook makes each one the game uses smaller for it, with the engine's encoders: Ogg Vorbis
for music and ambience (long and rich), Ogg Opus for everything else (effects, voice, the
interface: short and quick to start), into game.pak under audio/. Each is made once per change
of the file's contents (kept in build/cache/sounds/), and read back with the game's own decoders
before it goes in, so a game never gets a sound it can't play."""
import logging
import threading
from pathlib import Path
from typing import Any

from . import content_cache, engine

log = logging.getLogger(__name__)

PREFIX = "snd:"
FILES = (".wav", ".ogg", ".opus", ".mp3", ".flac")
# Library types (and audio/ subfolders) made Vorbis; every other sound is Opus.
VORBIS_KINDS = ("music", "ambiance", "ambience")


def ref(relative: str) -> str:
    """The content id for a sound file (relative to the project folder)."""
    return PREFIX + Path(relative).as_posix()


def parse(sound: str) -> str | None:
    """The file (relative to the project folder) a "snd:" id names; None for others."""
    return sound[len(PREFIX):] if isinstance(sound, str) and sound.startswith(PREFIX) else None


def codec(sound: str) -> str:
    """'vorbis' for music and ambience, 'opus' for the rest."""
    parts = Path(parse(sound) or "").parts
    kinds = parts[1:2] if parts[:1] in (("library",), ("audio",)) else ()
    return "vorbis" if kinds and kinds[0] in VORBIS_KINDS else "opus"


def pak_path(sound: str) -> str:
    """Where a sound goes in game.pak: audio/<its path>.ogg or .opus."""
    return f"audio/{parse(sound)}.{'ogg' if codec(sound) == 'vorbis' else 'opus'}"


def describe(sound: str) -> str:
    return parse(sound) or sound


def sounds_in(value: Any) -> list[str]:
    """Every "snd:" id in a component's values (however deep), in order, once each."""
    found: list[str] = []

    def walk(v: Any) -> None:
        if isinstance(v, str):
            if v.startswith(PREFIX) and v not in found:
                found.append(v)
        elif isinstance(v, dict):
            for item in v.values():
                walk(item)
        elif isinstance(v, (list, tuple)):
            for item in v:
                walk(item)

    walk(value)
    return found


class SoundCache:
    """Encoded sounds by "snd:" id, made once per change of the file's contents."""

    def __init__(self, size: int = 64):
        self.size = size
        self._items: dict[tuple, bytes] = {}
        self._lock = threading.Lock()
        self.encodes = 0  # sounds actually encoded (for tests)

    def data(self, project_root: Path, sound: str) -> bytes | None:
        """The game's bytes for a sound, or None (not a "snd:" id, a missing file, one that
        can't be read, or no engine module); the reason is logged."""
        relative = parse(sound)
        module = engine.module()
        if relative is None or module is None or not hasattr(module, "encode_sound"):
            return None
        file = Path(project_root) / relative
        try:
            stat = file.stat()
        except OSError:
            return None
        chosen = codec(sound)
        key = (str(file), chosen, stat.st_mtime_ns, stat.st_size)
        with self._lock:
            if key in self._items:
                return self._items[key]
        disk = content_cache.DiskCache(content_cache.cache_dir(Path(project_root), "sounds"))
        content_key = content_cache.key_of("sound", content_cache.file_hash(file), chosen)
        found = disk.get(content_key)
        if found is not None and "sound" in found[1]:
            data = found[1]["sound"]
        else:
            try:
                data = module.encode_sound(file, chosen)
                module.sound_info(data)  # (read back as a game reads it)
            except RuntimeError as e:
                log.warning("Sound %s: %s", relative, e)
                return None
            self.encodes += 1
            disk.put(content_key, {}, {"sound": data})
        with self._lock:
            if key not in self._items and len(self._items) >= self.size:
                self._items.pop(next(iter(self._items)))
            self._items[key] = data
        return data


CACHE = SoundCache()


def sound_data(project_root: Path, sound: str) -> bytes | None:
    return CACHE.data(project_root, sound)
