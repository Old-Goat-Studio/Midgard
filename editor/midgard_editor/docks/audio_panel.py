# SPDX-License-Identifier: Apache-2.0
"""The Audio panel (milestone 5.14): a game's sound, in four tabs.

- Sounds: sound events: their sounds (variations, by weight), bus, volume and pitch ranges,
  looping, cooldown, how many at once, and whether (and how far) they're heard placed in the
  world.
- Music: playlists: their tracks, order, where and when they play, priority, crossfade, gaps.
- Ambience: ambience sets: layers (each at its times of day) and sprinkles, where they play.
- Mixer: the buses' volumes as games start.

Every change is a step on the project's undo stack, and reaches a game playing under the editor
at once (the live link's audio.put). Play buttons preview through the engine's own mixer
(sound_preview.py), so what's heard is what the game plays.
"""
import copy
from collections.abc import Callable
from pathlib import Path
from typing import Any

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QSlider,
    QSpinBox,
    QSplitter,
    QStackedLayout,
    QTableWidget,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from .. import ambience, asset_library, music, scenes, sound_events, sound_preview, sounds
from .. import world_types as world_types_module
from ..project import Project

ID_ROLE = Qt.ItemDataRole.UserRole
TIMES = music.TIMES


def _spin(low: float, high: float, step: float, decimals: int = 2,
          suffix: str = "") -> QDoubleSpinBox:
    box = QDoubleSpinBox()
    box.setRange(low, high)
    box.setSingleStep(step)
    box.setDecimals(decimals)
    box.setKeyboardTracking(False)
    if suffix:
        box.setSuffix(suffix)
    return box


def _row(*widgets: QWidget) -> QWidget:
    w = QWidget()
    layout = QHBoxLayout(w)
    layout.setContentsMargins(0, 0, 0, 0)
    for x in widgets:
        layout.addWidget(x)
    return w


def _pair(value: Any, default: float) -> list[float]:
    """A setting kept as a number or [least, most], as [least, most]."""
    if isinstance(value, (list, tuple)) and len(value) == 2:
        return [float(value[0]), float(value[1])]
    if isinstance(value, (int, float)):
        return [float(value), float(value)]
    return [default, default]


class _ListAndForm(QWidget):
    """A tab: its items listed on the left (New, Delete), the chosen one's settings on the
    right (built by form())."""

    def __init__(self, panel: "AudioPanel", new_label: str):
        super().__init__()
        self.panel = panel
        self.list = QListWidget()
        self.list.currentItemChanged.connect(lambda *_: self.show_chosen())
        self.new_button = QPushButton(new_label)
        self.delete_button = QPushButton("Delete")
        self.delete_button.clicked.connect(self.delete_chosen)
        left = QWidget()
        left_layout = QVBoxLayout(left)
        left_layout.setContentsMargins(0, 0, 0, 0)
        left_layout.addWidget(self.list, 1)
        left_layout.addWidget(_row(self.new_button, self.delete_button))
        self.form_host = QWidget()
        self.form_layout = QVBoxLayout(self.form_host)
        self.form_layout.setContentsMargins(4, 0, 0, 0)
        split = QSplitter()
        split.addWidget(left)
        split.addWidget(self.form_host)
        split.setStretchFactor(1, 1)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(split)
        self.form: QWidget | None = None

    @property
    def project(self) -> Project | None:
        return self.panel.project

    # (each tab says what its items are)
    def items(self) -> list[dict[str, Any]]:
        raise NotImplementedError

    def build_form(self, item: dict[str, Any]) -> QWidget:
        raise NotImplementedError

    def delete_command(self, item: dict[str, Any]):
        raise NotImplementedError

    def chosen_id(self) -> str | None:
        current = self.list.currentItem()
        return current.data(ID_ROLE) if current is not None else None

    def chosen(self) -> dict[str, Any] | None:
        return next((i for i in self.items() if i["id"] == self.chosen_id()), None)

    def refresh(self) -> None:
        keep = self.chosen_id()
        self.list.blockSignals(True)
        self.list.clear()
        if self.project is not None:
            for item in self.items():
                row = QListWidgetItem(item["name"])
                row.setData(ID_ROLE, item["id"])
                self.list.addItem(row)
                if item["id"] == keep:
                    self.list.setCurrentItem(row)
        self.list.blockSignals(False)
        self.show_chosen()

    def choose(self, item_id: str) -> None:
        for i in range(self.list.count()):
            if self.list.item(i).data(ID_ROLE) == item_id:
                self.list.setCurrentRow(i)
                return

    def show_chosen(self) -> None:
        if self.form is not None:
            self.form_layout.removeWidget(self.form)
            self.form.deleteLater()
            self.form = None
        item = self.chosen()
        self.delete_button.setEnabled(item is not None)
        if item is not None:
            self.form = self.build_form(item)
            self.form_layout.addWidget(self.form)

    def delete_chosen(self) -> None:
        item = self.chosen()
        if item is not None and self.project is not None:
            self.project.undo_stack.push(self.delete_command(item))

    def push(self, command) -> None:
        self.project.undo_stack.push(command)


