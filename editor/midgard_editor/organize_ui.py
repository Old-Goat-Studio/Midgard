# SPDX-License-Identifier: Apache-2.0
"""Organizing a library in its window: the tree of collections and assets, dragging and dropping,
and the Rename, New Collection, Delete Collection, Move To, Move Up, Move Down and Sort A-Z
commands (on a right-click, with keys).

This file is the same in the Master Asset Library (master_library/organize_ui.py) and in Midgard
(midgard_editor/organize_ui.py): change both together. What a change does is library_tree's;
each program gives a Backend (Midgard's also keeps its game working, library_refs)."""
from collections.abc import Callable, Iterable
from pathlib import Path

from PySide6.QtCore import QItemSelectionModel, QMimeData, QObject, Qt, Signal
from PySide6.QtGui import QAction, QFont, QIcon, QKeySequence, QPainter, QPixmap
from PySide6.QtWidgets import (
    QAbstractItemView,
    QApplication,
    QInputDialog,
    QListWidget,
    QMenu,
    QMessageBox,
    QStyle,
    QTreeWidget,
    QTreeWidgetItem,
    QWidget,
)

from . import library_tree as lt

PATH_ROLE = Qt.ItemDataRole.UserRole + 40  # its path in the library ("effects/nox/walk")
KIND_ROLE = Qt.ItemDataRole.UserRole + 41  # "type", "collection" or "asset"
MIME = "application/x-library-paths"       # what is dragged: paths, one a line


def container_of(path: str) -> str:
    return path.rsplit("/", 1)[0]


# ---- the tree ----------------------------------------------------------------------------------
class LibraryTree(QTreeWidget):
    """Collections and assets as a tree, in their order; what is dropped on it is moved."""

    dropped = Signal(list, str, object)  # paths, where to (a type or collection), before (or None)

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.root_path = ""  # what the top level is in (a type), or "" when it shows the types
        self.collapsed: set[str] = set()  # (remembered while the window is open)
        self.setSelectionMode(QAbstractItemView.SelectionMode.ExtendedSelection)
        self.setDragEnabled(True)
        self.setAcceptDrops(True)
        self.setDropIndicatorShown(True)
        self.setDragDropMode(QAbstractItemView.DragDropMode.InternalMove)
        self.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.itemCollapsed.connect(lambda item: self.collapsed.add(item.data(0, PATH_ROLE)))
        self.itemExpanded.connect(lambda item: self.collapsed.discard(item.data(0, PATH_ROLE)))

    # what is chosen
    def chosen(self) -> list[str]:
        """The chosen assets' and collections' paths (types can't be organized)."""
        return [i.data(0, PATH_ROLE) for i in self.selectedItems()
                if i.data(0, KIND_ROLE) in ("asset", "collection")]

    def container(self, item: QTreeWidgetItem | None) -> str:
        """Where an item is: its parent's path, or the top level's."""
        if item is None:
            return self.root_path
        parent = item.parent()
        return parent.data(0, PATH_ROLE) if parent is not None else self.root_path

    def item_for(self, path: str) -> QTreeWidgetItem | None:
        for item in self.findItems("*", Qt.MatchFlag.MatchWildcard |
                                   Qt.MatchFlag.MatchRecursive):
            if item.data(0, PATH_ROLE) == path:
                return item
        return None

    def select_path(self, path: str) -> None:
        item = self.item_for(path)
        if item is not None:
            self.setCurrentItem(item)
            self.scrollToItem(item)

    # dragging
    def mimeTypes(self) -> list[str]:  # noqa: N802 (Qt's name)
        return [*super().mimeTypes(), MIME]

    def mimeData(self, items) -> QMimeData:  # noqa: N802 (Qt's name)
        data = super().mimeData(items)
        data.setData(MIME, "\n".join(self.chosen()).encode("utf-8"))
        return data

    def drop_target(self, item: QTreeWidgetItem | None,
                    where: QAbstractItemView.DropIndicatorPosition) -> tuple[str, str | None]:
        """Where a drop puts things: (the type or collection, the item before which, or None at
        its end)."""
        Position = QAbstractItemView.DropIndicatorPosition
        if item is None or where == Position.OnViewport:
            return self.root_path, None
        kind = item.data(0, KIND_ROLE)
        if where == Position.OnItem and kind in ("type", "collection"):
            return item.data(0, PATH_ROLE), None  # (into it, at its end)
        container = self.container(item)
        name = item.data(0, PATH_ROLE).rsplit("/", 1)[-1]
        if where == Position.AboveItem:
            return container, name
        # Below it (or on an asset): before the one after it.
        parent = item.parent()
        siblings = [parent.child(i) for i in range(parent.childCount())] if parent else \
            [self.topLevelItem(i) for i in range(self.topLevelItemCount())]
        at = siblings.index(item)
        after = siblings[at + 1] if at + 1 < len(siblings) else None
        return container, after.data(0, PATH_ROLE).rsplit("/", 1)[-1] if after else None

    def dropEvent(self, event) -> None:  # noqa: N802 (Qt's name)
        paths = self.chosen() if event.source() is self else []
        item = self.itemAt(event.position().toPoint())
        to, before = self.drop_target(item, self.dropIndicatorPosition())
        event.setDropAction(Qt.DropAction.IgnoreAction)  # (the tree is filled again instead)
        event.accept()
        if paths and to:
            self.dropped.emit(paths, to, before)


