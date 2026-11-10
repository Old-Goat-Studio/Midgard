# SPDX-License-Identifier: Apache-2.0
"""Screens made in the Menu editor (milestone 6.0): the title screen, menus, pause, game over.

A screen is a row of the screen table (schema/migrations/0021_screen.sql): an id, a name, and
JSON as engine/core/include/midgard/screen.h describes it:

    {"modal": true, "pauses": false, "background": [r, g, b, a],
     "elements": [{"id": "play", "kind": "button", "parent": "", "anchor": "center",
                   "x": 0, "y": 0, "w": 240, "h": 56, "text": "New Game",
                   "style": {...}, "action": {"do": "close"}}, ...]}

Elements are panels (holding others), text, buttons, sliders, checks and pictures, each hung on
an anchor of its parent and placed in dp. Any of them can show a picture from the Asset
Library's GUI section ("image": a "ui:" id, the file's path in the project), stretched over it;
the cook packs the pictures used. Scripts show screens and hear their buttons (midgard.ui.show,
on_click); the game draws them as the Menu editor's canvas does.
"""
import copy
import json
import math
import re
from pathlib import Path
from typing import Any

from PySide6.QtGui import QUndoCommand

from .project import Project

KINDS = {"panel": "Panel", "text": "Text", "button": "Button", "slider": "Slider",
         "check": "Check box", "option": "Option button", "image": "Picture"}
# The pictures each kind has: its "image", then those for its states ("images"), with the
# names the Menu editor gives them. A state left empty falls back (the engine's screen.h).
PICTURES: dict[str, list[tuple[str, str]]] = {
    "button": [("image", "Normal picture"), ("hover", "Hover picture"),
               ("pressed", "Pressed picture"), ("disabled", "Disabled picture")],
    "check": [("unchecked", "Unchecked picture"), ("checked", "Checked picture")],
    "option": [("unchecked", "Unselected picture"), ("checked", "Selected picture")],
    "slider": [("track", "Background picture"), ("pointer", "Pointer picture")],
}
STATES = {kind: [state for state, _label in slots if state != "image"]
          for kind, slots in PICTURES.items()}
IMAGE_PREFIX = "ui:"
IMAGE_SUFFIXES = (".png", ".jpg", ".jpeg")  # (what the game's UI reads: not KTX2)
ANCHORS = {"top_left": "Top left", "top": "Top", "top_right": "Top right", "left": "Left",
           "center": "Middle", "right": "Right", "bottom_left": "Bottom left",
           "bottom": "Bottom", "bottom_right": "Bottom right", "fill": "Fill (stretch)"}
ACTIONS = {"none": "Nothing", "close": "Close this screen", "open": "Open a screen…",
           "quit": "Quit the game", "respawn": "Respawn the player", "event": "Tell scripts only"}
# What the game uses a screen for by itself (engine/core/include/midgard/screen.h).
ROLES = {"": "Only when shown", "title": "Title screen (when the game starts)",
         "pause": "Pause menu (Esc, Start, a phone's Back)", "settings": "Settings",
         "game_over": "Game over (when a stat runs out)"}
BUSES = {"master": "Volume (everything)", "music": "Music volume", "effects": "Effects volume",
         "voice": "Voices volume", "ambience": "Ambience volume", "interface": "Menu sounds volume"}


def _button(eid: str, text: str, y: float, action: dict, **more) -> dict:
    return {"id": eid, "kind": "button", "parent": "panel", "anchor": "top", "x": 0, "y": y,
            "w": 260, "h": 56, "text": text, "action": action, **more}


def _title(text: str, colour: list[float] | None = None) -> dict:
    style = {"font_size": 34, "align": "center", "bold": True}
    if colour:
        style["color"] = colour
    return {"id": "title", "kind": "text", "parent": "panel", "anchor": "top", "x": 0, "y": 24,
            "w": 300, "h": 50, "text": text, "style": style}


def _panel(h: float) -> dict:
    return {"id": "panel", "kind": "panel", "anchor": "center", "x": 0, "y": 0, "w": 340, "h": h}