# ---- sounds ----------------------------------------------------------------------------------
class SoundsTab(_ListAndForm):
    def __init__(self, panel: "AudioPanel"):
        super().__init__(panel, "New from Library...")
        self.new_button.setToolTip("A sound event playing a sound from the Asset Library")
        self.new_button.clicked.connect(self.new_from_library)

    def items(self) -> list[dict[str, Any]]:
        return sound_events.load_all(self.project)

    def delete_command(self, item):
        return sound_events.SoundEventCommand(self.project, item, None, "")

    def picker(self):
        from .asset_window import LibraryPicker
        return LibraryPicker(self.project, "A Sound from the Library",
                             ("effects", "ambiance", "music"), asset_library.AUDIO, self)

    def new_from_library(self) -> None:
        picker = self.picker()
        if picker.exec() and picker.chosen is not None:
            self.add_from_file(picker.chosen)

    def add_from_file(self, file: Path) -> str:
        """A new sound event for a library file (one step). Returns its id."""
        event = sound_events.event_for_sound(self.project, file)
        self.push(sound_events.SoundEventCommand(self.project, None, event, ""))
        self.choose(event["id"])
        return event["id"]

    def change(self, item: dict, key: str, value: Any, label: str) -> None:
        """One setting of an event (in its "event" settings, or "name"), as a step."""
        new = copy.deepcopy(item)
        if key == "name":
            new["name"] = value
        elif value is None:
            new["event"].pop(key, None)
        else:
            new["event"][key] = value
        if new != item:
            self.push(sound_events.SoundEventCommand(self.project, item, new, label, key))

    def build_form(self, item: dict[str, Any]) -> QWidget:
        e = item["event"]
        w = QWidget()
        form = QFormLayout(w)
        current = lambda: sound_events.load(self.project, item["id"]) or item  # noqa: E731
        name = QLineEdit(item["name"])
        name.editingFinished.connect(
            lambda: name.text().strip() and self.change(current(), "name", name.text().strip(),
                                                        "name"))
        form.addRow("Name", name)
        form.addRow(QLabel(f"<small>Scripts play it as \"{item['name']}\" or "
                           f"\"{item['id']}\".</small>"))
        # Its sounds (variations, by weight).
        table = QTableWidget(0, 2)
        table.setHorizontalHeaderLabels(["Sound", "Weight"])
        header = table.horizontalHeader()
        header.setStretchLastSection(False)
        header.setSectionResizeMode(0, header.ResizeMode.Stretch)
        table.setMinimumHeight(90)
        variants = e.get("sounds", [])
        for v in variants:
            sound = v if isinstance(v, str) else v.get("sound", "")
            weight = 1.0 if isinstance(v, str) else float(v.get("weight", 1.0))
            r = table.rowCount()
            table.insertRow(r)
            label = QLabel(Path(sounds.describe(sound)).name)
            label.setToolTip(sounds.describe(sound))
            table.setCellWidget(r, 0, label)
            box = _spin(0.0, 100.0, 0.5, 1)
            box.setValue(weight)
            box.valueChanged.connect(lambda val, i=r: self._set_weight(current(), i, val))
            table.setCellWidget(r, 1, box)
        self.sounds_table = table
        add = QPushButton("Add Sound...")
        add.setToolTip("Another variation, from the Asset Library: one is picked each time, "
                       "by weight")
        add.clicked.connect(lambda: self._add_variant(current()))
        remove = QPushButton("Remove")
        remove.clicked.connect(lambda: self._remove_variant(current(), table.currentRow()))
        form.addRow("Sounds", table)
        form.addRow("", _row(add, remove))
        # How it plays.
        bus = QComboBox()
        for key, label in sound_events.BUSES.items():
            bus.addItem(label, key)
        bus.setCurrentIndex(max(bus.findData(e.get("bus", "effects")), 0))
        bus.currentIndexChanged.connect(
            lambda _i: self.change(current(), "bus", bus.currentData(), "bus"))
        form.addRow("Bus", bus)
        for key, label, low, high, step, tip in (
                ("volume", "Volume", 0.0, 4.0, 0.05, "Picked between these each time it plays"),
                ("pitch", "Pitch", 0.1, 4.0, 0.05, "2 an octave up; picked between these")):
            lo, hi = _pair(e.get(key), 1.0)
            a, b = _spin(low, high, step), _spin(low, high, step)
            a.setValue(lo)
            b.setValue(hi)
            for box in (a, b):
                box.setToolTip(tip)
                box.valueChanged.connect(lambda _v, k=key, x=a, y=b: self.change(
                    current(), k, [round(x.value(), 3), round(y.value(), 3)], k))
            form.addRow(label, _row(a, QLabel("to"), b))
        loop = QCheckBox("Loops")
        loop.setChecked(bool(e.get("loop", False)))
        loop.toggled.connect(lambda on: self.change(current(), "loop", on or None, "loop"))
        form.addRow("", loop)
        cooldown = _spin(0.0, 600.0, 0.1, 2, " s")
        cooldown.setValue(float(e.get("cooldown", 0.0)))
        cooldown.setToolTip("It won't play again until this long has passed (0: any time)")
        cooldown.valueChanged.connect(
            lambda v: self.change(current(), "cooldown", round(v, 3) or None, "cooldown"))
        most = QSpinBox()
        most.setRange(0, 64)
        most.setValue(int(e.get("max_voices", 0)))
        most.setSpecialValueText("any number")
        most.setToolTip("At most this many at once: the oldest stops for a new one")
        most.valueChanged.connect(
            lambda v: self.change(current(), "max_voices", v or None, "at most"))
        form.addRow("Waits", _row(cooldown, QLabel("at most"), most))
        spatial = QCheckBox("Heard from where it is (3D)")
        spatial.setChecked(bool(e.get("spatial", True)))
        spatial.setToolTip("Off: heard the same everywhere (music, a radio in the interface)")
        spatial.toggled.connect(
            lambda on: self.change(current(), "spatial", None if on else False, "placed"))
        form.addRow("", spatial)
        near, far = _spin(0.01, 10000.0, 0.5, 2, " m"), _spin(0.01, 10000.0, 1.0, 1, " m")
        near.setValue(float(e.get("min_distance", 1.0)))
        far.setValue(float(e.get("max_distance", 40.0)))
        near.setToolTip("All of it within this; half as loud at twice this distance")
        far.setToolTip("Silent beyond this")
        near.valueChanged.connect(
            lambda v: self.change(current(), "min_distance", round(v, 3), "heard from"))
        far.valueChanged.connect(
            lambda v: self.change(current(), "max_distance", round(v, 3), "heard within"))
        form.addRow("Full within", _row(near, QLabel("silent beyond"), far))
        form.addRow(self.panel.play_row(lambda: self.panel.preview.play_event(
            self.project, item["id"])))
        return w

    def _variants(self, item: dict) -> list[dict]:
        return [{"sound": v, "weight": 1} if isinstance(v, str) else dict(v)
                for v in item["event"].get("sounds", [])]

    def _set_weight(self, item: dict, row: int, weight: float) -> None:
        variants = self._variants(item)
        if 0 <= row < len(variants):
            variants[row]["weight"] = round(weight, 3)
            self.change(item, "sounds", variants, "weight")

    def _add_variant(self, item: dict) -> None:
        picker = self.picker()
        if picker.exec() and picker.chosen is not None:
            self.add_variant(item, picker.chosen)

    def add_variant(self, item: dict, file: Path) -> None:
        root = Path(self.project.root).resolve()
        sound = sounds.ref(Path(file).resolve().relative_to(root).as_posix())
        self.change(item, "sounds", self._variants(item) + [{"sound": sound, "weight": 1}],
                    "sounds")
        self.show_chosen()

    def _remove_variant(self, item: dict, row: int) -> None:
        variants = self._variants(item)
        if 0 <= row < len(variants):
            del variants[row]
            self.change(item, "sounds", variants, "sounds")
            self.show_chosen()


