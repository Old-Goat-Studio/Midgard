# SPDX-License-Identifier: Apache-2.0
"""Notices model and image files changing on disk while a project is open (roadmap §5.5).

Every second it looks at the files in the project's objects/ and textures/ folders. When some
changed (saved again from Blender or a paint program, added, or removed), it says so with the
project's signals:
- files_changed, with the changed paths (the Objects panel lists new models; the live link
  cooks them for the running game);
- objects_changed, for each object made from a changed file: it is imported again, and the
  viewport, its thumbnail and the running game show the new version;
- entities_changed, for each entity whose material or mesh uses a changed file.

Looking (rather than asking the operating system to tell) also sees files that editors save
by replacing them, and folders on any disk.
"""
from pathlib import Path

from PySide6.QtCore import QObject, QTimer

from . import textures
from .objects import object_files
from .project import Project

PROJECT_ASSET_PREFIX = "asset:"


class ContentWatcher(QObject):
    def __init__(self, project: Project, interval_ms: int = 1000,
                 parent: QObject | None = None):
        super().__init__(parent)
        self.project = project
        self._stamps = self._scan()
        self.timer = QTimer(self)
        self.timer.setInterval(interval_ms)
        self.timer.timeout.connect(self.check)

    def start(self) -> None:
        self.timer.start()

    def stop(self) -> None:
        self.timer.stop()

    def _scan(self) -> dict[str, tuple[int, int]]:
        stamps = {}
        root = self.project.root
        for folder in (self.project.objects_dir, self.project.textures_dir):
            if not folder.is_dir():
                continue
            for file in folder.rglob("*"):
                relative = file.relative_to(root)
                if any(part.startswith(".") for part in relative.parts):
                    continue
                try:
                    if file.is_file():
                        s = file.stat()
                        stamps[relative.as_posix()] = (s.st_mtime_ns, s.st_size)
                except OSError:  # gone while looking
                    continue
        return stamps

    def check(self) -> set[str]:
        """Looks now; returns the paths (relative to the project folder) that changed."""
        now = self._scan()
        changed = {p for p in now.keys() | self._stamps.keys()
                   if now.get(p) != self._stamps.get(p)}
        self._stamps = now
        if changed:
            self._tell(changed)
        return changed

    def _tell(self, changed: set[str]) -> None:
        project = self.project
        root = project.root
        project.files_changed.emit(sorted(changed))

        def relative(path: Path) -> str:
            try:
                return path.resolve().relative_to(root.resolve()).as_posix()
            except ValueError:
                return ""

        def texture_files(components: dict) -> set[str]:
            return {parsed[0] for t in textures.components_textures(components)
                    if (parsed := textures.parse(t))}

        for obj in project.objects():
            used = {relative(f) for f in object_files(project.objects_dir, obj)} if obj.source \
                else set()
            for material in obj.settings.get("materials", {}).values():
                used |= texture_files({"material": material})
            for part in obj.parts:
                used |= texture_files(part.get("components", {}))
                asset = part.get("components", {}).get("mesh", {}).get("asset", "")
                if asset.startswith(PROJECT_ASSET_PREFIX):
                    used.add("objects/" + asset[len(PROJECT_ASSET_PREFIX):])
            if used & changed:
                project.objects_changed.emit(obj.id)
        for entity in project.entities():
            used = texture_files(entity.components)
            asset = entity.components.get("mesh", {}).get("asset", "")
            if asset.startswith(PROJECT_ASSET_PREFIX):
                used.add("objects/" + asset[len(PROJECT_ASSET_PREFIX):])
            if used & changed:
                project.entities_changed.emit(entity.id)
