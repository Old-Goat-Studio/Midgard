# SPDX-License-Identifier: Apache-2.0
"""Input actions and their bindings (milestone 6.1; schema/migrations/0003_input.sql, 0022).

Scripts and the game read actions ("jump", "move_y"), never raw keys; each action is bound to
controls on the keyboard, the mouse, a gamepad and the touch screen. The built-in actions (the
player's moving, looking, jumping, running, using, the view, the menu, flying down) are the
engine's; a project adds its own in the Input editor and its scripts read them by id
(midgard.action, down, pressed). Each action has the name players see (the Controls screen,
prompts) and whether players may rebind it.

An action: {"id", "name", "kind": "button" | "axis", "context", "rebindable": bool,
            "bindings": [{"device", "control", "scale"}]}
An axis's bindings carry a direction (scale 1 or -1 for keys and buttons: D right, A left) or
a rate (the mouse's degrees a pixel, a stick's degrees a second).
"""
import re
from pathlib import Path
from typing import Any

from PySide6.QtGui import QUndoCommand

from .project import Project

BUILT_IN = ("move_x", "move_y", "look_x", "look_y", "zoom", "jump", "run", "view", "use",
            "menu", "descend")
KINDS = {"button": "Button (pressed or not)", "axis": "Axis (from -1 to 1)"}
# Contexts (6.1 step 2; migration 23): an action counts only while its context is the game's.
# The game is in "menu" while a menu shows, "flying" while the player flies, else "on_foot"; a
# script can choose another ("vehicle" in a car: midgard.set_input_context).
CONTEXTS = {"always": "Always", "on_foot": "On foot", "flying": "Flying", "menu": "In menus",
            "vehicle": "In vehicles"}
DEVICES = {"keyboard": "Keyboard", "mouse": "Mouse", "gamepad": "Gamepad", "touch": "Touch"}
MOUSE = {"left": "Left button", "right": "Right button", "middle": "Middle button (wheel)",
         "x1": "Back button", "x2": "Forward button", "delta_x": "Moving left and right",
         "delta_y": "Moving up and down", "wheel": "Wheel"}
GAMEPAD = {"a": "A (bottom)", "b": "B (right)", "x": "X (left)", "y": "Y (top)",
           "leftshoulder": "Left bumper", "rightshoulder": "Right bumper",
           "lefttrigger": "Left trigger", "righttrigger": "Right trigger",
           "leftstick": "Left stick (pressed)", "rightstick": "Right stick (pressed)",
           "dpup": "D-pad up", "dpdown": "D-pad down", "dpleft": "D-pad left",
           "dpright": "D-pad right", "back": "Back (View)", "start": "Start (Menu)",
           "guide": "Guide (Home)", "leftx": "Left stick across", "lefty": "Left stick up and down",
           "rightx": "Right stick across", "righty": "Right stick up and down"}
GAMEPAD_AXES = ("leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger")
# The on-screen touch controls' (midgard/ui/touch.h; the touch layout editor adds buttons).
TOUCH = {"stick_x": "Stick across", "stick_y": "Stick up and down",
         "drag_x": "Dragging across", "drag_y": "Dragging up and down", "jump": "Jump button",
         "use": "Use button", "run": "Run button", "view": "View button",
         "descend": "Down button (flying)"}
# The touch controls' layout (6.1 step 3; midgard/touch_layout.h), kept in the game's settings as
# "touch": each button's id (the touch control actions bind), label, place in dp in from the
# safe area's bottom right (sideways; "narrow": upright), size, tap on and off, and when it
# shows: "always", a context, or "pause_menu" (when the game has one: it opens it).
DEFAULT_TOUCH = [
    {"id": "jump", "label": "Jump", "x": 84, "y": 96, "r": 40, "narrow": [74, 92, 38],
     "toggles": False, "shows": "always"},
    {"id": "use", "label": "Use", "x": 186, "y": 62, "r": 30, "narrow": [166, 56, 28],
     "toggles": False, "shows": "always"},
    {"id": "run", "label": "Run", "x": 180, "y": 170, "r": 30, "narrow": [162, 152, 28],
     "toggles": True, "shows": "always"},
    {"id": "view", "label": "View", "x": 78, "y": 206, "r": 26, "narrow": [70, 196, 26],
     "toggles": False, "shows": "always"},
    {"id": "descend", "label": "Down", "x": 84, "y": 290, "r": 30, "narrow": [152, 240, 28],
     "toggles": False, "shows": "flying"},
    {"id": "menu", "label": "Menu", "x": 186, "y": 252, "r": 26, "narrow": [70, 290, 26],
     "toggles": False, "shows": "pause_menu"},
]
TOUCH_SHOWS = {"always": "Always", "on_foot": "On foot", "flying": "Flying",
               "menu": "In menus", "vehicle": "In vehicles",
               "pause_menu": "When the game has a pause menu (opens it)"}