# ---- where (and when): music's and ambience's conditions ------------------------------------
class WhereEditor(QWidget):
    """Scenes, world types and biomes to tick (none ticked: anywhere), and times of day."""

    def __init__(self, project: Project, settings: dict, on_change: Callable[[str, list], None],
                 times: bool = True):
        super().__init__()
        form = QFormLayout(self)
        form.setContentsMargins(0, 0, 0, 0)
        self.lists: dict[str, QListWidget] = {}
        biome_names = sorted({b.get("name", "")
                              for wt in world_types_module.load_all(project)
                              for b in (world_types_module.recipe(project, wt["id"]) or {})
                              .get("terrain", {}).get("biomes", []) if b.get("name")}
                             | set(settings.get("biomes", [])))
        choices = {
            "scenes": [(s["id"], s["name"]) for s in scenes.load_all(project)],
            "world_types": [(w["id"], w["name"]) for w in world_types_module.load_all(project)],
            "biomes": [(b, b) for b in biome_names],
        }
        labels = {"scenes": "In scenes", "world_types": "World types", "biomes": "Biomes"}
        for key, options in choices.items():
            box = QListWidget()
            box.setMaximumHeight(72)
            box.setToolTip("None ticked: anywhere")
            for value, label in options:
                row = QListWidgetItem(label)
                row.setData(ID_ROLE, value)
                row.setFlags(row.flags() | Qt.ItemFlag.ItemIsUserCheckable)
                row.setCheckState(Qt.CheckState.Checked if value in settings.get(key, [])
                                  else Qt.CheckState.Unchecked)
                box.addItem(row)
            box.itemChanged.connect(lambda _r, k=key, b=box: on_change(k, [
                b.item(i).data(ID_ROLE) for i in range(b.count())
                if b.item(i).checkState() == Qt.CheckState.Checked]))
            self.lists[key] = box
            form.addRow(labels[key], box)
        self.times: dict[str, QCheckBox] = {}
        if times:
            row = []
            for key, label in TIMES.items():
                check = QCheckBox(label)
                check.setChecked(key in settings.get("times", []))
                check.toggled.connect(lambda _on: on_change("times", [
                    k for k, c in self.times.items() if c.isChecked()]))
                self.times[key] = check
                row.append(check)
            form.addRow("At (none: any time)", _row(*row))