# The ready-made screens (step 7): what each is called, and the screen (its Settings button
# opens "{settings}", the settings screen's id).
TEMPLATES: dict[str, tuple[str, dict]] = {
    "title": ("Title", {
        "role": "title", "modal": True, "pauses": True, "background": [0.03, 0.04, 0.08, 0.85],
        "elements": [_panel(320), _title("My Game", [1.0, 0.82, 0.48, 1.0]),
                     _button("play", "Play", 96, {"do": "close"}),
                     _button("settings", "Settings", 164, {"do": "open", "screen": "{settings}"}),
                     _button("quit", "Quit", 232, {"do": "quit"})]}),
    "pause": ("Pause", {
        "role": "pause", "modal": True, "pauses": True, "background": [0.0, 0.0, 0.0, 0.55],
        "elements": [_panel(320), _title("Paused"),
                     _button("resume", "Resume", 96, {"do": "close"}),
                     _button("settings", "Settings", 164, {"do": "open", "screen": "{settings}"}),
                     _button("quit", "Quit", 232, {"do": "quit"})]}),
    "settings": ("Settings", {
        "role": "settings", "modal": True, "pauses": True, "background": [0.0, 0.0, 0.0, 0.55],
        "elements": [
            # (it fits a phone held sideways: about 330 dp tall)
            _panel(324), {**_title("Settings"), "y": 12, "h": 44},
            {"id": "volume_label", "kind": "text", "parent": "panel", "anchor": "top", "x": 0,
             "y": 62, "w": 280, "h": 22, "text": "Volume", "style": {"font_size": 16}},
            {"id": "volume", "kind": "slider", "parent": "panel", "anchor": "top", "x": 0,
             "y": 84, "w": 280, "h": 44, "binds": "volume:master"},
            {"id": "music_label", "kind": "text", "parent": "panel", "anchor": "top", "x": 0,
             "y": 134, "w": 280, "h": 22, "text": "Music", "style": {"font_size": 16}},
            {"id": "music", "kind": "slider", "parent": "panel", "anchor": "top", "x": 0,
             "y": 156, "w": 280, "h": 44, "binds": "volume:music"},
            {**_button("jump", "Jump", 210, {"do": "none"}, binds="key:jump"), "x": -72,
             "w": 136, "h": 48},
            {**_button("use", "Use", 210, {"do": "none"}, binds="key:use"), "x": 72,
             "w": 136, "h": 48},
            {**_button("back", "Back", -12, {"do": "close"}), "anchor": "bottom", "h": 48}]}),
    "game_over": ("Game over", {
        "role": "game_over", "stat": "health", "modal": True, "pauses": True,
        "background": [0.25, 0.0, 0.0, 0.6],
        "elements": [_panel(260), _title("You died", [1.0, 0.45, 0.4, 1.0]),
                     _button("again", "Try again", 100, {"do": "respawn"}),
                     _button("quit", "Quit", 168, {"do": "quit"})]}),
}


def from_template(project: Project, kind: str) -> list[dict[str, Any]]:
    """The project's screens with a ready-made one added (and the settings screen its Settings
    button opens, if the project hasn't one). Its id is the last one's (or the one before, with
    settings made too)."""
    screens = copy.deepcopy(load_all(project))
    taken = {s["id"] for s in screens}

    def add(which: str) -> dict:
        name, screen = TEMPLATES[which]
        names = {s["name"].lower() for s in screens}
        unique, n = name, 2
        while unique.lower() in names:
            unique, n = f"{name} {n}", n + 1
        made = {"id": new_id(taken, unique), "name": unique, "screen": copy.deepcopy(screen)}
        if any(s["screen"].get("role") == screen.get("role") for s in screens):
            made["screen"].pop("role", None)  # (one screen per role: this is a spare)
        taken.add(made["id"])
        screens.append(made)
        return made

    settings = next((s for s in screens if s["screen"].get("role") == "settings"), None)
    needs_settings = kind in ("title", "pause") and settings is None
    made = add(kind)
    if needs_settings:
        settings = add("settings")
    for e in made["screen"]["elements"]:
        if e.get("action", {}).get("screen") == "{settings}":
            e["action"]["screen"] = settings["id"] if settings else ""
            if not settings:
                e["action"] = {"do": "none"}
    if needs_settings:  # (the made screen chosen, not its settings)
        screens.remove(made)
        screens.insert(len(screens) - 1, made)
    return screens


def input_actions(project: Project) -> list[str]:
    """The project's button actions (jump, use...): what a key button can rebind."""
    with project.connect() as con:
        try:
            return [a for (a,) in con.execute(
                "SELECT id FROM input_action WHERE kind = 'button' ORDER BY id")]
        except Exception:
            return []
