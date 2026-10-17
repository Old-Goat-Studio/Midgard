# SPDX-License-Identifier: Apache-2.0
"""Content-hashed results of the slow steps, importing models and encoding textures (roadmap
§5.5), kept in the project's build/cache/. Each result is filed under a hash of everything it
was made from (the files' contents, the settings, the engine's version), so cooking and
opening the project again only redo what really changed, and a file saved again unchanged
costs nothing.
"""
import hashlib
import json
import logging
import os
import shutil
from pathlib import Path
from typing import Any

from . import engine

log = logging.getLogger(__name__)

FORMAT = 1  # bump when what is kept changes
_file_hashes: dict[tuple, str] = {}


def file_hash(path: Path) -> str:
    """The SHA-256 of a file's contents ("" if it can't be read), remembered while the file
    keeps its time and size."""
    try:
        stat = path.stat()
    except OSError:
        return ""
    key = (str(path), stat.st_mtime_ns, stat.st_size)
    if key not in _file_hashes:
        digest = hashlib.sha256()
        try:
            with open(path, "rb") as f:
                for block in iter(lambda: f.read(1 << 20), b""):
                    digest.update(block)
        except OSError:
            return ""
        _file_hashes[key] = digest.hexdigest()
    return _file_hashes[key]


def engine_stamp() -> Any:
    """What the engine is, so a newer engine's results replace an older one's."""
    try:
        return engine.version()
    except Exception:  # noqa: BLE001 (a broken module: nothing cached is trusted)
        return None


def key_of(*parts: Any) -> str:
    return hashlib.sha256(json.dumps([FORMAT, engine_stamp(), *parts], sort_keys=True,
                                     default=str).encode("utf-8")).hexdigest()


class DiskCache:
    """Results by key, each a folder with meta.json (any JSON) and its blobs (bytes)."""

    def __init__(self, folder: Path):
        self.folder = folder

    def get(self, key: str) -> tuple[Any, dict[str, bytes]] | None:
        entry = self.folder / key
        try:
            meta = json.loads((entry / "meta.json").read_text("utf-8"))
            blobs = {name: (entry / name).read_bytes() for name in meta["blobs"]}
        except (OSError, ValueError, KeyError, TypeError):
            return None
        return meta["data"], blobs

    def put(self, key: str, data: Any, blobs: dict[str, bytes]) -> None:
        """Keeps a result (written beside its place and moved in, so a reader never sees half of
        one). A failure only means it is made again next time."""
        entry = self.folder / key
        if entry.exists():
            return
        tmp = self.folder / f"{key}.{os.getpid()}.tmp"
        try:
            shutil.rmtree(tmp, ignore_errors=True)
            tmp.mkdir(parents=True)
            for name, blob in blobs.items():
                (tmp / name).write_bytes(blob)
            (tmp / "meta.json").write_text(json.dumps({"data": data, "blobs": sorted(blobs)}),
                                           "utf-8")
            os.replace(tmp, entry)
        except OSError as e:
            log.debug("Not cached (%s): %s", key, e)
            shutil.rmtree(tmp, ignore_errors=True)


def cache_dir(project_root: Path, kind: str) -> Path:
    return project_root / "build" / "cache" / kind