# ---- music -----------------------------------------------------------------------------------
class MusicTab(_ListAndForm):
    def __init__(self, panel: "AudioPanel"):
        super().__init__(panel, "New Playlist")
        self.new_button.clicked.connect(lambda: self.add("Playlist"))

    def items(self) -> list[dict[str, Any]]:
        return music.load_all(self.project)

    def delete_command(self, item):
        return music.PlaylistCommand(self.project, item, None, "")

    def add(self, name: str) -> str:
        playlist = {"id": music.new_id(self.project, name), "name": name,
                    "playlist": {"tracks": [], "crossfade": 3.0}}
        self.push(music.PlaylistCommand(self.project, None, playlist, ""))
        self.choose(playlist["id"])
        return playlist["id"]

    def change(self, item: dict, key: str, value: Any, label: str) -> None:
        new = copy.deepcopy(item)
        if key == "name":
            new["name"] = value
        elif value in (None, []):
            new["playlist"].pop(key, None)
        else:
            new["playlist"][key] = value
        if new != item:
            self.push(music.PlaylistCommand(self.project, item, new, label, key))

    def build_form(self, item: dict[str, Any]) -> QWidget:
        p = item["playlist"]
        current = lambda: music.load(self.project, item["id"]) or item  # noqa: E731
        w = QWidget()
        form = QFormLayout(w)
        name = QLineEdit(item["name"])
        name.editingFinished.connect(
            lambda: name.text().strip() and self.change(current(), "name", name.text().strip(),
                                                        "name"))
        form.addRow("Name", name)
        events = {e["id"]: e["name"] for e in sound_events.load_all(self.project)}
        tracks = QListWidget()
        tracks.setMaximumHeight(100)
        for t in p.get("tracks", []):
            row = QListWidgetItem(events.get(t, f"{t} (missing)"))
            row.setData(ID_ROLE, t)
            tracks.addItem(row)
        self.tracks = tracks
        pick = QComboBox()
        for eid, ename in events.items():
            pick.addItem(ename, eid)
        add = QPushButton("Add")
        add.clicked.connect(lambda: pick.currentData() and self.change(
            current(), "tracks", p.get("tracks", []) + [pick.currentData()], "tracks"))
        remove = QPushButton("Remove")
        remove.clicked.connect(lambda: tracks.currentRow() >= 0 and self.change(
            current(), "tracks", [t for i, t in enumerate(p.get("tracks", []))
                                  if i != tracks.currentRow()], "tracks"))
        up = QPushButton("Up")
        up.clicked.connect(lambda: self._move(current(), tracks.currentRow(), -1))
        form.addRow("Tracks", tracks)
        form.addRow("", _row(pick, add, remove, up))
        order = QComboBox()
        order.addItem("In order", "in_order")
        order.addItem("Shuffled", "shuffle")
        order.setCurrentIndex(max(order.findData(p.get("order", "in_order")), 0))
        order.currentIndexChanged.connect(lambda _i: self.change(
            current(), "order", None if order.currentData() == "in_order"
            else order.currentData(), "order"))
        form.addRow("Order", order)
        form.addRow(WhereEditor(self.project, p, lambda k, v: self.change(current(), k, v, k)))
        priority = QSpinBox()
        priority.setRange(-100, 100)
        priority.setValue(int(p.get("priority", 0)))
        priority.setToolTip("Between playlists that fit as well, the higher plays")
        priority.valueChanged.connect(
            lambda v: self.change(current(), "priority", v or None, "priority"))
        crossfade = _spin(0.0, 60.0, 0.5, 1, " s")
        crossfade.setValue(float(p.get("crossfade", 3.0)))
        crossfade.valueChanged.connect(
            lambda v: self.change(current(), "crossfade", round(v, 3), "crossfade"))
        form.addRow("Priority", _row(priority, QLabel("crossfade"), crossfade))
        lo, hi = _pair(p.get("gap"), 0.0)
        a, b = _spin(0.0, 600.0, 1.0, 1, " s"), _spin(0.0, 600.0, 1.0, 1, " s")
        a.setValue(lo)
        b.setValue(hi)
        for box in (a, b):
            box.setToolTip("Silence between tracks, picked between these")
            box.valueChanged.connect(lambda _v: self.change(
                current(), "gap", [a.value(), b.value()] if b.value() > 0 else None, "gap"))
        form.addRow("Gap", _row(a, QLabel("to"), b))
        form.addRow(self.panel.play_row(lambda: self.panel.preview.play_playlist(
            self.project, item["id"])))
        return w

    def _move(self, item: dict, row: int, by: int) -> None:
        t = list(item["playlist"].get("tracks", []))
        if 0 <= row < len(t) and 0 <= row + by < len(t):
            t[row], t[row + by] = t[row + by], t[row]
            self.change(item, "tracks", t, "order of tracks")