# What each kind starts as when it is added.
NEW_ELEMENT = {
    "panel": {"kind": "panel", "anchor": "center", "x": 0, "y": 0, "w": 400, "h": 300},
    "text": {"kind": "text", "anchor": "center", "x": 0, "y": 0, "w": 300, "h": 40,
             "text": "Text", "style": {"align": "center"}},
    "button": {"kind": "button", "anchor": "center", "x": 0, "y": 0, "w": 240, "h": 56,
               "text": "Button", "action": {"do": "none"}},
    "slider": {"kind": "slider", "anchor": "center", "x": 0, "y": 0, "w": 260, "h": 44,
               "min": 0, "max": 1, "value": 0.5},
    "check": {"kind": "check", "anchor": "center", "x": 0, "y": 0, "w": 260, "h": 44,
              "text": "Check", "value": False},
    "option": {"kind": "option", "anchor": "center", "x": 0, "y": 0, "w": 260, "h": 44,
               "text": "Option", "group": "options", "value": False},
    "image": {"kind": "image", "anchor": "center", "x": 0, "y": 0, "w": 128, "h": 128},
}
# A new screen: a menu in a panel, a title and one button that closes it.
NEW_SCREEN = {"modal": True, "pauses": False, "background": [0.0, 0.0, 0.0, 0.5],
              "elements": [
                  {"id": "panel", "kind": "panel", "anchor": "center", "x": 0, "y": 0,
                   "w": 420, "h": 320},
                  {"id": "title", "kind": "text", "parent": "panel", "anchor": "top", "x": 0,
                   "y": 24, "w": 380, "h": 44, "text": "Menu",
                   "style": {"font_size": 30, "align": "center"}},
                  {"id": "close", "kind": "button", "parent": "panel", "anchor": "bottom",
                   "x": 0, "y": -28, "w": 240, "h": 56, "text": "Close",
                   "action": {"do": "close"}}]}
# The preview sizes: (label, width, height, pixels a dp, safe area left, top, right, bottom).
PREVIEWS = [
    ("Desktop (1280 × 720)", 1280, 720, 1.0, (0, 0, 0, 0)),
    ("Phone, sideways", 1170, 540, 1.5, (90, 0, 90, 40)),
    ("Phone, upright", 540, 1170, 1.5, (0, 90, 0, 60)),
]


def image_ref(project: Project, file: Path) -> str:
    """A picture's id for screens: "ui:" and its path in the project."""
    return IMAGE_PREFIX + Path(file).resolve().relative_to(
        Path(project.root).resolve()).as_posix()


def image_file(project: Project, content: str) -> Path | None:
    """The file a "ui:" id names (None: not one, outside the project, or not there)."""
    if not content.startswith(IMAGE_PREFIX):
        return None
    root = Path(project.root).resolve()
    file = (root / content[len(IMAGE_PREFIX):]).resolve()
    return file if file.is_relative_to(root) and file.is_file() else None


def gui_images(project: Project) -> dict[str, str]:
    """The pictures the Asset Library's GUI section holds, by "ui:" id: their names."""
    from . import asset_library
    return {image_ref(project, path): f"{asset.name} ({path.name})"
            for asset, path in asset_library.files_of(project, ("gui",), IMAGE_SUFFIXES)}


def images_of(screen: dict[str, Any]) -> list[str]:
    """The pictures a screen shows (each once)."""
    out: list[str] = []

    def add(image: Any) -> None:
        if isinstance(image, str) and image and image not in out:
            out.append(image)
    if not isinstance(screen, dict):
        return out
    add(screen.get("image"))
    for e in screen.get("elements", []):
        if isinstance(e, dict):
            add(e.get("image"))
            images = e.get("images", {})
            for image in images.values() if isinstance(images, dict) else []:
                add(image)
    return out


def image_files(project: Project, screens: list[dict[str, Any]]) -> dict[str, str]:
    """The pictures some screens show, by id: their files (those that are there)."""
    files = {}
    for s in screens:
        for image in images_of(s.get("screen", s)):
            if (file := image_file(project, image)) is not None:
                files[image] = str(file)
    return files


def has_table(project: Project) -> bool:
    with project.connect() as con:
        return con.execute(
            "SELECT 1 FROM sqlite_master WHERE name = 'screen'").fetchone() is not None


