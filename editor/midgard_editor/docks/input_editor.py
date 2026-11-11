# SPDX-License-Identifier: Apache-2.0
"""The Input editor (milestone 6.1): the game's actions and what does each.

A list of the actions on the left (the built-in ones first: moving, looking, jumping, running,
using, the view, the menu, flying down; then the project's own), New Action and Delete; on the
right the chosen one's name (what players see), its id (what scripts read), whether it's a
button or an axis, whether players may rebind it, and its bindings: keys (press the key), mouse
buttons and movement, gamepad buttons and sticks, and the on-screen touch controls. An axis's
keys and buttons each push one way (D right, A left). What's wrong is said under it: no
bindings, or a control another action also has.

Every change is a step on the project's undo stack (typing a name is one step).
"""
import copy
from typing import Any

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QDoubleSpinBox,
    QFormLayout,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMenu,
    QPushButton,
    QSplitter,
    QTableWidget,
    QTableWidgetItem,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

from .. import input_actions as ia
from ..project import Project

ID_ROLE = Qt.ItemDataRole.UserRole


class KeyCaptureDialog(QDialog):
    """Waits for a key (any, Escape too: only the Cancel button stops it)."""

    def __init__(self, action_name: str, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Add a Key")
        self.key: str | None = None
        layout = QVBoxLayout(self)
        self.prompt = QLabel(f"Press the key for <b>{action_name}</b>…")
        layout.addWidget(self.prompt)
        self.note = QLabel("")
        self.note.setWordWrap(True)
        layout.addWidget(self.note)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Cancel)
        buttons.rejected.connect(self.reject)
        for b in buttons.buttons():
            b.setFocusPolicy(Qt.FocusPolicy.NoFocus)  # (Space and Return are keys here too)
        layout.addWidget(buttons)
        self.resize(320, 120)

    def take(self, key: int, scan_code: int = 0) -> bool:
        name = ia.key_name(key, scan_code)
        if name is None:
            self.note.setText("That key can't be used in the game; try another.")
            return False
        self.key = name
        self.accept()
        return True

    def keyPressEvent(self, e) -> None:  # noqa: N802 (Qt's name)
        if not e.isAutoRepeat():
            self.take(e.key(), e.nativeScanCode())