# ---- ambience --------------------------------------------------------------------------------
class AmbienceTab(_ListAndForm):
    def __init__(self, panel: "AudioPanel"):
        super().__init__(panel, "New Ambience")
        self.new_button.clicked.connect(lambda: self.add("Ambience"))

    def items(self) -> list[dict[str, Any]]:
        return ambience.load_all(self.project)

    def delete_command(self, item):
        return ambience.AmbienceCommand(self.project, item, None, "")

    def add(self, name: str) -> str:
        ambience_set = {"id": ambience.new_id(self.project, name), "name": name,
                        "ambience": {"layers": [], "crossfade": 4.0}}
        self.push(ambience.AmbienceCommand(self.project, None, ambience_set, ""))
        self.choose(ambience_set["id"])
        return ambience_set["id"]

    def change(self, item: dict, key: str, value: Any, label: str) -> None:
        new = copy.deepcopy(item)
        if key == "name":
            new["name"] = value
        elif value in (None, []):
            new["ambience"].pop(key, None)
        else:
            new["ambience"][key] = value
        if new != item:
            self.push(ambience.AmbienceCommand(self.project, item, new, label, key))

    def _parts_table(self, item: dict, key: str, current) -> QTableWidget:
        """Layers or sprinkles: a sound, its times of day, and its numbers."""
        events = sound_events.load_all(self.project)
        layers = key == "layers"
        heads = ["Sound", *TIMES.values()] + (["Volume"] if layers else ["Every (s)", "Away (m)"])
        table = QTableWidget(0, len(heads))
        table.setHorizontalHeaderLabels(heads)
        table.setMinimumHeight(90)
        parts = [p if isinstance(p, dict) else {"event": p}
                 for p in item["ambience"].get(key, [])]

        def changed(row: int, field: str, value: Any) -> None:
            new_parts = copy.deepcopy(parts)
            if value in (None, []):
                new_parts[row].pop(field, None)
            else:
                new_parts[row][field] = value
            self.change(current(), key, new_parts, key)

        for r, part in enumerate(parts):
            table.insertRow(r)
            pick = QComboBox()
            for e in events:
                pick.addItem(e["name"], e["id"])
            if pick.findData(part.get("event")) < 0:
                pick.addItem(f"{part.get('event', '')} (missing)", part.get("event", ""))
            pick.setCurrentIndex(pick.findData(part.get("event")))
            pick.currentIndexChanged.connect(
                lambda _i, row=r, c=pick: changed(row, "event", c.currentData()))
            table.setCellWidget(r, 0, pick)
            for c, time in enumerate(TIMES, start=1):
                check = QCheckBox()
                check.setChecked(time in part.get("times", []))
                check.toggled.connect(lambda on, row=r, t=time: changed(row, "times", [
                    x for x in TIMES if (x == t and on) or
                    (x != t and x in parts[row].get("times", []))]))
                table.setCellWidget(r, c, check)
            if layers:
                volume = _spin(0.0, 4.0, 0.05)
                volume.setValue(float(part.get("volume", 1.0)))
                volume.valueChanged.connect(lambda v, row=r: changed(row, "volume", round(v, 3)))
                table.setCellWidget(r, 5, volume)
            else:
                for col, field, default in ((5, "every", 20.0), (6, "distance", 15.0)):
                    box = _spin(0.1, 10000.0, 1.0, 1)
                    box.setValue(sum(_pair(part.get(field), default)) / 2.0)
                    box.setToolTip("About this (the game varies it by a third either way)")
                    box.valueChanged.connect(lambda v, row=r, f=field: changed(
                        row, f, [round(v * 2 / 3, 2), round(v * 4 / 3, 2)]))
                    table.setCellWidget(r, col, box)
        return table

    def build_form(self, item: dict[str, Any]) -> QWidget:
        a = item["ambience"]
        current = lambda: ambience.load(self.project, item["id"]) or item  # noqa: E731
        w = QWidget()
        form = QFormLayout(w)
        name = QLineEdit(item["name"])
        name.editingFinished.connect(
            lambda: name.text().strip() and self.change(current(), "name", name.text().strip(),
                                                        "name"))
        form.addRow("Name", name)
        events = sound_events.load_all(self.project)
        first = events[0]["id"] if events else ""
        for key, label, tip in (
                ("layers", "Layers", "Sounds looping under everything, at their times of day"),
                ("sprinkles", "Sprinkles",
                 "One-off sounds now and then, somewhere around the listener")):
            table = self._parts_table(item, key, current)
            table.setToolTip(tip)
            setattr(self, f"{key}_table", table)
            add = QPushButton(f"Add {label[:-1].lower()}")
            add.clicked.connect(lambda _c=False, k=key: first and self.change(
                current(), k, [*current()["ambience"].get(k, []), {"event": first}], k))
            remove = QPushButton("Remove")
            remove.clicked.connect(lambda _c=False, k=key, t=table: t.currentRow() >= 0 and
                                   self.change(current(), k, [
                                       p for i, p in enumerate(current()["ambience"].get(k, []))
                                       if i != t.currentRow()], k))
            form.addRow(label, table)
            form.addRow("", _row(add, remove))
        form.addRow(WhereEditor(self.project, a, lambda k, v: self.change(current(), k, v, k),
                                times=False))
        priority = QSpinBox()
        priority.setRange(-100, 100)
        priority.setValue(int(a.get("priority", 0)))
        priority.valueChanged.connect(
            lambda v: self.change(current(), "priority", v or None, "priority"))
        crossfade = _spin(0.0, 60.0, 0.5, 1, " s")
        crossfade.setValue(float(a.get("crossfade", 4.0)))
        crossfade.valueChanged.connect(
            lambda v: self.change(current(), "crossfade", round(v, 3), "crossfade"))
        form.addRow("Priority", _row(priority, QLabel("crossfade"), crossfade))
        time = QComboBox()
        for key, label in TIMES.items():
            time.addItem(label, key)
        time.setCurrentIndex(1)
        form.addRow(self.panel.play_row(lambda: self.panel.preview.play_ambience(
            self.project, item["id"], time.currentData()), extra=[QLabel("as at"), time]))
        return w