STICK_CONTROLS = {"stick_x": "Stick across", "stick_y": "Stick up and down",
                  "drag_x": "Dragging across", "drag_y": "Dragging up and down"}


def touch_layout(project: Project) -> list[dict[str, Any]]:
    """The touch buttons (Midgard's own unless the game has its own layout)."""
    from . import menus
    found = menus.game_settings(project).get("touch")
    buttons = found.get("buttons") if isinstance(found, dict) else None
    return [dict(b) for b in (buttons if isinstance(buttons, list) else DEFAULT_TOUCH)]


def touch_controls(project: Project | None) -> dict[str, str]:
    """The touch device's controls: the stick, dragging to look, and the layout's buttons."""
    buttons = touch_layout(project) if project is not None else DEFAULT_TOUCH
    return {**STICK_CONTROLS, **{b["id"]: f"{b.get('label', b['id'])} button" for b in buttons}}


def touch_problems(project: Project) -> list[str]:
    """Touch buttons no action uses (pressing them does nothing), and ids used twice."""
    layout = touch_layout(project)
    bound = {b["control"] for a in load_all(project) for b in a["bindings"]
             if b["device"] == "touch"}
    found, seen = [], set()
    for b in layout:
        if b["id"] in seen:
            found.append(f"the touch button '{b['id']}' is there twice")
        seen.add(b["id"])
        if b["id"] not in bound and b.get("shows") != "pause_menu":
            found.append(f"the touch button '{b.get('label', b['id'])}' does nothing: no action "
                         "has it")
    return found


# Continuous controls: their scale is a rate, not a direction.
RATES = {("mouse", "delta_x"), ("mouse", "delta_y"), ("touch", "drag_x"), ("touch", "drag_y")}
ID_PATTERN = re.compile(r"^[a-z][a-z0-9_]{0,39}$")


def control_label(device: str, control: str) -> str:
    """How a control reads: "E", "Left button", "A (bottom)", "Jump button"."""
    names = {"mouse": MOUSE, "gamepad": GAMEPAD,
             "touch": {**TOUCH, **STICK_CONTROLS}}.get(device, {})
    return names.get(control, control)


def binding_label(binding: dict[str, Any], kind: str) -> str:
    """A binding as the editor lists it: "W (+)", "Mouse: Left button", "Gamepad: A (bottom)"."""
    device, control = binding["device"], binding["control"]
    text = control_label(device, control)
    if device != "keyboard":
        text = f"{DEVICES.get(device, device)}: {text}"
    scale = float(binding.get("scale", 1.0))
    if kind == "axis" and (device, control) not in RATES and abs(scale) <= 1.0:
        text += " (+)" if scale > 0 else " (−)"
    return text


def has_tables(project: Project) -> bool:
    with project.connect() as con:
        return con.execute("SELECT 1 FROM sqlite_master WHERE name = 'input_action'").fetchone() \
            is not None


def load_all(project: Project) -> list[dict[str, Any]]:
    """Every action with its bindings: the built-in ones first (in their order), then the
    project's own by name."""
    with project.connect() as con:
        rows = con.execute("SELECT id, name, kind, context, rebindable FROM input_action"
                           ).fetchall()
        bindings = con.execute("SELECT action, device, control, scale FROM input_binding "
                               "ORDER BY rowid").fetchall()
    by_action: dict[str, list] = {}
    for action, device, control, scale in bindings:
        by_action.setdefault(action, []).append(
            {"device": device, "control": control, "scale": float(scale)})
    out = [{"id": r[0], "name": r[1] or r[0], "kind": r[2], "context": r[3] or "always",
            "rebindable": bool(r[4]), "bindings": by_action.get(r[0], [])} for r in rows]

    def order(a):
        return (0, BUILT_IN.index(a["id"]), "") if a["id"] in BUILT_IN else \
            (1, 0, a["name"].lower())
    return sorted(out, key=order)


