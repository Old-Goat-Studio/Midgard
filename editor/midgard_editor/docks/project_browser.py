# SPDX-License-Identifier: Apache-2.0
"""The Project panel: the files in the project folder (editor-only .midgard/ is hidden)."""
from pathlib import Path

from PySide6.QtCore import QDir, QModelIndex, QSortFilterProxyModel, QUrl, Signal
from PySide6.QtGui import QDesktopServices
from PySide6.QtWidgets import QFileSystemModel, QLabel, QStackedLayout, QTreeView, QWidget

from ..project import Project


class _HideWorkDir(QSortFilterProxyModel):
    def filterAcceptsRow(self, row: int, parent: QModelIndex) -> bool:
        name = self.sourceModel().index(row, 0, parent).data()
        return name not in (".midgard", ".gitignore")


class ProjectBrowser(QWidget):
    file_selected = Signal(object)  # Path, or None for the project itself

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.model = QFileSystemModel(self)
        self.model.setFilter(QDir.Filter.AllEntries | QDir.Filter.NoDotAndDotDot
                             | QDir.Filter.Hidden)
        self.proxy = _HideWorkDir(self)
        self.proxy.setSourceModel(self.model)
        self.tree = QTreeView()
        self.tree.setModel(self.proxy)
        self.tree.setHeaderHidden(True)
        for column in (1, 2, 3):
            self.tree.hideColumn(column)
        self.tree.doubleClicked.connect(self._open_file)
        self.tree.selectionModel().currentChanged.connect(self._selected)
        self.empty = QLabel("No project open.")
        self.stack = QStackedLayout(self)
        self.stack.addWidget(self.empty)
        self.stack.addWidget(self.tree)

    def set_project(self, project: Project | None) -> None:
        if project is None:
            self.stack.setCurrentWidget(self.empty)
            return
        root = self.model.setRootPath(str(project.root))
        self.tree.setRootIndex(self.proxy.mapFromSource(root))
        self.stack.setCurrentWidget(self.tree)

    def _path(self, index: QModelIndex) -> Path:
        return Path(self.model.filePath(self.proxy.mapToSource(index)))

    def _selected(self, current: QModelIndex, _previous: QModelIndex) -> None:
        self.file_selected.emit(self._path(current) if current.isValid() else None)

    def _open_file(self, index: QModelIndex) -> None:
        path = self._path(index)
        if path.is_file():
            QDesktopServices.openUrl(QUrl.fromLocalFile(str(path)))