# ---- the mixer -------------------------------------------------------------------------------
class MixerTab(QWidget):
    def __init__(self, panel: "AudioPanel"):
        super().__init__()
        self.panel = panel
        self.faders: dict[str, QSlider] = {}
        self.values: dict[str, QLabel] = {}
        form = QFormLayout(self)
        form.addRow(QLabel("How loud each bus is as the game starts (scripts change them while "
                           "it plays: midgard.audio.set_volume)."))
        for bus, label in sound_events.BUS_NAMES.items():
            fader = QSlider(Qt.Orientation.Horizontal)
            fader.setRange(0, 200)  # (percent: up to twice as loud)
            value = QLabel("100%")
            value.setMinimumWidth(40)
            fader.valueChanged.connect(lambda v, b=bus, t=value: self._moved(b, v, t))
            self.faders[bus] = fader
            self.values[bus] = value
            form.addRow(label, _row(fader, value))

    def refresh(self) -> None:
        project = self.panel.project
        if project is None:
            return
        for bus, volume in sound_events.load_buses(project).items():
            fader = self.faders[bus]
            fader.blockSignals(True)
            fader.setValue(round(volume * 100))
            fader.blockSignals(False)
            self.values[bus].setText(f"{fader.value()}%")

    def _moved(self, bus: str, value: int, label: QLabel) -> None:
        label.setText(f"{value}%")
        self.set_volume(bus, value / 100.0)

    def set_volume(self, bus: str, volume: float) -> None:
        project = self.panel.project
        old = sound_events.load_buses(project)
        if old.get(bus) == round(volume, 4):
            return
        project.undo_stack.push(sound_events.BusesCommand(project, old,
                                                          {**old, bus: round(volume, 4)}, bus))