def load(project: Project, action_id: str) -> dict[str, Any] | None:
    return next((a for a in load_all(project) if a["id"] == action_id), None)


def save(project: Project, action: dict[str, Any]) -> None:
    """An action and its bindings, as given (in place of what was there)."""
    with project.connect() as con:
        con.execute("INSERT INTO input_action (id, name, kind, context, rebindable) "
                    "VALUES (?, ?, ?, ?, ?) ON CONFLICT(id) DO UPDATE SET name = excluded.name, "
                    "kind = excluded.kind, context = excluded.context, "
                    "rebindable = excluded.rebindable",
                    (action["id"], action.get("name", ""), action.get("kind", "button"),
                     action.get("context", "always"), int(action.get("rebindable", True))))
        con.execute("DELETE FROM input_binding WHERE action = ?", (action["id"],))
        for b in action.get("bindings", []):
            con.execute("INSERT OR IGNORE INTO input_binding (action, device, control, scale) "
                        "VALUES (?, ?, ?, ?)",
                        (action["id"], b["device"], b["control"], float(b.get("scale", 1.0))))
    project.input_changed.emit(action["id"])


def delete(project: Project, action_id: str) -> None:
    with project.connect() as con:
        con.execute("DELETE FROM input_binding WHERE action = ?", (action_id,))
        con.execute("DELETE FROM input_action WHERE id = ?", (action_id,))
    project.input_changed.emit(action_id)


def new_id(project: Project, name: str) -> str:
    """An id for a new action from its name ("Honk the horn": "honk_the_horn"), not taken."""
    base = re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")[:32] or "action"
    if not base[0].isalpha():
        base = "action_" + base
    taken = {a["id"] for a in load_all(project)}
    candidate, n = base, 2
    while candidate in taken:
        candidate, n = f"{base}_{n}", n + 1
    return candidate


def overlap(a: str, b: str) -> bool:
    """Whether two contexts can be the game's at once (always is with every one)."""
    return a == b or "always" in (a, b)


def _clashes(a: dict[str, Any], b: dict[str, Any]) -> list[str]:
    """Controls two actions of the same context share (pressing one would do both)."""
    if a["id"] == b["id"] or not overlap(a.get("context", "always"),
                                         b.get("context", "always")):
        return []
    mine = {(x["device"], x["control"]) for x in a.get("bindings", [])}
    shared = mine & {(x["device"], x["control"]) for x in b.get("bindings", [])}
    return [f"'{a['name']}' and '{b['name']}' both use "
            f"{binding_label({'device': d, 'control': c}, 'button')}"
            for d, c in sorted(shared) if (d, c) not in RATES]


def _own_problems(action: dict[str, Any]) -> list[str]:
    found = []
    if not ID_PATTERN.match(action["id"]):
        found.append(f"'{action['id']}' isn't a usable id (small letters, digits and _)")
    if not action.get("bindings"):
        found.append(f"'{action['name']}' has no keys, buttons or touch controls: nothing does it")
    return found


def problems_of(action: dict[str, Any], others: list[dict[str, Any]]) -> list[str]:
    """What's wrong with an action: no bindings, or a control another action in the same
    context also has (pressing it would do both)."""
    return _own_problems(action) + [p for o in others for p in _clashes(action, o)]


def problems(project: Project) -> list[str]:
    """Everything wrong with the project's actions, each clash said once."""
    actions = load_all(project)
    found: list[str] = []
    for i, a in enumerate(actions):
        found += _own_problems(a)
        for other in actions[i + 1:]:
            found += _clashes(a, other)
    return found


_SCRIPT_USE = re.compile(r"midgard\.(action|down|pressed)\(\s*['\"]([^'\"]+)['\"]")


