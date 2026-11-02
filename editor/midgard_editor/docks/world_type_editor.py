# SPDX-License-Identifier: Apache-2.0
"""The World Type editor (milestone 5.11): world types' recipes, edited and previewed.

A world type is a recipe for worlds (midgard/world_type.h, world_types.py): its ground as a node
graph (midgard/worldgen.h; the shared node-graph widget), its climate and biomes, the objects it
places, and the shapes and sizes it suits with their gravity, atmosphere, calendar and walls.
Every edit is one undoable step, saved over the world type's latest version, or as a new
version once that one has shipped. The preview is the engine's own: a map of the whole world and
a view of its ground for any seed, on any shape and size it suits, with the ground's cost and
what the cook would say."""
import copy
import json
import logging
from typing import Any

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QColor, QImage, QPixmap
from PySide6.QtWidgets import (
    QCheckBox,
    QColorDialog,
    QComboBox,
    QDoubleSpinBox,
    QFileDialog,
    QFormLayout,
    QHBoxLayout,
    QHeaderView,
    QInputDialog,
    QLabel,
    QLineEdit,
    QListWidget,
    QMessageBox,
    QPushButton,
    QSpinBox,
    QSplitter,
    QStackedLayout,
    QTableWidget,
    QTableWidgetItem,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from .. import calendars, engine, environments, world_types
from ..node_graph import GraphModel, NodeGraphView, NodeSpec, edit
from ..physics_dialogs import SHAPES, WALLS, origin

log = logging.getLogger(__name__)

# The world graph's nodes (midgard/worldgen.h): kind -> (title, inputs, variadic, category,
# default settings).
NODES = {
    "noise": ("Noise", (), False, "Sources", {"size": 500.0, "octaves": 6, "salt": ""}),
    "ridged": ("Ridged Noise", (), False, "Sources", {"size": 500.0, "octaves": 6, "salt": ""}),
    "constant": ("Number", (), False, "Sources", {"value": 0.0}),
    "input": ("Input", (), False, "Sources", {"name": "latitude"}),
    "add": ("Add", ("in",), True, "Maths", {}),
    "multiply": ("Multiply", ("in",), True, "Maths", {}),
    "min": ("Lowest", ("in",), True, "Maths", {}),
    "max": ("Highest", ("in",), True, "Maths", {}),
    "subtract": ("Subtract", ("a", "b"), False, "Maths", {}),
    "divide": ("Divide", ("a", "b"), False, "Maths", {}),
    "lerp": ("Blend", ("a", "b", "t"), False, "Maths", {}),
    "abs": ("Absolute", ("x",), False, "Maths", {}),
    "clamp": ("Clamp", ("x",), False, "Shape", {"min": 0.0, "max": 100.0}),
    "smoothstep": ("Smooth Step", ("x",), False, "Shape", {"from": 0.0, "to": 1.0}),
    "curve": ("Curve", ("x",), False, "Shape", {"points": [[0.0, 0.0], [1.0, 1.0]]}),
    "terrace": ("Terraces", ("x",), False, "Shape", {"step": 10.0, "sharpness": 1.0}),
    "smooth": ("Smooth", ("x",), False, "Filters", {"radius": 10.0}),
    "erode": ("Erode", ("x",), False, "Filters", {"radius": 10.0, "strength": 0.5}),
}
SPECS = {kind: NodeSpec(kind, title, inputs, variadic, category, params)
         for kind, (title, inputs, variadic, category, params) in NODES.items()}
INPUT_NAMES = {"latitude": "Latitude (degrees)", "edge_distance": "Distance from the edge (m)",
               "end_distance": "Distance from the ends (m)"}
SHAPE_KEYS = ["bounded_area", "flat_cylinder", "curved_cylinder", "sphere_outside",
              "sphere_inside"]
# The recipe's ground settings shown (the hills' own come from the graph).
GROUND_FIELDS = {"sea_level": ("Sea level", -2000, 2000, 0, " m"),
                 "snow_line": ("Snow line (no climate)", -2000, 5000, 90, " m"),
                 "ocean_band": ("Ocean round an area", 0, 50_000, 300, " m"),
                 "ocean_depth": ("Ocean depth", 0, 5000, 30, " m"),
                 "end_ramp": ("Rising to a cylinder's ends", 0, 20_000, 150, " m"),
                 "wall_height": ("Height at the ends", -2000, 5000, 120, " m"),
                 "spacing": ("Finest grid spacing", 0.25, 64, 2, " m")}
CLIMATE_FIELDS = {"temperature": ("Temperature at sea level", -150, 150, 14, " °C"),
                  "latitude_cooling": ("Colder at the poles by", 0, 200, 40, " °C"),
                  "lapse_rate": ("Colder each km up by", 0, 50, 6.5, " °C"),
                  "variation": ("Variation", 0, 50, 3, " °C"),
                  "season_swing": ("Winter to summer", 0, 100, 12, " °C")}
MAP_BOX = (320, 160)  # the preview's map fits in this, in its world's proportions
PROJECTIONS = {"top_down": "From above", "unrolled": "Unrolled",
               "equirectangular": "Latitude and longitude", "cube_faces": "Cube faces"}
VIEW_SIZE = (320, 180)
PLACEMENT_COLUMNS = ["Object", "Spacing", "Chance", "Slope from", "to", "Height from", "to",
                     "Biomes", "Size from", "to", "Lean", "Distance"]


# ---- the graph: the engine's JSON and the widget's ------------------------------------------
def classic_graph(terrain: dict) -> dict:
    """The 5.9 ground as a graph: base + height_scale * noise (the same heights, exactly)."""
    return {"output": "height", "nodes": {
        "hills": {"op": "noise", "size": terrain.get("feature_size", 500.0),
                  "octaves": terrain.get("octaves", 6)},
        "scaled": {"op": "multiply", "in": [terrain.get("height_scale", 60.0), "hills"]},
        "height": {"op": "add", "in": [terrain.get("base", 8.0), "scaled"]}},
        "layout": {"hills": [0, 0], "scaled": [230, 20], "height": [460, 0]}}


def to_model(graph: dict) -> dict:
    """A world graph's JSON as the node-graph widget's model."""
    layout = graph.get("layout", {})
    nodes = {}
    for i, (node_id, node) in enumerate(graph.get("nodes", {}).items()):
        params = {k: v for k, v in node.items() if k not in ("op", "in")}
        kind = node.get("op", "constant")
        spec = SPECS.get(kind)
        inputs = list(node.get("in", []))
        if spec and not spec.variadic:
            inputs += [None] * (len(spec.inputs) - len(inputs))
        nodes[node_id] = {"kind": kind, "pos": list(layout.get(node_id, [i * 220, 0])),
                          "inputs": inputs,
                          "params": {**(spec.params if spec else {}), **params}}
    return {"nodes": nodes, "output": graph.get("output")}


def from_model(model: dict) -> dict:
    """The widget's model as a world graph's JSON (an empty input as 0; positions kept in
    "layout", which the engine leaves alone)."""
    nodes, layout = {}, {}
    for node_id, node in model["nodes"].items():
        out = {"op": node["kind"]}
        for key, value in node.get("params", {}).items():
            if key == "salt" and not value:
                continue
            out[key] = value
        spec = SPECS.get(node["kind"])
        if spec and spec.inputs:
            out["in"] = [0 if i is None else i for i in node["inputs"]]
        nodes[node_id] = out
        layout[node_id] = [round(node["pos"][0], 1), round(node["pos"][1], 1)]
    return {"output": model.get("output"), "nodes": nodes, "layout": layout}


def main_size(shape: str) -> tuple[str, float, float]:
    """The size a shape's preview varies: its name and range."""
    sizes = SHAPES[shape][1]
    name = next(k for k in ("radius", "circumference", "width") if k in sizes)
    return name, sizes[name][0], sizes[name][1]


def map_image(settings: dict, projection: str, box: tuple[int, int]) -> QImage:
    """A world's map (midgard/world_map.h) as large as fits in box, in its proportions. Raises
    ValueError for settings the engine refuses."""
    midgard = engine.module()
    text = json.dumps(settings)
    aspect = midgard.map_aspect(text, projection)
    width = max(1, min(box[0], round(box[1] * aspect)))
    height = max(1, min(box[1], round(width / aspect)))
    pixels = midgard.world_map(text, width, height, projection)
    return QImage(pixels, width, height, QImage.Format.Format_RGBA8888).copy()


def preview_settings(recipe: dict, shape: str, size: float, seed: int) -> dict:
    """Scene settings for previewing a recipe: a body of that shape and size (kept in the
    shape's range; its other sizes their defaults), and the ground's settings with the recipe's
    graph and the seed."""
    body: dict[str, Any] = {"id": "preview", "shape": shape, "gravity": 9.81, "seed": seed}
    for name, (_low, _high, default) in SHAPES[shape][1].items():
        body[name] = default
    main, low, high = main_size(shape)
    body[main] = min(max(size, low), high)
    if shape == "bounded_area":
        body["length"] = body["width"]  # a square
    body["origin"] = origin(shape, body)
    terrain = dict(recipe.get("terrain", {}))
    if recipe.get("graph"):
        terrain["graph"] = {k: v for k, v in recipe["graph"].items() if k != "layout"}
    terrain["seed"] = seed
    return {"body": body, "terrain": terrain}


class SaveGraphCommand(world_types.SaveRecipeCommand):
    """Saving a recipe after a change to it: one drag of the same nodes is one step."""
    MOVE_ID = 7312

    def __init__(self, project, world_type: str, new_recipe: dict, moved: frozenset | None):
        super().__init__(project, world_type, new_recipe)
        self.moved = moved

    def id(self) -> int:
        return self.MOVE_ID if self.moved else -1

    def mergeWith(self, other) -> bool:  # noqa: N802 (Qt's name)
        if (not isinstance(other, SaveGraphCommand) or other.moved != self.moved
                or other.world_type != self.world_type or other.version != self.version):
            return False
        self.recipe = other.recipe  # (already saved by its redo)
        return True


class _GraphSaver:
    """The graph view's undo stack: each of its edits saved as the recipe's (see save)."""

    def __init__(self, panel: "WorldTypePanel"):
        self.panel = panel

    def push(self, command) -> None:
        self.panel._graph_edited(command.text(), command.moved)


class WorldTypePanel(QWidget):
    """The World Types dock."""

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.project = None
        self.world_type: str | None = None
        self.recipe: dict[str, Any] = {}
        self._loading = False
        self._view = None  # the engine's offscreen view, for the 3D preview
        self.map_image: QImage | None = None
        self._refresh = QTimer(self)
        self._refresh.setSingleShot(True)
        self._refresh.setInterval(400)
        self._refresh.timeout.connect(self.update_preview)

        self.stack = QStackedLayout(self)
        self.stack.addWidget(QLabel("Open a project to make world types."))
        page = QWidget()
        self.stack.addWidget(page)
        outer = QVBoxLayout(page)

        bar = QHBoxLayout()
        self.chooser = QComboBox()
        self.chooser.currentIndexChanged.connect(lambda _i: self._chosen())
        bar.addWidget(QLabel("World type"))
        bar.addWidget(self.chooser, 1)
        self.name = QLineEdit()
        self.name.setPlaceholderText("Name")
        self.name.editingFinished.connect(self._renamed)
        bar.addWidget(self.name, 1)
        self.version = QLabel()
        bar.addWidget(self.version)
        for text, slot in (("New", self.new_world_type), ("Duplicate", self.duplicate),
                           ("Delete", lambda: self.delete_world_type(confirm=True))):
            b = QPushButton(text)
            b.clicked.connect(lambda _=False, s=slot: s())
            bar.addWidget(b)
        outer.addLayout(bar)

        split = QSplitter(Qt.Orientation.Horizontal)
        outer.addWidget(split, 1)
        self.tabs = QTabWidget()
        split.addWidget(self.tabs)

        # Ground: the graph, and the chosen node's settings.
        ground = QWidget()
        ground_layout = QHBoxLayout(ground)
        self.model = GraphModel(SPECS)
        self.graph_saver = _GraphSaver(self)
        self.graph_view = NodeGraphView(self.model, undo_stack=self.graph_saver)
        self.graph_view.selection_changed.connect(self._node_chosen)
        ground_layout.addWidget(self.graph_view, 3)
        self.node_form_host = QWidget()
        self.node_form = QFormLayout(self.node_form_host)
        ground_layout.addWidget(self.node_form_host, 1)
        self.tabs.addTab(ground, "Ground")

        # Climate and biomes.
        climate = QWidget()
        climate_layout = QVBoxLayout(climate)
        form = QFormLayout()
        self.has_climate = QCheckBox("A climate (temperatures, snow below freezing)")
        self.has_climate.toggled.connect(lambda _on: self._climate_changed())
        form.addRow(self.has_climate)
        self.climate_fields: dict[str, QDoubleSpinBox] = {}
        for key, (label, low, high, default, suffix) in CLIMATE_FIELDS.items():
            box = _spin(low, high, default, suffix)
            box.editingFinished.connect(self._climate_changed)
            form.addRow(label, box)
            self.climate_fields[key] = box
        climate_layout.addLayout(form)
        climate_layout.addWidget(QLabel("Biomes (up to 8): ground colours by temperature and "
                                        "moisture; double-click a colour to choose it"))
        self.biomes = QTableWidget(0, 7)
        self.biomes.setHorizontalHeaderLabels(["Name", "From °C", "To °C", "Moisture from",
                                               "to", "Colour", "Dry colour"])
        self.biomes.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        self.biomes.itemChanged.connect(lambda _item: self._biomes_changed())
        self.biomes.cellDoubleClicked.connect(self._pick_colour)
        climate_layout.addWidget(self.biomes, 1)
        row = QHBoxLayout()
        for text, slot in (("Add Biome", self.add_biome), ("Remove Biome", self.remove_biome)):
            b = QPushButton(text)
            b.clicked.connect(lambda _=False, s=slot: s())
            row.addWidget(b)
        row.addStretch(1)
        climate_layout.addLayout(row)
        self.tabs.addTab(climate, "Climate and Biomes")

        # Placements.
        placements = QWidget()
        placements_layout = QVBoxLayout(placements)
        self.placements = QTableWidget(0, len(PLACEMENT_COLUMNS))
        self.placements.setHorizontalHeaderLabels(PLACEMENT_COLUMNS)
        self.placements.itemChanged.connect(lambda _item: self._placements_changed())
        placements_layout.addWidget(self.placements, 1)
        row = QHBoxLayout()
        for text, slot in (("Add Rule", self.add_rule), ("Remove Rule", self.remove_rule)):
            b = QPushButton(text)
            b.clicked.connect(lambda _=False, s=slot: s())
            row.addWidget(b)
        row.addStretch(1)
        placements_layout.addLayout(row)
        self.tabs.addTab(placements, "Placements")

        # The recipe's other settings.
        recipe = QWidget()
        rform = QFormLayout(recipe)
        self.shapes: dict[str, QCheckBox] = {}
        shapes_row = QWidget()
        shapes_layout = QVBoxLayout(shapes_row)
        shapes_layout.setContentsMargins(0, 0, 0, 0)
        for key in SHAPE_KEYS:
            box = QCheckBox(SHAPES[key][0])
            box.toggled.connect(lambda _on: self._recipe_fields_changed())
            shapes_layout.addWidget(box)
            self.shapes[key] = box
        rform.addRow("Suits (none ticked: any)", shapes_row)
        self.limit_size = QCheckBox("Only worlds sized")
        self.limit_size.toggled.connect(lambda _on: self._recipe_fields_changed())
        self.size_min = _spin(1, 1e6, 1000, " m")
        self.size_max = _spin(1, 1e6, 20000, " m")
        for box in (self.size_min, self.size_max):
            box.editingFinished.connect(self._recipe_fields_changed)
        size_row = QHBoxLayout()
        size_row.addWidget(self.limit_size)
        size_row.addWidget(self.size_min)
        size_row.addWidget(QLabel("to"))
        size_row.addWidget(self.size_max)
        rform.addRow("Size", size_row)
        self.gravity_set = QCheckBox("Gravity")
        self.gravity_set.toggled.connect(lambda _on: self._recipe_fields_changed())
        self.gravity = _spin(0.01, 100, 9.81, " m/s²")
        self.gravity.editingFinished.connect(self._recipe_fields_changed)
        gravity_row = QHBoxLayout()
        gravity_row.addWidget(self.gravity_set)
        gravity_row.addWidget(self.gravity)
        rform.addRow("", gravity_row)
        self.atmosphere = QComboBox()
        self.atmosphere.currentIndexChanged.connect(lambda _i: self._recipe_fields_changed())
        rform.addRow("Atmosphere", self.atmosphere)
        self.calendar = QComboBox()
        self.calendar.currentIndexChanged.connect(lambda _i: self._recipe_fields_changed())
        rform.addRow("Calendar", self.calendar)
        self.wall = QComboBox()
        for key, label in WALLS.items():
            self.wall.addItem(label, key)
        self.wall.currentIndexChanged.connect(lambda _i: self._recipe_fields_changed())
        rform.addRow("A cylinder's end walls", self.wall)
        self.ground_fields: dict[str, QDoubleSpinBox] = {}
        for key, (label, low, high, default, suffix) in GROUND_FIELDS.items():
            box = _spin(low, high, default, suffix)
            box.editingFinished.connect(self._recipe_fields_changed)
            rform.addRow(label, box)
            self.ground_fields[key] = box
        self.tabs.addTab(recipe, "Recipe")

        # The preview.
        preview = QWidget()
        pv = QVBoxLayout(preview)
        pform = QFormLayout()
        self.preview_shape = QComboBox()
        self.preview_shape.currentIndexChanged.connect(lambda _i: self._shape_chosen())
        pform.addRow("Shape", self.preview_shape)
        self.preview_size = _spin(1000, 20000, 6000, " m")
        self.preview_size.editingFinished.connect(self._schedule)
        pform.addRow("Size", self.preview_size)
        self.seed = QSpinBox()
        self.seed.setRange(0, 2_000_000_000)
        self.seed.setValue(1)
        self.seed.valueChanged.connect(lambda _v: self._schedule())
        pform.addRow("Seed", self.seed)
        self.projection = QComboBox()
        self.projection.currentIndexChanged.connect(lambda _i: self._schedule())
        pform.addRow("Map", self.projection)
        pv.addLayout(pform)
        self.map = QLabel()
        self.map.setMinimumSize(320, 160)
        self.map.setAlignment(Qt.AlignmentFlag.AlignCenter)
        pv.addWidget(self.map)
        save_row = QHBoxLayout()
        self.save_size = QSpinBox()
        self.save_size.setRange(64, 4096)
        self.save_size.setValue(2048)
        self.save_size.setSuffix(" px")
        self.save_size.setToolTip("The saved map's longer side")
        save = QPushButton("Save Map Image…")
        save.clicked.connect(lambda: self._save_map_as())
        save_row.addWidget(save)
        save_row.addWidget(self.save_size)
        save_row.addStretch(1)
        pv.addLayout(save_row)
        self.view_image = QLabel()
        self.view_image.setMinimumSize(320, 180)
        self.view_image.setAlignment(Qt.AlignmentFlag.AlignCenter)
        pv.addWidget(self.view_image)
        self.cost = QLabel()
        pv.addWidget(self.cost)
        self.problems = QListWidget()
        pv.addWidget(self.problems, 1)
        split.addWidget(preview)
        split.setStretchFactor(0, 3)
        split.setStretchFactor(1, 1)

    # ---- the project ------------------------------------------------------------------------
    def set_project(self, project) -> None:
        if self.project is not None:
            self.project.world_types_changed.disconnect(self._world_types_changed)
        self.project = project
        self.stack.setCurrentIndex(0 if project is None else 1)
        if project is not None:
            project.world_types_changed.connect(self._world_types_changed)
        self._reload(keep=None)

    def _world_types_changed(self, _world_type: str) -> None:
        """A save (shown already, if it was this panel's own) or an undo or redo (shown now)."""
        self._reload(keep=self.world_type)

    def _reload(self, keep: str | None) -> None:
        self.chooser.blockSignals(True)
        self.chooser.clear()
        if self.project is not None:
            for t in world_types.load_all(self.project):
                self.chooser.addItem(t["name"], t["id"])
        index = self.chooser.findData(keep) if keep else (0 if self.chooser.count() else -1)
        self.chooser.setCurrentIndex(max(index, 0) if self.chooser.count() else -1)
        self.chooser.blockSignals(False)
        self._chosen()

    def _chosen(self) -> None:
        """Shows the chosen world type's latest version."""
        same = self.world_type == self.chooser.currentData()
        self.world_type = self.chooser.currentData()
        found = (world_types.recipe(self.project, self.world_type)
                 if self.project is not None and self.world_type else None)
        found = copy.deepcopy(found or {})
        if same and found == self.recipe and self.world_type:
            self._show_name()  # this panel's own save: shown already
            return
        self.recipe = found
        self._loading = True
        try:
            self._show_recipe(fit=not same)
        finally:
            self._loading = False
        self._schedule()

    def _show_name(self) -> None:
        info = next((t for t in world_types.load_all(self.project) if t["id"] == self.world_type),
                    None) if self.project and self.world_type else None
        self.name.setText(info["name"] if info else "")
        self.version.setText(f"version {info['version']}" if info else "")

    def _show_recipe(self, fit: bool = True) -> None:
        r = self.recipe
        terrain = r.get("terrain", {})
        self._show_name()
        graph = r.get("graph") or classic_graph(terrain)
        self.model.restore(to_model(graph))
        if fit:
            self.graph_view.fit()
        self._node_chosen(self.graph_view.selected())
        climate = terrain.get("climate")
        self.has_climate.setChecked(climate is not None)
        for key, box in self.climate_fields.items():
            box.setValue(float((climate or {}).get(key, CLIMATE_FIELDS[key][3])))
        self.biomes.blockSignals(True)
        self.biomes.setRowCount(0)
        for b in terrain.get("biomes", []):
            self._biome_row(b)
        self.biomes.blockSignals(False)
        self.placements.blockSignals(True)
        self.placements.setRowCount(0)
        for rule in terrain.get("placements", []):
            self._rule_row(rule)
        self.placements.blockSignals(False)
        for key, box in self.shapes.items():
            box.setChecked(key in r.get("shapes", []))
        self.limit_size.setChecked("size" in r)
        if "size" in r:
            self.size_min.setValue(float(r["size"][0]))
            self.size_max.setValue(float(r["size"][1]))
        self.gravity_set.setChecked("gravity" in r)
        self.gravity.setValue(float(r.get("gravity", 9.81)))
        self._fill_choices(self.atmosphere, [(e["id"], e["name"]) for e in
                                             environments.load_all(self.project)]
                           if self.project else [], r.get("atmosphere", ""))
        self._fill_choices(self.calendar, [(c["id"], c.get("name", c["id"])) for c in
                                           calendars.load_all(self.project)]
                           if self.project else [], r.get("calendar", ""))
        self.wall.setCurrentIndex(max(self.wall.findData(terrain.get("wall", "cliff")), 0))
        for key, box in self.ground_fields.items():
            box.setValue(float(terrain.get(key, GROUND_FIELDS[key][3])))
        self._fill_preview_shapes()

    def _fill_preview_shapes(self) -> None:
        """The preview offers the shapes the recipe suits."""
        self.preview_shape.blockSignals(True)
        current = self.preview_shape.currentData()
        self.preview_shape.clear()
        for key in self.recipe.get("shapes") or SHAPE_KEYS:
            self.preview_shape.addItem(SHAPES[key][0], key)
        self.preview_shape.setCurrentIndex(max(self.preview_shape.findData(current), 0))
        self.preview_shape.blockSignals(False)
        self._shape_chosen()

    @staticmethod
    def _fill_choices(combo: QComboBox, items: list, current: str) -> None:
        combo.blockSignals(True)
        combo.clear()
        combo.addItem("(none)", "")
        for key, name in items:
            combo.addItem(name, key)
        combo.setCurrentIndex(max(combo.findData(current), 0))
        combo.blockSignals(False)

    # ---- saving -----------------------------------------------------------------------------
    def save(self, recipe: dict, label: str = "Change world type",
             moved: frozenset | None = None) -> None:
        """The recipe as the world type's (one undoable step on the project's stack)."""
        if self.project is None or self.world_type is None or recipe == self.recipe:
            return
        command = SaveGraphCommand(self.project, self.world_type, recipe, moved)
        command.setText(label)
        self.recipe = copy.deepcopy(recipe)
        self.project.undo_stack.push(command)
        self._schedule()

    def _ground(self) -> dict:
        return dict(self.recipe.get("terrain", {}))

    def _graph_edited(self, label: str, moved: frozenset | None) -> None:
        """An edit in the graph view (already made to the model)."""
        if self._loading:
            return
        graph = from_model(self.model.graph)
        if graph != self.recipe.get("graph"):
            self.save({**self.recipe, "graph": graph}, label, moved)

    def _climate_changed(self) -> None:
        if self._loading:
            return
        ground = self._ground()
        if self.has_climate.isChecked():
            ground["climate"] = {k: round(b.value(), 3) for k, b in self.climate_fields.items()}
        else:
            ground.pop("climate", None)
        self.save({**self.recipe, "terrain": ground}, "Change the climate")

    def _recipe_fields_changed(self) -> None:
        if self._loading:
            return
        r = copy.deepcopy(self.recipe)
        shapes = [k for k, b in self.shapes.items() if b.isChecked()]
        if shapes:
            r["shapes"] = shapes
        else:
            r.pop("shapes", None)
        if self.limit_size.isChecked():
            r["size"] = [self.size_min.value(), self.size_max.value()]
        else:
            r.pop("size", None)
        if self.gravity_set.isChecked():
            r["gravity"] = round(self.gravity.value(), 3)
        else:
            r.pop("gravity", None)
        for key, combo in (("atmosphere", self.atmosphere), ("calendar", self.calendar)):
            if combo.currentData():
                r[key] = combo.currentData()
            else:
                r.pop(key, None)
        ground = dict(r.get("terrain", {}))
        ground["wall"] = self.wall.currentData()
        for key, box in self.ground_fields.items():
            ground[key] = round(box.value(), 3)
        r["terrain"] = ground
        self.save(r, "Change world type")
        self._fill_preview_shapes()

    # ---- the chosen node's settings ---------------------------------------------------------
    def _node_chosen(self, ids: list) -> None:
        while self.node_form.rowCount():
            self.node_form.removeRow(0)
        if len(ids) != 1 or ids[0] not in self.model.nodes:
            self.node_form.addRow(QLabel("Right-click the canvas to add a node; choose one "
                                         "to change its settings."))
            return
        node_id = ids[0]
        node = self.model.nodes[node_id]
        self.node_form.addRow(QLabel(f"<b>{SPECS[node['kind']].title}</b> ({node_id})"))
        params = node["params"]

        def set_param(key, value):
            new = dict(self.model.nodes[node_id]["params"])
            if new.get(key) != value:
                new[key] = value
                edit(self.model, self.graph_saver, "Change a node's setting",
                     lambda m: m.set_params(node_id, new))

        for key, value in params.items():
            if key == "name":
                combo = QComboBox()
                for name, label in INPUT_NAMES.items():
                    combo.addItem(label, name)
                combo.setCurrentIndex(max(combo.findData(value), 0))
                combo.currentIndexChanged.connect(
                    lambda _i, c=combo, k=key: set_param(k, c.currentData()))
                self.node_form.addRow("Input", combo)
            elif key == "salt":
                line = QLineEdit(str(value))
                line.setToolTip("A name: the same noise settings with another name give "
                                "another noise")
                line.editingFinished.connect(lambda e=line, k=key: set_param(k, e.text()))
                self.node_form.addRow("Name", line)
            elif key == "points":
                line = QLineEdit("; ".join(f"{x:g}, {y:g}" for x, y in value))
                line.setToolTip("x, y; x, y; ... with x rising")
                line.editingFinished.connect(lambda e=line: self._set_points(node_id, e))
                self.node_form.addRow("Points", line)
            elif key == "octaves":
                box = QSpinBox()
                box.setRange(1, 12)
                box.setValue(int(value))
                box.valueChanged.connect(lambda v, k=key: set_param(k, int(v)))
                self.node_form.addRow("Octaves", box)
            else:
                box = _spin(-1e6, 1e6, float(value), "")
                box.editingFinished.connect(lambda b=box, k=key: set_param(k, b.value()))
                self.node_form.addRow(key.replace("_", " ").capitalize(), box)

    def _set_points(self, node_id: str, edit: QLineEdit) -> None:
        try:
            points = [[float(a), float(b)] for a, b in
                      (pair.split(",") for pair in edit.text().split(";") if pair.strip())]
        except ValueError:
            return
        params = dict(self.model.nodes[node_id]["params"])
        params["points"] = points
        edit(self.model, self.graph_saver, "Change a curve",
             lambda m: m.set_params(node_id, params))

    # ---- biomes -----------------------------------------------------------------------------
    def _biome_row(self, b: dict) -> None:
        row = self.biomes.rowCount()
        self.biomes.insertRow(row)
        t = b.get("temperature", [-100, 100])
        m = b.get("moisture", [0, 1])
        values = [b.get("name", ""), t[0], t[1], m[0], m[1]]
        for col, value in enumerate(values):
            self.biomes.setItem(row, col, QTableWidgetItem(str(value)))
        for col, key, default in ((5, "color", [0.27, 0.42, 0.16]),
                                  (6, "dry_color", [0.55, 0.50, 0.28])):
            rgb = b.get(key, default)
            item = QTableWidgetItem(json.dumps([round(c, 3) for c in rgb]))
            item.setBackground(QColor.fromRgbF(*rgb))
            item.setFlags(item.flags() & ~Qt.ItemFlag.ItemIsEditable)
            self.biomes.setItem(row, col, item)

    def _biomes_changed(self) -> None:
        if self._loading:
            return
        biomes = []
        try:
            for row in range(self.biomes.rowCount()):
                cells = [self.biomes.item(row, c).text() for c in range(7)]
                biomes.append({"name": cells[0],
                               "temperature": [float(cells[1]), float(cells[2])],
                               "moisture": [float(cells[3]), float(cells[4])],
                               "color": json.loads(cells[5]),
                               "dry_color": json.loads(cells[6])})
        except (ValueError, AttributeError):
            return  # half typed: saved when it reads
        ground = self._ground()
        if biomes:
            ground["biomes"] = biomes
        else:
            ground.pop("biomes", None)
        self.save({**self.recipe, "terrain": ground}, "Change the biomes")

    def add_biome(self) -> None:
        if self.biomes.rowCount() >= 8:
            return
        self.biomes.blockSignals(True)
        self._biome_row({"name": f"Biome {self.biomes.rowCount() + 1}"})
        self.biomes.blockSignals(False)
        self._biomes_changed()

    def remove_biome(self) -> None:
        row = self.biomes.currentRow()
        if row >= 0:
            self.biomes.blockSignals(True)
            self.biomes.removeRow(row)
            self.biomes.blockSignals(False)
            self._biomes_changed()

    def _pick_colour(self, row: int, col: int) -> None:
        if col not in (5, 6):
            return
        rgb = json.loads(self.biomes.item(row, col).text())
        colour = QColorDialog.getColor(QColor.fromRgbF(*rgb), self, "Ground colour")
        if colour.isValid():
            self.set_biome_colour(row, col, [colour.redF(), colour.greenF(), colour.blueF()])

    def set_biome_colour(self, row: int, col: int, rgb: list) -> None:
        item = self.biomes.item(row, col)
        self.biomes.blockSignals(True)
        item.setText(json.dumps([round(c, 3) for c in rgb]))
        item.setBackground(QColor.fromRgbF(*rgb))
        self.biomes.blockSignals(False)
        self._biomes_changed()

    # ---- placements -------------------------------------------------------------------------
    def _rule_row(self, rule: dict) -> None:
        row = self.placements.rowCount()
        self.placements.insertRow(row)
        slope, height = rule.get("slope", [0, 30]), rule.get("height", [0.5, 1000000])
        scale = rule.get("scale", [1, 1])
        values = [rule.get("object", ""), rule.get("spacing", 8), rule.get("chance", 1),
                  slope[0], slope[1], height[0], height[1], ", ".join(rule.get("biomes", [])),
                  scale[0], scale[1], rule.get("align", 0), rule.get("distance", 200)]
        for col, value in enumerate(values):
            self.placements.setItem(row, col, QTableWidgetItem(f"{value:g}" if isinstance(
                value, float) else str(value)))

    def _placements_changed(self) -> None:
        if self._loading:
            return
        rules = []
        try:
            for row in range(self.placements.rowCount()):
                cells = [self.placements.item(row, c).text()
                         for c in range(len(PLACEMENT_COLUMNS))]
                num = [0.0 if c in (0, 7) else float(text) for c, text in enumerate(cells)]
                rule = {"object": cells[0], "spacing": num[1], "chance": num[2],
                        "slope": [num[3], num[4]], "height": [num[5], num[6]],
                        "scale": [num[8], num[9]], "align": num[10], "distance": num[11]}
                biomes = [b.strip() for b in cells[7].split(",") if b.strip()]
                if biomes:
                    rule["biomes"] = biomes
                rules.append(rule)
        except (ValueError, AttributeError):
            return
        ground = self._ground()
        if rules:
            ground["placements"] = rules
        else:
            ground.pop("placements", None)
        self.save({**self.recipe, "terrain": ground}, "Change the placements")

    def add_rule(self, object_id: str | None = None) -> None:
        if object_id is None and self.project is not None:
            ids = [o["id"] for o in self.project.objects()] if hasattr(self.project,
                                                                        "objects") else []
            object_id = ids[0] if ids else "obj_"
        self.placements.blockSignals(True)
        self._rule_row({"object": object_id or "obj_"})
        self.placements.blockSignals(False)
        self._placements_changed()

    def remove_rule(self) -> None:
        row = self.placements.currentRow()
        if row >= 0:
            self.placements.blockSignals(True)
            self.placements.removeRow(row)
            self.placements.blockSignals(False)
            self._placements_changed()

    # ---- world types ------------------------------------------------------------------------
    def new_world_type(self, name: str | None = None) -> str | None:
        if self.project is None:
            return None
        if name is None:
            name, ok = QInputDialog.getText(self, "New World Type", "Name")
            if not ok or not name.strip():
                return None
        world_type = world_types.new_id(self.project, name)
        terrain: dict = {}
        self.project.undo_stack.push(world_types.SaveRecipeCommand(
            self.project, world_type, {"terrain": terrain, "graph": classic_graph(terrain)},
            name.strip()))
        self._reload(keep=world_type)
        return world_type

    def duplicate(self) -> str | None:
        if self.project is None or self.world_type is None:
            return None
        name = f"{self.name.text()} (copy)"
        world_type = world_types.new_id(self.project, name)
        self.project.undo_stack.push(world_types.SaveRecipeCommand(
            self.project, world_type, copy.deepcopy(self.recipe), name))
        self._reload(keep=world_type)
        return world_type

    def _renamed(self) -> None:
        if self.project is None or self.world_type is None:
            return
        name = self.name.text().strip()
        info = next(t for t in world_types.load_all(self.project) if t["id"] == self.world_type)
        if name and name != info["name"]:
            self.project.undo_stack.push(world_types.SaveRecipeCommand(
                self.project, self.world_type, copy.deepcopy(self.recipe), name))

    def delete_world_type(self, confirm: bool = True) -> bool:
        if self.project is None or self.world_type is None:
            return False
        try:
            command = world_types.DeleteWorldTypeCommand(self.project, self.world_type)
        except world_types.WorldTypeError as e:
            if confirm:
                QMessageBox.information(self, "World Types", str(e))
            return False
        if confirm and QMessageBox.question(self, "Delete World Type",
                                            f"Delete {self.name.text()}?") != \
                QMessageBox.StandardButton.Yes:
            return False
        self.project.undo_stack.push(command)
        self._reload(keep=None)
        return True

    # ---- the preview ------------------------------------------------------------------------
    def _shape_chosen(self) -> None:
        shape = self.preview_shape.currentData()
        if shape:
            name, low, high = main_size(shape)
            self.preview_size.blockSignals(True)
            self.preview_size.setRange(low, high)
            self.preview_size.setPrefix(f"{name}: ")
            self.preview_size.blockSignals(False)
            midgard = engine.module()
            self.projection.blockSignals(True)
            current = self.projection.currentData()
            self.projection.clear()
            for key in midgard.map_projections(shape) if midgard else []:
                self.projection.addItem(PROJECTIONS.get(key, key), key)
            self.projection.setCurrentIndex(max(self.projection.findData(current), 0))
            self.projection.blockSignals(False)
        self._schedule()

    def _schedule(self) -> None:
        self._refresh.start()

    def preview_settings(self) -> dict | None:
        shape = self.preview_shape.currentData()
        if not shape or not self.recipe:
            return None
        return preview_settings(self.recipe, shape, self.preview_size.value(), self.seed.value())

    def update_preview(self) -> None:
        """The map, the view of the ground, its cost and what the cook would say."""
        self.problems.clear()
        settings = self.preview_settings()
        midgard = engine.module()
        if settings is None or midgard is None:
            self.cost.setText("" if midgard else "The preview needs the engine module.")
            return
        recipe = dict(self.recipe)
        if "graph" in settings["terrain"]:
            recipe["graph"] = settings["terrain"]["graph"]  # (without its layout)
        recipe_json = json.dumps(recipe)
        for problem in midgard.recipe_problems(recipe_json, json.dumps(settings["body"])):
            self.problems.addItem(problem)
        graph = settings["terrain"].get("graph")
        try:
            cost = midgard.graph_cost(json.dumps(graph)) if graph else 1.0
            self.cost.setText(f"The ground costs {cost:.1f}× the 5.9 ground"
                              + (" (over 4: it streams slowly)" if cost > 4 else ""))
        except ValueError as e:
            self.cost.setText(str(e))
            return
        if self.problems.count():
            return
        try:
            self.map_image = map_image(settings, self.projection.currentData() or "", MAP_BOX)
            self.map.setPixmap(QPixmap.fromImage(self.map_image))
        except (ValueError, RuntimeError) as e:
            self.problems.addItem(str(e))
            return
        self.render_view(settings)

    def save_map(self, path: str, long_side: int) -> bool:
        """The preview's world's map as an image file (PNG by its name), its longer side
        long_side pixels."""
        settings = self.preview_settings()
        if settings is None or engine.module() is None:
            return False
        image = map_image(settings, self.projection.currentData() or "", (long_side, long_side))
        return image.save(path)

    def _save_map_as(self) -> None:
        if self.preview_settings() is None:
            return
        name = f"{self.world_type}_{self.seed.value()}_{self.projection.currentData()}.png"
        path, _ = QFileDialog.getSaveFileName(self, "Save Map Image", name, "PNG images (*.png)")
        if path:
            try:
                ok = self.save_map(path, self.save_size.value())
            except (ValueError, RuntimeError) as e:
                ok, path = False, str(e)
            if not ok:
                QMessageBox.warning(self, "Save Map Image", f"The map wasn't saved: {path}")

    def render_view(self, settings: dict) -> None:
        """A view of the ground at the scene's origin from above, through the engine's own
        renderer (the game's look)."""
        midgard = engine.module()
        width, height = VIEW_SIZE
        try:
            if self._view is None:
                self._view = midgard.Viewport()
                self._view.attach_offscreen(width, height)
            self._view.set_settings(json.dumps(settings))
            ground = self._view.terrain_ground([0.0, 0.0, 0.0])
            base, up = (list(ground[1]), list(ground[2])) if ground else ([0.0] * 3, [0, 1, 0])
            base[1] = max(base[1], settings["terrain"].get("sea_level", 0.0))
            eye = [base[i] + up[i] * 90.0 for i in range(3)]
            # Looking along +X and down a little (the shapes' surfaces run along X there).
            target = [eye[0] + 400.0, eye[1] - 120.0, eye[2] + 150.0]
            self._view.set_camera(eye, target, 60.0)
            self._view.render()
            pixels = self._view.read_pixels()
            image = QImage(pixels, width, height, QImage.Format.Format_RGBA8888).copy()
            self.view_image.setPixmap(QPixmap.fromImage(image))
        except RuntimeError as e:  # no Vulkan driver
            self._view = None
            self.view_image.setText(f"No view: {e}")

def _spin(low: float, high: float, value: float, suffix: str) -> QDoubleSpinBox:
    box = QDoubleSpinBox()
    box.setRange(low, high)
    box.setDecimals(3)
    box.setValue(value)
    box.setSuffix(suffix)
    box.setKeyboardTracking(False)
    return box