# ---- the panel -------------------------------------------------------------------------------
class AudioPanel(QWidget):
    """Sounds, music, ambience and the mixer (see the module's notes)."""
    previewing = Signal(bool)

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.project: Project | None = None
        self.preview = sound_preview.shared()
        self.preview.stopped.connect(lambda: self.previewing.emit(False))
        self.tabs = QTabWidget()
        self.sounds_tab = SoundsTab(self)
        self.music_tab = MusicTab(self)
        self.ambience_tab = AmbienceTab(self)
        self.mixer_tab = MixerTab(self)
        self.tabs.addTab(self.sounds_tab, "Sounds")
        self.tabs.addTab(self.music_tab, "Music")
        self.tabs.addTab(self.ambience_tab, "Ambience")
        self.tabs.addTab(self.mixer_tab, "Mixer")
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(4, 4, 4, 4)
        layout.addWidget(self.tabs)
        if not self.preview.available():
            layout.addWidget(QLabel("<small>Sounds can't be previewed here (the engine "
                                    "module, or a sound device, is missing).</small>"))
        self.empty = QLabel("No project open.")
        self.stack = QStackedLayout(self)
        self.stack.addWidget(self.empty)
        self.stack.addWidget(page)

    def set_project(self, project: Project | None) -> None:
        if self.project is not None:
            try:
                self.project.sound_events_changed.disconnect(self.refresh)
                self.project.scenes_changed.disconnect(self.refresh)
            except (RuntimeError, TypeError):
                pass
        self.preview.stop()
        self.project = project
        self.stack.setCurrentIndex(1 if project is not None else 0)
        if project is not None:
            project.sound_events_changed.connect(self.refresh)
            project.scenes_changed.connect(self.refresh)
        self.refresh()

    def refresh(self, *_args) -> None:
        if self.project is None:
            return
        for tab in (self.sounds_tab, self.music_tab, self.ambience_tab, self.mixer_tab):
            tab.refresh()

    def play_row(self, play: Callable[[], bool], extra: list | None = None) -> QWidget:
        """Play and Stop buttons (Play disabled without a preview)."""
        button = QPushButton("▶ Play")
        button.setEnabled(self.preview.available())
        button.setToolTip("Hear it as the game plays it")
        button.clicked.connect(lambda: play() and self.previewing.emit(True))
        stop = QPushButton("■ Stop")
        stop.clicked.connect(self.preview.stop)
        return _row(button, stop, *(extra or []))