class TypeList(QListWidget):
    """The types, down the side: assets dropped on one move into it (between similar types)."""

    dropped = Signal(list, str)  # paths, the type

    def __init__(self, role: int, parent: QWidget | None = None):
        super().__init__(parent)
        self.role = role  # (where each item keeps its type)
        self.setAcceptDrops(True)
        self.setDropIndicatorShown(True)

    def _type_at(self, event) -> str:
        item = self.itemAt(event.position().toPoint())
        return (item.data(self.role) or "") if item is not None else ""

    def dragEnterEvent(self, event) -> None:  # noqa: N802 (Qt's name)
        if event.mimeData().hasFormat(MIME):
            event.acceptProposedAction()
        else:
            event.ignore()

    def dragMoveEvent(self, event) -> None:  # noqa: N802 (Qt's name)
        if event.mimeData().hasFormat(MIME) and self._type_at(event):
            event.acceptProposedAction()
        else:
            event.ignore()

    def dropEvent(self, event) -> None:  # noqa: N802 (Qt's name)
        type_ = self._type_at(event)
        paths = bytes(event.mimeData().data(MIME)).decode("utf-8").split("\n")
        event.setDropAction(Qt.DropAction.IgnoreAction)
        event.accept()
        if type_ and any(paths):
            self.dropped.emit([p for p in paths if p], type_)


