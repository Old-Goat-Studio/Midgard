# SPDX-License-Identifier: Apache-2.0
"""Assets ▸ Add from Master…: the Master Asset Library's assets (found by a search, or a type),
chosen and copied into the project's own library (milestone 5.13b)."""
import logging

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
)

from .. import asset_library as lib
from .. import master as mst

log = logging.getLogger(__name__)
ROLE = Qt.ItemDataRole.UserRole


def licence_label(code: str) -> str:
    return lib.LICENCES.get(code, (code or "none", "", False))[0]


class AddFromMasterDialog(QDialog):
    def __init__(self, project, master: mst.Master, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Add from the Master Asset Library")
        self.resize(820, 560)
        self.project = project
        self.master = master
        self.added: list[lib.Asset] = []
        self.report = ""
        self.quiet = False  # (tests: the report isn't shown)
        self.assets, unreadable = master.scan()
        have = {a.uid for a in lib.scan(project)[0]}
        layout = QVBoxLayout(self)
        layout.addWidget(QLabel(f"{master.name} ({master.root}): {len(self.assets)} assets. "
                                "Those chosen are copied into this project's library."))
        find = QHBoxLayout()
        self.search = QLineEdit()
        self.search.setPlaceholderText("Search names, descriptions, tags, authors…")
        self.search.setClearButtonEnabled(True)
        self.search.textChanged.connect(lambda _t: self.show_list())
        find.addWidget(self.search, 1)
        self.type = QComboBox()
        self.type.addItem("Every type", "")
        for key, (label, _kinds) in lib.TYPES.items():
            self.type.addItem(label, key)
        self.type.currentIndexChanged.connect(lambda _i: self.show_list())
        find.addWidget(self.type)
        layout.addLayout(find)
        self.tree = QTreeWidget()
        self.tree.setHeaderLabels(["Name", "Type", "Licence", "Version", "Tags"])
        self.tree.setColumnWidth(0, 260)
        self.tree.setSelectionMode(QTreeWidget.SelectionMode.ExtendedSelection)
        self.tree.setRootIsDecorated(False)
        self.tree.itemSelectionChanged.connect(self._chosen)
        self.tree.itemDoubleClicked.connect(lambda *_: self.add_chosen())
        layout.addWidget(self.tree, 1)
        self.info = QLabel()
        self.info.setWordWrap(True)
        layout.addWidget(self.info)
        if unreadable:
            note = QLabel("Not read: " + "; ".join(unreadable))
            note.setWordWrap(True)
            note.setEnabled(False)
            layout.addWidget(note)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        self.add_button = buttons.addButton("Add to Project",
                                            QDialogButtonBox.ButtonRole.ActionRole)
        self.add_button.clicked.connect(self.add_chosen)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        self._have = have
        self.show_list()

    def _matches(self, a: lib.Asset) -> bool:
        type_ = self.type.currentData()
        if type_ and a.type != type_:
            return False
        bought = a.purchase or {}
        text = " ".join([a.name, a.description, " ".join(a.tags), a.author, a.title,
                         a.category, bought.get("seller", ""), bought.get("product", "")])
        return all(w in text.lower() for w in self.search.text().lower().split())

    def show_list(self) -> None:
        self.tree.clear()
        for a in self.assets:
            if not self._matches(a):
                continue
            item = QTreeWidgetItem([a.name, lib.TYPES.get(a.type, (a.type,))[0],
                                    licence_label(a.licence), str(a.version),
                                    ", ".join(a.tags)])
            item.setData(0, ROLE, a.uid)
            if a.uid in self._have:  # (the project has it: not added twice)
                item.setFlags(item.flags() & ~Qt.ItemFlag.ItemIsSelectable)
                item.setText(0, f"{a.name}  (in the project)")
                for c in range(5):
                    item.setForeground(c, Qt.GlobalColor.gray)
            self.tree.addTopLevelItem(item)
        self._chosen()

    def chosen(self) -> list[lib.Asset]:
        ids = {i.data(0, ROLE) for i in self.tree.selectedItems()}
        return [a for a in self.assets if a.uid in ids]

    def choose(self, *names: str) -> None:
        for i in range(self.tree.topLevelItemCount()):
            item = self.tree.topLevelItem(i)
            uid = item.data(0, ROLE)
            item.setSelected(any(a.uid == uid and a.name in names for a in self.assets))

    def _chosen(self) -> None:
        chosen = self.chosen()
        self.add_button.setEnabled(bool(chosen))
        notes = []
        if any(a.licence == lib.PURCHASED for a in chosen):
            notes.append("Purchased: games may use them, but their original files are never "
                         "given away (Commit offers to keep them out of Git).")
        if any(lib.share_alike(a) for a in chosen):
            notes.append("Share-alike: a game that changes them must offer the changed assets "
                         "under the same licence.")
        if len(chosen) == 1 and chosen[0].description:
            notes.insert(0, chosen[0].description)
        self.info.setText(" ".join(notes))

    def add_chosen(self) -> list[lib.Asset]:
        """The chosen assets copied into the project's library."""
        added, problems = [], []
        for a in self.chosen():
            try:
                added.append(mst.add_to_project(self.project, self.master, a))
            except (mst.MasterError, lib.LibraryError, OSError) as e:
                problems.append(f"{a.name}: {e}")
        for a in added:
            log.info("Added '%s' from the Master into the library (%s)", a.name, a.id)
            self._have.add(a.uid)
        self.added += added
        n = len(added)
        self.report = f"Added {n} asset{'' if n == 1 else 's'} to the project's library."
        if problems:
            self.report += "\n\nNot added:\n" + "\n".join(problems)
        self.show_list()
        if not self.quiet:
            QMessageBox.information(self, "Add from Master", self.report)
        return added
