# SPDX-License-Identifier: Apache-2.0
"""Editor settings in midgard.json: ~/.local/share/Midgard on Linux, %APPDATA%\\Midgard on
Windows (QStandardPaths.AppDataLocation with application name "Midgard" and no organisation)."""
import copy
import json
import os
from pathlib import Path
from typing import Any

from PySide6.QtCore import QStandardPaths

MAX_RECENT = 10

DEFAULTS: dict[str, Any] = {
    "theme": None,               # "light", "dark" or "qss:<name>"; None = desktop colours
    "recent_projects": [],       # paths to {project}.config files, newest first
    "layout_last": None,         # {"state": b64, "geometry": b64} saved on exit
    "layouts": {},               # user-saved layouts by name
    "backup_keep": 10,           # rotating project backups to keep
    "backup_minutes": 10,        # minutes between automatic backups while editing
    "use_x11": False,            # Linux: start under XWayland for full multi-monitor support
    "last_project_dir": "",
    "open_last_project": True,   # start with the project worked on last (if it's still there)
    "export_dir": "",            # last Build ▸ Export output folder
    "snippets": [],              # the user's own Script editor snippets
    "new_project_git": True,     # New Project: keep the project in Git (when Git is installed)
    "assistant_enabled": False,  # the Assistant (roadmap §9) is off until the user turns it on
    "assistant_model": "claude-opus-5-5",
    "assistant_effort": "medium",
}


def default_dir() -> Path:
    return Path(QStandardPaths.writableLocation(QStandardPaths.StandardLocation.AppDataLocation))


class Settings:
    """Editor settings, written atomically so a crash never leaves half a file."""

    def __init__(self, directory: Path | None = None):
        self.dir = directory or default_dir()
        self.dir.mkdir(parents=True, exist_ok=True)
        self.path = self.dir / "midgard.json"
        try:
            loaded = json.loads(self.path.read_text(encoding="utf-8"))
            self._data = loaded if isinstance(loaded, dict) else {}
        except (OSError, ValueError):
            self._data = {}

    def get(self, key: str, default: Any = None) -> Any:
        if key in self._data:
            return self._data[key]
        if default is not None:
            return default
        return copy.deepcopy(DEFAULTS.get(key))  # never hand out the shared default objects

    def set(self, key: str, value: Any) -> None:
        self._data[key] = value
        tmp = self.path.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(self._data, indent=2), encoding="utf-8")
        os.replace(tmp, self.path)

    # ---- recent projects ---------------------------------------------------------------
    def recent_projects(self) -> list[Path]:
        """Recent project configs that still exist, newest first."""
        return [Path(p) for p in self.get("recent_projects") if Path(p).is_file()]

    def add_recent(self, config: Path) -> None:
        path = str(Path(config).resolve())
        recent = [p for p in self.get("recent_projects") if p != path]
        self.set("recent_projects", [path, *recent][:MAX_RECENT])

    def clear_recent(self) -> None:
        self.set("recent_projects", [])