def script_problems(project: Project, scripts: dict[str, Path]) -> list[str]:
    """Actions the scripts read that the project doesn't have: they'd always read nothing."""
    known = {a["id"] for a in load_all(project)}
    found = []
    for module, file in sorted(scripts.items()):
        try:
            text = file.read_text("utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        for line_no, line in enumerate(text.splitlines(), 1):
            for m in _SCRIPT_USE.finditer(line):
                if m.group(2) not in known:
                    found.append(f"Script '{module}' (line {line_no}) reads action "
                                 f"'{m.group(2)}', which the project doesn't have (see the "
                                 "Input editor): it never does anything.")
    return found


# ---- keys, from Qt to the names the game uses (SDL's scancode names) ---------------------------
def key_name(key: int, scan_code: int = 0, text: str = "") -> str | None:
    """The game's name for a key Qt reports (SDL's scancode names: "A", "Space", "Left Shift"),
    or None for one the game can't bind."""
    from PySide6.QtCore import Qt
    k = Qt.Key
    if Qt.Key.Key_A.value <= key <= Qt.Key.Key_Z.value:
        return chr(key)
    if Qt.Key.Key_0.value <= key <= Qt.Key.Key_9.value:
        return chr(key)
    if Qt.Key.Key_F1.value <= key <= Qt.Key.Key_F12.value:
        return f"F{key - Qt.Key.Key_F1.value + 1}"
    # The right-hand modifiers by their scan codes (Windows: 54 right Shift, 0x11D right Ctrl,
    # 0x138 right Alt; X11 adds 8: 62, 105, 108).
    right = scan_code in (54, 0x11D, 0x138, 62, 105, 108)
    named = {k.Key_Space: "Space", k.Key_Return: "Return", k.Key_Enter: "Keypad Enter",
             k.Key_Escape: "Escape", k.Key_Tab: "Tab", k.Key_Backspace: "Backspace",
             k.Key_Delete: "Delete", k.Key_Insert: "Insert", k.Key_Home: "Home",
             k.Key_End: "End", k.Key_PageUp: "PageUp", k.Key_PageDown: "PageDown",
             k.Key_Up: "Up", k.Key_Down: "Down", k.Key_Left: "Left", k.Key_Right: "Right",
             k.Key_Shift: "Right Shift" if right else "Left Shift",
             k.Key_Control: "Right Ctrl" if right else "Left Ctrl",
             k.Key_Alt: "Right Alt" if right else "Left Alt",
             k.Key_CapsLock: "CapsLock", k.Key_Comma: ",", k.Key_Period: ".",
             k.Key_Slash: "/", k.Key_Semicolon: ";", k.Key_Apostrophe: "'",
             k.Key_BracketLeft: "[", k.Key_BracketRight: "]", k.Key_Backslash: "\\",
             k.Key_Minus: "-", k.Key_Equal: "=", k.Key_QuoteLeft: "`"}
    for qt_key, name in named.items():
        if key == qt_key.value:
            return name
    return None


class SetActionCommand(QUndoCommand):
    """An action changed, added (old None) or deleted (new None): one undo step. Edits to the
    same action and field merge (typing its name is one step)."""

    def __init__(self, project: Project, old: dict[str, Any] | None, new: dict[str, Any] | None,
                 text: str, merge_key: str = ""):
        super().__init__(text)
        self.project, self.old, self.new = project, old, new
        self.merge_key = merge_key

    def _apply(self, value: dict[str, Any] | None, other: dict[str, Any] | None) -> None:
        # (Renamed: the old id goes; deleted: it goes; then the new one, as it is.)
        if other is not None and (value is None or value["id"] != other["id"]):
            delete(self.project, other["id"])
        if value is not None:
            save(self.project, value)

    def redo(self) -> None:
        self._apply(self.new, self.old)

    def undo(self) -> None:
        self._apply(self.old, self.new)

    def id(self) -> int:
        return 0x1A70 if self.merge_key else -1

    def mergeWith(self, other) -> bool:  # noqa: N802 (Qt's name)
        if not isinstance(other, SetActionCommand) or not self.merge_key or \
                other.merge_key != self.merge_key:
            return False
        self.new = other.new
        return True