def load_all(project: Project) -> list[dict[str, Any]]:
    """Every screen, in order: {"id", "name", "screen"} (none before migration 21)."""
    if not has_table(project):
        return []
    with project.connect() as con:
        rows = con.execute("SELECT id, name, screen FROM screen ORDER BY position, name")
        return [{"id": i, "name": n, "screen": json.loads(s)} for i, n, s in rows]


def load(project: Project, screen_id: str) -> dict[str, Any] | None:
    return next((s for s in load_all(project) if s["id"] == screen_id), None)


def save_all(project: Project, screens: list[dict[str, Any]]) -> None:
    """The screens, exactly these, in this order."""
    with project.connect() as con:
        con.execute("DELETE FROM screen")
        con.executemany("INSERT INTO screen (id, name, position, screen) VALUES (?, ?, ?, ?)",
                        [(s["id"], s["name"], i, json.dumps(s["screen"]))
                         for i, s in enumerate(screens)])
    project.screens_changed.emit("")


def new_id(taken: set[str], name: str, prefix: str = "scr_") -> str:
    """An id for something named `name`, unlike those taken."""
    stem = prefix + (re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_") or "screen")
    sid, n = stem, 2
    while sid in taken:
        sid, n = f"{stem}_{n}", n + 1
    return sid


def element_id(screen: dict[str, Any], kind: str) -> str:
    """An id for a new element of a kind: button, button_2..."""
    taken = {e.get("id") for e in screen.get("elements", [])}
    eid, n = kind, 2
    while eid in taken:
        eid, n = f"{kind}_{n}", n + 1
    return eid


def children(screen: dict[str, Any], parent: str) -> list[dict[str, Any]]:
    return [e for e in screen.get("elements", []) if e.get("parent", "") == parent]


def descendants(screen: dict[str, Any], element: str) -> set[str]:
    """An element and everything inside it (a panel's parts, theirs...)."""
    found = {element}
    grew = True
    while grew:
        grew = False
        for e in screen.get("elements", []):
            if e.get("parent", "") in found and e["id"] not in found:
                found.add(e["id"])
                grew = True
    return found


def _colour(value: Any) -> bool:
    return isinstance(value, list) and len(value) in (3, 4) and all(
        isinstance(v, int | float) and not isinstance(v, bool) for v in value)


def problems_of(screen: Any, screen_ids: set[str] | None = None) -> list[str]:
    """What's wrong with a screen, in plain words (as the engine refuses it, and an Open
    button whose screen isn't there)."""
    if not isinstance(screen, dict):
        return ["isn't a set of settings"]
    found = []
    if screen.get("role", "") not in ROLES:
        found.append(f"it has an unknown role '{screen.get('role')}'")
    for key in ("modal", "pauses"):
        if key in screen and not isinstance(screen[key], bool):
            found.append(f"'{key}' isn't true or false")
    if "background" in screen and screen["background"] is not None and \
            not _colour(screen["background"]):
        found.append("its background isn't a colour")
    if screen.get("image") and (not isinstance(screen["image"], str) or
                                not screen["image"].startswith(IMAGE_PREFIX)):
        found.append("its picture isn't one from the Asset Library's GUI section")
    elements = screen.get("elements", [])
    if not isinstance(elements, list):
        return found + ["its elements aren't a list"]
    ids: set[str] = set()
    panels: set[str] = set()
    for e in elements:
        if not isinstance(e, dict) or not e.get("id"):
            found.append("has an element without an id")
            continue
        name = f"element '{e['id']}'"
        if e["id"] in ids:
            found.append(f"has two elements called '{e['id']}'")
        ids.add(e["id"])
        kind = e.get("kind", "text")
        if kind not in KINDS:
            found.append(f"{name} has an unknown kind '{kind}'")
        if e.get("anchor", "top_left") not in ANCHORS:
            found.append(f"{name} has an unknown anchor '{e.get('anchor')}'")
        if e.get("parent") and e["parent"] not in panels:
            found.append(f"{name} is inside '{e['parent']}', which isn't a panel before it")
        for key in ("x", "y", "w", "h", "min", "max", "step"):
            v = e.get(key, 0)
            if isinstance(v, bool) or not isinstance(v, int | float) or not math.isfinite(v):
                found.append(f"{name}'s '{key}' isn't a number")
        action = e.get("action", {}).get("do", "none") if isinstance(e.get("action"), dict) \
            else "none"
        if action not in ACTIONS:
            found.append(f"{name} has an unknown action '{action}'")
        if action == "open":
            target = e["action"].get("screen", "")
            if not target:
                found.append(f"{name} opens a screen but doesn't say which")
            elif screen_ids is not None and target not in screen_ids:
                found.append(f"{name} opens '{target}', which isn't a screen")
        if kind == "slider" and isinstance(e.get("max", 1), int | float) and \
                isinstance(e.get("min", 0), int | float) and e.get("max", 1) <= e.get("min", 0):
            found.append(f"{name}'s most isn't above its least")
        image = e.get("image", "")
        if image and (not isinstance(image, str) or not image.startswith(IMAGE_PREFIX)):
            found.append(f"{name}'s picture isn't one from the Asset Library's GUI section")
        images = e.get("images", {})
        if not isinstance(images, dict):
            found.append(f"{name}'s pictures aren't a set of pictures")
        else:
            for state, picture in images.items():
                if state not in STATES.get(kind, []):
                    found.append(f"{name} is a {KINDS.get(kind, kind).lower()}, which has no "
                                 f"'{state}' picture")
                elif not isinstance(picture, str) or not picture.startswith(IMAGE_PREFIX):
                    found.append(f"{name}'s '{state}' picture isn't one from the Asset "
                                 "Library's GUI section")
        if "enabled" in e and not isinstance(e["enabled"], bool):
            found.append(f"{name}'s 'enabled' isn't true or false")
        binds = e.get("binds", "")
        if binds and not ((kind == "slider" and isinstance(binds, str) and
                           binds.startswith("volume:") and binds[7:] in BUSES) or
                          (kind == "button" and isinstance(binds, str) and
                           binds.startswith("key:") and len(binds) > 4)):
            found.append(f"{name} can't be tied to '{binds}' (a slider to a volume, a button "
                         "to a key)")
        if kind == "panel":
            panels.add(e["id"])
    return found


def problems(project: Project) -> list[str]:
    """Every screen's problems, and names given twice."""
    screens = load_all(project)
    ids = {s["id"] for s in screens}
    found = [f"Screen '{s['name']}' {p}." for s in screens
             for p in problems_of(s["screen"], ids)]
    for s in screens:
        for image in images_of(s["screen"]):
            if image.startswith(IMAGE_PREFIX) and image_file(project, image) is None:
                found.append(f"Screen '{s['name']}' shows the picture "
                             f"'{image[len(IMAGE_PREFIX):]}', which isn't in the project; it "
                             "is drawn without it.")
            elif Path(image).suffix.lower() not in IMAGE_SUFFIXES:
                found.append(f"Screen '{s['name']}' shows '{image[len(IMAGE_PREFIX):]}': "
                             "screens show PNG and JPEG pictures only.")
    seen: set[str] = set()
    roles: dict[str, str] = {}
    for s in screens:
        if s["name"].strip().lower() in seen:
            found.append(f"Two screens are called '{s['name']}'.")
        seen.add(s["name"].strip().lower())
        role = s["screen"].get("role", "")
        if role and role in roles:
            found.append(f"Screens '{roles[role]}' and '{s['name']}' are both the "
                         f"{ROLES.get(role, role).split(' (')[0].lower()}: only the first is "
                         "used.")
        roles.setdefault(role, s["name"])
    return found


def live_message(project: Project) -> list[dict[str, Any]]:
    """The screens for the running game (the live link's screens.put)."""
    return load_all(project)


class SetScreensCommand(QUndoCommand):
    """The Menu editor's edits: every screen, before and after. Edits of one field (typing,
    dragging) merge."""

    ID = 0x5C4E

    def __init__(self, project: Project, old: list[dict], new: list[dict], label: str,
                 merge_key: str = ""):
        super().__init__(label)
        self.project = project
        self.old, self.new = copy.deepcopy(old), copy.deepcopy(new)
        self.merge_key = merge_key

    def id(self) -> int:
        return self.ID

    def mergeWith(self, other: QUndoCommand) -> bool:
        if not isinstance(other, SetScreensCommand) or not self.merge_key or \
                other.merge_key != self.merge_key:
            return False
        self.new = copy.deepcopy(other.new)
        return True

    def redo(self) -> None:
        save_all(self.project, self.new)

    def undo(self) -> None:
        save_all(self.project, self.old)