class InputPanel(QWidget):
    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.project: Project | None = None
        self.actions: list[dict[str, Any]] = []
        self._filling = False
        split = QSplitter(Qt.Orientation.Horizontal)
        outer = QHBoxLayout(self)
        outer.addWidget(split)

        left = QWidget()
        left_layout = QVBoxLayout(left)
        left_layout.setContentsMargins(0, 0, 0, 0)
        self.list = QListWidget()
        self.list.currentRowChanged.connect(lambda _r: self.show_action())
        left_layout.addWidget(self.list, 1)
        row = QHBoxLayout()
        self.new_button = QPushButton("New Action")
        self.new_button.clicked.connect(lambda: self.new_action())
        self.delete_button = QPushButton("Delete")
        self.delete_button.clicked.connect(lambda: self.delete_action())
        row.addWidget(self.new_button)
        row.addWidget(self.delete_button)
        left_layout.addLayout(row)
        split.addWidget(left)

        right = QWidget()
        right_layout = QVBoxLayout(right)
        form = QFormLayout()
        self.name = QLineEdit()
        self.name.setToolTip("What players see: the Controls screen, prompts (\"Jump\")")
        self.name.textEdited.connect(lambda t: self._set("name", t.strip() or None, "name"))
        form.addRow("Name", self.name)
        self.id_edit = QLineEdit()
        self.id_edit.setToolTip("Scripts read the action by this id (small letters, digits and "
                                "_): midgard.pressed(\"honk\"). The built-in ones' are fixed")
        self.id_edit.editingFinished.connect(lambda: self.set_id(self.id_edit.text()))
        form.addRow("Id", self.id_edit)
        self.id_label = QLabel()
        self.id_label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        form.addRow("In scripts", self.id_label)
        self.kind = QComboBox()
        for key, label in ia.KINDS.items():
            self.kind.addItem(label, key)
        self.kind.setToolTip("A button is pressed or not (jump); an axis goes from -1 to 1 "
                             "(moving forward and back: W one way, S the other)")
        self.kind.activated.connect(lambda _i: self._set("kind", self.kind.currentData()))
        form.addRow("Kind", self.kind)
        self.context = QComboBox()
        for key, label in ia.CONTEXTS.items():
            self.context.addItem(label, key)
        self.context.setToolTip("When it counts: always, or only on foot, flying, in menus or in "
                                "vehicles (a script says when: midgard.set_input_context). One "
                                "key can do different things in different contexts")
        self.context.activated.connect(lambda _i: self._set("context",
                                                            self.context.currentData()))
        form.addRow("Counts", self.context)
        self.rebindable = QCheckBox("Players can change its keys and buttons")
        self.rebindable.toggled.connect(lambda on: self._set("rebindable", on))
        form.addRow("", self.rebindable)
        right_layout.addLayout(form)
        right_layout.addWidget(QLabel("Keys, buttons and touch controls that do it:"))
        self.bindings = QTableWidget(0, 3)
        self.bindings.setHorizontalHeaderLabels(["Control", "Way", ""])
        self.bindings.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        self.bindings.verticalHeader().setVisible(False)
        self.bindings.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        right_layout.addWidget(self.bindings, 1)
        adds = QHBoxLayout()
        self.add_key = QPushButton("Add Key…")
        self.add_key.setToolTip("Then press the key")
        self.add_key.clicked.connect(lambda: self.capture_key())
        adds.addWidget(self.add_key)
        self.add_menus: dict[str, QToolButton] = {}
        for device in ("mouse", "gamepad", "touch"):
            button = QToolButton()
            button.setText(f"Add {ia.DEVICES[device]}")
            button.setPopupMode(QToolButton.ToolButtonPopupMode.InstantPopup)
            menu = QMenu(button)
            menu.aboutToShow.connect(lambda d=device, m=menu: self._fill_add_menu(d, m))
            button.setMenu(menu)
            adds.addWidget(button)
            self.add_menus[device] = button
        adds.addStretch(1)
        right_layout.addLayout(adds)
        self.problems = QLabel()
        self.problems.setWordWrap(True)
        self.problems.setStyleSheet("color: #c0392b")
        right_layout.addWidget(self.problems)
        split.addWidget(right)
        split.setSizes([260, 520])
        self.right = right
        self.set_project(None)

    # ---- the project ---------------------------------------------------------------------------
    def set_project(self, project: Project | None) -> None:
        if self.project is not None:
            self.project.input_changed.disconnect(self._changed)
        self.project = project
        if project is not None:
            project.input_changed.connect(self._changed)
        self.setEnabled(project is not None and ia.has_tables(project))
        self.reload()

    def _changed(self, _action_id: str) -> None:
        if not self._filling:
            self.reload(self.current_id())

    def reload(self, keep: str | None = None) -> None:
        self.actions = ia.load_all(self.project) if self.project is not None and \
            ia.has_tables(self.project) else []
        self.list.blockSignals(True)
        self.list.clear()
        for a in self.actions:
            built_in = a["id"] in ia.BUILT_IN
            item = QListWidgetItem(f"{a['name']}" + ("" if built_in else "  ★"))
            item.setData(ID_ROLE, a["id"])
            item.setToolTip(f"{a['id']}" + (" (built in)" if built_in else " (this game's own)"))
            self.list.addItem(item)
        self.list.blockSignals(False)
        ids = [a["id"] for a in self.actions]
        row = ids.index(keep) if keep in ids else (0 if ids else -1)
        self.list.setCurrentRow(row)
        self.show_action()

    def current_id(self) -> str | None:
        item = self.list.currentItem()
        return item.data(ID_ROLE) if item is not None else None

    def current(self) -> dict[str, Any] | None:
        return next((a for a in self.actions if a["id"] == self.current_id()), None)

    def choose(self, action_id: str) -> None:
        for i in range(self.list.count()):
            if self.list.item(i).data(ID_ROLE) == action_id:
                self.list.setCurrentRow(i)

    # ---- the chosen action -------------------------------------------------------------------
    def show_action(self) -> None:
        a = self.current()
        self.right.setEnabled(a is not None)
        self.delete_button.setEnabled(a is not None and a["id"] not in ia.BUILT_IN)
        self._filling = True
        try:
            if a is None:
                self.name.clear()
                self.id_edit.clear()
                self.id_label.clear()
                self.bindings.setRowCount(0)
                self.problems.clear()
                return
            if self.name.text().strip() != a["name"]:
                self.name.setText(a["name"])
            self.id_edit.setText(a["id"])
            self.id_edit.setReadOnly(a["id"] in ia.BUILT_IN)
            read = "action" if a["kind"] == "axis" else "pressed"
            self.id_label.setText(f"<code>midgard.{read}(\"{a['id']}\")</code>")
            self.kind.setCurrentIndex(max(0, self.kind.findData(a["kind"])))
            self.kind.setEnabled(a["id"] not in ia.BUILT_IN)
            self.context.setCurrentIndex(max(0, self.context.findData(a["context"])))
            self.rebindable.setChecked(a["rebindable"])
            self._show_bindings(a)
            found = ia.problems_of(a, self.actions)
            self.problems.setText("\n".join(found))
        finally:
            self._filling = False

    def _show_bindings(self, a: dict[str, Any]) -> None:
        table = self.bindings
        table.setRowCount(len(a["bindings"]))
        for row, b in enumerate(a["bindings"]):
            table.setItem(row, 0, QTableWidgetItem(
                ia.binding_label(b, "button") if b["device"] != "keyboard"
                else f"Key: {b['control']}"))
            if a["kind"] == "axis" and (b["device"], b["control"]) in ia.RATES:
                box = QDoubleSpinBox()
                box.setRange(-1000.0, 1000.0)
                box.setDecimals(3)
                box.setValue(float(b.get("scale", 1.0)))
                box.setToolTip("How much it turns: degrees a pixel (the mouse) or a dp (touch)")
                box.editingFinished.connect(lambda r=row, w=box: self._set_scale(r, w.value()))
                table.setCellWidget(row, 1, box)
            elif a["kind"] == "axis":
                way = QComboBox()
                way.addItem("+ (right, forward, up)", 1.0)
                way.addItem("− (left, back, down)", -1.0)
                scale = float(b.get("scale", 1.0))
                if b["device"] == "gamepad" and b["control"] in ia.GAMEPAD_AXES:
                    way.setItemText(0, "+ (as the stick goes)")
                    way.setItemText(1, "− (turned round)")
                way.setCurrentIndex(0 if scale >= 0 else 1)
                way.activated.connect(
                    lambda _i, r=row, w=way: self._set_scale(
                        r, abs(float(a["bindings"][r].get("scale", 1.0)) or 1.0) *
                        w.currentData()))
                table.setCellWidget(row, 1, way)
            else:
                table.removeCellWidget(row, 1)
                table.setItem(row, 1, QTableWidgetItem(""))
            remove = QToolButton()
            remove.setText("✕")
            remove.setToolTip("Remove this one")
            remove.clicked.connect(lambda _c=False, r=row: self.remove_binding(r))
            table.setCellWidget(row, 2, remove)
        table.resizeColumnToContents(1)
        table.resizeColumnToContents(2)

    # ---- changes (each one undo step) ---------------------------------------------------------
    def _push(self, new: dict[str, Any], text: str, merge: str = "") -> None:
        old = self.current()
        if self.project is None or old is None or new == old:
            return
        self._filling = True
        try:
            self.project.undo_stack.push(ia.SetActionCommand(
                self.project, copy.deepcopy(old), copy.deepcopy(new), text,
                merge_key=f"{old['id']}:{merge}" if merge else ""))
        finally:
            self._filling = False
        self.reload(new["id"])

    def _set(self, key: str, value, merge: str = "") -> None:
        a = self.current()
        if self._filling or a is None:
            return
        if key == "name" and value is None:
            return  # (an action keeps a name)
        new = {**copy.deepcopy(a), key: value}
        if key == "kind" and value == "button":  # (a button's bindings push one way)
            for b in new["bindings"]:
                if (b["device"], b["control"]) not in ia.RATES:
                    b["scale"] = 1.0
        label = {"name": "Rename", "kind": "Change the kind of", "context": "Change when",
                 "rebindable": "Change rebinding of"}[key]
        self._push(new, f"{label} action '{a['name']}'", merge)

    def set_id(self, new_id: str) -> bool:
        """The project's own action's id changed (scripts read it by the new one)."""
        a = self.current()
        new_id = new_id.strip()
        if self._filling or a is None or a["id"] in ia.BUILT_IN or new_id == a["id"]:
            return False
        if not ia.ID_PATTERN.match(new_id) or any(o["id"] == new_id for o in self.actions):
            self.problems.setText(f"'{new_id}' can't be its id: small letters, digits and _, "
                                  "starting with a letter, and not another action's.")
            self.id_edit.setText(a["id"])
            return False
        self._push({**copy.deepcopy(a), "id": new_id}, f"Change the id of '{a['name']}' to "
                   f"{new_id}")
        return True

    def _set_scale(self, row: int, scale: float) -> None:
        a = self.current()
        if a is None or not 0 <= row < len(a["bindings"]):
            return
        new = copy.deepcopy(a)
        new["bindings"][row]["scale"] = round(scale, 4)
        self._push(new, f"Change a binding of '{a['name']}'")

    def add_binding(self, device: str, control: str, scale: float | None = None) -> None:
        a = self.current()
        if a is None:
            return
        if any(b["device"] == device and b["control"] == control for b in a["bindings"]):
            return  # (it has it already)
        if scale is None:
            scale = {("mouse", "delta_x"): 0.15, ("mouse", "delta_y"): -0.15,
                     ("touch", "drag_x"): 0.3, ("touch", "drag_y"): -0.3}.get(
                (device, control), 1.0)
        new = copy.deepcopy(a)
        new["bindings"].append({"device": device, "control": control, "scale": scale})
        self._push(new, f"Bind {ia.binding_label({'device': device, 'control': control}, 'button')}"
                        f" to '{a['name']}'")

    def remove_binding(self, row: int) -> None:
        a = self.current()
        if a is None or not 0 <= row < len(a["bindings"]):
            return
        new = copy.deepcopy(a)
        gone = new["bindings"].pop(row)
        self._push(new, f"Unbind {ia.binding_label(gone, 'button')} from '{a['name']}'")

    def key_dialog(self) -> KeyCaptureDialog:
        a = self.current()
        return KeyCaptureDialog(a["name"] if a else "", self)

    def capture_key(self) -> None:
        dialog = self.key_dialog()
        if dialog.exec() and dialog.key:
            self.add_binding("keyboard", dialog.key)

    def _fill_add_menu(self, device: str, menu: QMenu) -> None:
        menu.clear()
        a = self.current()
        names = {"mouse": ia.MOUSE, "gamepad": ia.GAMEPAD, "touch": ia.TOUCH}[device]
        have = {b["control"] for b in a["bindings"] if b["device"] == device} if a else set()
        for control, label in names.items():
            act = menu.addAction(label, lambda c=control: self.add_binding(device, c))
            act.setEnabled(control not in have)

    def new_action(self, name: str = "New action") -> str | None:
        if self.project is None or not ia.has_tables(self.project):
            return None
        action = {"id": ia.new_id(self.project, name), "name": name, "kind": "button",
                  "context": "always", "rebindable": True, "bindings": []}
        self._filling = True
        try:
            self.project.undo_stack.push(ia.SetActionCommand(
                self.project, None, action, f"New action '{name}'"))
        finally:
            self._filling = False
        self.reload(action["id"])
        self.name.setFocus()
        self.name.selectAll()
        return action["id"]

    def delete_action(self) -> bool:
        a = self.current()
        if self.project is None or a is None or a["id"] in ia.BUILT_IN:
            return False
        self._filling = True
        try:
            self.project.undo_stack.push(ia.SetActionCommand(
                self.project, copy.deepcopy(a), None, f"Delete action '{a['name']}'"))
        finally:
            self._filling = False
        self.reload()
        return True