def folder_icon(tree: QTreeWidget) -> QIcon:
    """A folder at the height of a line of text, on a strip as wide as the tree's pictures (so
    a collection's row is no taller than its name, and its name lines up with its assets')."""
    side = max(16, tree.fontMetrics().height())
    width = max(side, tree.iconSize().width())
    strip = QPixmap(width, side)
    strip.fill(Qt.GlobalColor.transparent)
    painter = QPainter(strip)
    folder = tree.style().standardIcon(QStyle.StandardPixmap.SP_DirIcon).pixmap(side, side)
    painter.drawPixmap((width - side) // 2, 0, side, side, folder)
    painter.end()
    return QIcon(strip)


def fill(tree: LibraryTree, nodes: Iterable[lt.Node],
         add_asset: Callable[[QTreeWidgetItem | QTreeWidget, lt.Node], QTreeWidgetItem | None],
         keep: Callable[[lt.Node], bool] | None = None) -> None:
    """Fills the tree with nodes (types or what is in one), in order. `add_asset` makes an
    asset's row (its columns are the program's), or None to leave it out; `keep`, when given (a
    search), leaves out assets it says no to, and collections with none of those in them."""
    chosen = set(tree.chosen())
    current = tree.currentItem().data(0, PATH_ROLE) if tree.currentItem() else None
    tree.blockSignals(True)
    tree.clear()
    icon = folder_icon(tree)
    bold = QFont(tree.font())
    bold.setBold(True)

    def add(parent, node: lt.Node) -> QTreeWidgetItem | None:
        if node.kind == "asset":
            if keep is not None and not keep(node):
                return None
            item = add_asset(parent, node)
        else:
            item = QTreeWidgetItem(parent, [node.name])
            item.setIcon(0, icon)
            item.setFont(0, bold)
            for child in node.children:
                add(item, child)
            if keep is not None and item.childCount() == 0:
                (parent.removeChild(item) if isinstance(parent, QTreeWidgetItem) else
                 parent.takeTopLevelItem(parent.indexOfTopLevelItem(item)))
                return None
            item.setExpanded(node.path not in tree.collapsed or keep is not None)
        if item is None:
            return None
        item.setData(0, PATH_ROLE, node.path)
        item.setData(0, KIND_ROLE, node.kind)
        if node.kind == "type":
            item.setFlags(item.flags() & ~Qt.ItemFlag.ItemIsDragEnabled)
        if node.path in chosen:
            item.setSelected(True)
        if node.path == current:
            tree.setCurrentItem(item, 0, QItemSelectionModel.SelectionFlag.NoUpdate)
        return item

    for node in nodes:
        add(tree, node)
    tree.blockSignals(False)


# ---- what organizing does ------------------------------------------------------------------
class Backend:
    """What a program's organizing does: library_tree's, in its library (`root`). Midgard's
    also changes its game to match."""

    def __init__(self, root: Path, update_asset: lt.UpdateAsset | None = None):
        self.root = Path(root)
        self.update_asset = update_asset

    def rename_asset(self, path: str, name: str) -> lt.Moved:
        return lt.rename_asset(self.root, path, name, self.update_asset)

    def rename_collection(self, path: str, name: str) -> lt.Moved:
        return lt.rename_collection(self.root, path, name)

    def move(self, paths: list[str], to: str, index: int | None) -> lt.Moved:
        return lt.move(self.root, paths, to, index, self.update_asset)

    def new_collection(self, parent: str, name: str) -> str:
        return lt.new_collection(self.root, parent, name)

    def delete_collection(self, path: str) -> None:
        lt.delete_collection(self.root, path)

    def move_by(self, path: str, step: int) -> None:
        lt.move_by(self.root, path, step)

    def sort_a_z(self, path: str) -> None:
        lt.sort_a_z(self.root, path)


class Organizer(QObject):
    """The organizing commands for a window's tree (and its type list)."""

    changed = Signal(str)  # something was organized: the path to show chosen ("" for none)

    def __init__(self, window: QWidget, tree: LibraryTree, types: Iterable[tuple[str, str]],
                 types_list: TypeList | None = None):
        super().__init__(window)
        self.window, self.tree = window, tree
        self.type_names = dict(types)  # type -> its name (for Move To)
        self.backend: Backend | None = None
        # Called before each change: the window lets go of the files it shows (a sound playing
        # from a folder stops it being moved, on Windows).
        self.before_change: Callable[[], None] | None = None
        # (tests answer these instead of a person)
        self.ask_text: Callable[[str, str, str], str | None] = self._ask_text
        self.ask_yes: Callable[[str, str], bool] = self._ask_yes
        self.ask_item: Callable[[str, str, list[str]], str | None] = self._ask_item
        self.tell: Callable[[str, str], None] = self._tell

        def action(text: str, run: Callable[[], None], keys: str = "", tip: str = "") -> QAction:
            a = QAction(text, window)
            if keys:
                a.setShortcut(QKeySequence(keys))
                a.setShortcutContext(Qt.ShortcutContext.WidgetWithChildrenShortcut)
            if tip:
                a.setToolTip(tip)
                a.setStatusTip(tip)
            a.triggered.connect(lambda _checked=False: run())
            tree.addAction(a)
            return a
        self.rename_action = action("&Rename…", self.rename, "F2",
                                    "Rename the chosen asset or collection")
        self.new_action = action("&New Collection…", self.new_collection, "Ctrl+Shift+N",
                                 "A new collection, in the chosen one (or where the chosen "
                                 "asset is)")
        self.delete_action = action("&Delete Collection", self.delete_collection, "",
                                    "Delete the chosen collection (only when it holds no "
                                    "assets)")
        self.move_to_action = action("Move &To…", self.move_to, "Ctrl+Shift+M",
                                     "Move what is chosen into another collection or type")
        self.up_action = action("Move &Up", lambda: self.move_by(-1), "Alt+Up")
        self.down_action = action("Move Do&wn", lambda: self.move_by(+1), "Alt+Down")
        self.sort_action = action("&Sort A-Z", self.sort_a_z, "",
                                  "Put what is in the chosen collection (or where the chosen "
                                  "asset is) in A-Z order, collections first")
        tree.customContextMenuRequested.connect(self._menu)
        tree.itemSelectionChanged.connect(self.update_actions)
        tree.dropped.connect(self.drop)
        if types_list is not None:
            types_list.dropped.connect(lambda paths, type_: self.drop(paths, type_, None))
        self.update_actions()

    # ---- state
    def set_backend(self, backend: Backend | None) -> None:
        self.backend = backend
        self.update_actions()

    def _chosen_items(self) -> list[QTreeWidgetItem]:
        return [i for i in self.tree.selectedItems()
                if i.data(0, KIND_ROLE) in ("asset", "collection")]

    def _place(self) -> str:
        """Where a new collection goes, or what Sort A-Z sorts: the chosen collection or type,
        or where the chosen asset is."""
        item = self.tree.currentItem()
        if item is None or not item.isSelected():
            return self.tree.root_path
        if item.data(0, KIND_ROLE) in ("type", "collection"):
            return item.data(0, PATH_ROLE)
        return self.tree.container(item)

    def update_actions(self) -> None:
        on = self.backend is not None
        items = self._chosen_items()
        one = items[0] if len(items) == 1 else None
        kind = one.data(0, KIND_ROLE) if one is not None else None
        self.rename_action.setEnabled(on and one is not None)
        self.new_action.setEnabled(on and bool(self._place()))
        self.delete_action.setEnabled(on and kind == "collection")
        self.move_to_action.setEnabled(on and bool(items))
        self.up_action.setEnabled(on and one is not None)
        self.down_action.setEnabled(on and one is not None)
        self.sort_action.setEnabled(on and bool(self._place()))

    def _menu(self, pos) -> None:
        self.update_actions()
        menu = QMenu(self.tree)
        for a in (self.rename_action, self.new_action, self.delete_action, None,
                  self.move_to_action, self.up_action, self.down_action, self.sort_action):
            menu.addSeparator() if a is None else menu.addAction(a)
        menu.exec(self.tree.viewport().mapToGlobal(pos))

    # ---- doing it
    def _do(self, title: str, work: Callable[[], str | None]) -> bool:
        """Runs a change, says why if it couldn't be made, and shows the library again."""
        if self.backend is None:
            return False
        if self.before_change is not None:
            self.before_change()
        QApplication.setOverrideCursor(Qt.CursorShape.WaitCursor)
        try:
            show = work()
        except lt.OrganizeError as e:
            QApplication.restoreOverrideCursor()
            if type(e).__name__ != "Cancelled":  # (you said no: nothing to say)
                self.tell(title, str(e))
            return False
        QApplication.restoreOverrideCursor()
        self.changed.emit(show or "")
        return True

    def rename(self) -> bool:
        items = self._chosen_items()
        if len(items) != 1:
            return False
        item = items[0]
        path, kind = item.data(0, PATH_ROLE), item.data(0, KIND_ROLE)
        name = self.ask_text("Rename", f"A new name for {item.text(0)}:", item.text(0))
        if name is None or name.strip() in ("", item.text(0)):
            return False

        def work() -> str:
            moved = (self.backend.rename_asset(path, name) if kind == "asset" else
                     self.backend.rename_collection(path, name))
            return moved[0][1] if moved else path
        return self._do("Rename", work)

    def new_collection(self) -> bool:
        place = self._place()
        if not place:
            return False
        name = self.ask_text("New Collection", "Its name:", "")
        if not name or not name.strip():
            return False
        return self._do("New Collection", lambda: self.backend.new_collection(place, name))

    def delete_collection(self) -> bool:
        items = self._chosen_items()
        if len(items) != 1 or items[0].data(0, KIND_ROLE) != "collection":
            return False
        item = items[0]
        if not self.ask_yes("Delete Collection", f"Delete the collection '{item.text(0)}'?"):
            return False
        path = item.data(0, PATH_ROLE)
        return self._do("Delete Collection",
                        lambda: (self.backend.delete_collection(path), container_of(path))[1])

    def places(self) -> list[tuple[str, str]]:
        """Every type and collection, as (path, name shown: "FX / NOX / Footsteps")."""
        out = []

        def walk(node: lt.Node, label: str) -> None:
            out.append((node.path, label))
            for child in node.children:
                if child.kind == "collection":
                    walk(child, f"{label} / {child.name}")
        for type_, name in self.type_names.items():
            for node in lt.tree(self.backend.root, [(type_, name)], []):
                walk(node, name)
        return out

    def move_to(self) -> bool:
        paths = self.tree.chosen()
        if not paths or self.backend is None:
            return False
        kinds = {p.split("/")[0] for p in paths}
        places = [(p, label) for p, label in self.places()
                  if all(lt.similar(k, p.split("/")[0]) for k in kinds)
                  and not any(p == q or p.startswith(q + "/") for q in paths)]
        labels = [label for _p, label in places]
        chosen = self.ask_item("Move To", "Move what is chosen into:", labels)
        if chosen is None or chosen not in labels:
            return False
        return self.drop(paths, places[labels.index(chosen)][0], None)

    def drop(self, paths: list[str], to: str, before: str | None) -> bool:
        """Moves things to a type or collection, before an item there (or at its end)."""
        if not paths or self.backend is None:
            return False
        order = [n.folder_name for n in lt.children(self.backend.root, to)
                 if n.path not in paths]
        index = order.index(before) if before in order else None

        def work() -> str:
            moved = dict(self.backend.move(paths, to, index))
            return moved.get(paths[0], paths[0])
        return self._do("Move", work)

    def move_by(self, step: int) -> bool:
        items = self._chosen_items()
        if len(items) != 1:
            return False
        path = items[0].data(0, PATH_ROLE)
        return self._do("Move", lambda: (self.backend.move_by(path, step), path)[1])

    def sort_a_z(self) -> bool:
        place = self._place()
        if not place:
            return False
        return self._do("Sort A-Z", lambda: (self.backend.sort_a_z(place), "")[1])

    # ---- asking (a person; tests replace these)
    def _ask_text(self, title: str, label: str, text: str) -> str | None:
        value, ok = QInputDialog.getText(self.window, title, label, text=text)
        return value if ok else None

    def _ask_yes(self, title: str, text: str) -> bool:
        return QMessageBox.question(self.window, title, text) == \
            QMessageBox.StandardButton.Yes

    def _ask_item(self, title: str, label: str, items: list[str]) -> str | None:
        value, ok = QInputDialog.getItem(self.window, title, label, items, 0, False)
        return value if ok else None

    def _tell(self, title: str, text: str) -> None:
        QMessageBox.warning(self.window, title, text)
