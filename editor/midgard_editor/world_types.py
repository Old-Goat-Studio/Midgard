# SPDX-License-Identifier: Apache-2.0
"""World types (milestone 5.11; migration 12, midgard/world_type.h): recipes for worlds, kept in
versions. A scene's terrain names a world type's version and a seed:

    "terrain": {"world_type": "wt_...", "version": 2, "seed": 7}

Every version a shipped game could use is kept unchanged (an export marks the versions its scenes
use as shipped), so a world still generates the same after its world type changes: saving over
a shipped version saves a new one instead. Unshipped versions are changed in place."""
import copy
import json
import re
from typing import Any

from PySide6.QtGui import QUndoCommand

from .project import Project


def load_all(project: Project) -> list[dict[str, Any]]:
    """Every world type: {"id", "name", "version"} (the latest), by name."""
    with project.connect() as con:
        rows = con.execute("SELECT id, name, version FROM world_type ORDER BY name, id").fetchall()
    return [{"id": r[0], "name": r[1], "version": r[2]} for r in rows]


def recipe(project: Project, world_type: str, version: int | None = None) -> dict[str, Any] | None:
    """A version's recipe (the latest if none is given), or None if there is no such version."""
    with project.connect() as con:
        if version is None:
            row = con.execute("SELECT version FROM world_type WHERE id = ?",
                              (world_type,)).fetchone()
            if row is None:
                return None
            version = row[0]
        row = con.execute("SELECT recipe FROM world_type_version WHERE world_type = ? AND "
                          "version = ?", (world_type, version)).fetchone()
    return None if row is None else json.loads(row[0])


def is_shipped(project: Project, world_type: str, version: int) -> bool:
    with project.connect() as con:
        row = con.execute("SELECT shipped FROM world_type_version WHERE world_type = ? AND "
                          "version = ?", (world_type, version)).fetchone()
    return bool(row and row[0])


def new_id(project: Project, name: str) -> str:
    base = "wt_" + (re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_") or "world")
    taken = {t["id"] for t in load_all(project)}
    candidate, n = base, 2
    while candidate in taken:
        candidate, n = f"{base}_{n}", n + 1
    return candidate


def resolved_settings(project: Project, settings: dict[str, Any]) -> dict[str, Any]:
    """Scene settings with the terrain's world type looked up: the recipe's terrain settings and
    the world's seed, as the engine's viewport and checks take them (they have no database)."""
    terrain = settings.get("terrain")
    if not isinstance(terrain, dict) or "world_type" not in terrain:
        return settings
    out = copy.deepcopy(settings)
    found = recipe(project, terrain["world_type"], terrain.get("version", 1))
    if found is None:
        out.pop("terrain")  # (the cook names it)
        return out
    out["terrain"] = dict(found.get("terrain", {}))
    if found.get("graph"):
        out["terrain"]["graph"] = found["graph"]  # how the ground's height is made
    if "seed" in terrain:
        out["terrain"]["seed"] = terrain["seed"]
    return out


# Ground textures (milestone 6.0b; midgard/terrain.h's GroundTexture): each ground layer, and
# each biome's lush and dry ground, can take a texture set from the Asset Library: its maps by
# slot ("tex:" ids, as a material's) and a tile size in metres:
#     "terrain": {"ground": {"grass": {"textures": {"base_color": "tex:...", ...}, "tile": 4}},
#                 "biomes": [{..., "texture": {...}, "dry_texture": {...}}]}
GROUND_LAYERS = {"grass": "Grass", "dry_grass": "Dry grass", "sand": "Sand", "rock": "Rock",
                 "snow": "Snow", "paint_1": "Paint layer 1", "paint_2": "Paint layer 2"}
# The two paint layers: only painted (the Terrain panel's brush), each with a name of its own
# ("name", say "Path") and its texture set; untextured, earth and grey paving.
PAINT_LAYERS = ("paint_1", "paint_2")
GROUND_SLOTS = ("base_color", "normal", "metallic_roughness", "occlusion", "emissive")
# Each map's short name, for the tick boxes that switch one off ("off": a list of slots; the map
# is kept but neither drawn nor packed).
GROUND_MAP_NAMES = {"base_color": "Colour", "normal": "Normal", "metallic_roughness": "Metal/rough",
                    "occlusion": "AO", "emissive": "Emissive"}
DEFAULT_TILE = 4.0


def ground_textures(recipe: dict[str, Any]) -> list[str]:
    """Every texture a recipe's ground layers and biomes use (each once, in order)."""
    terrain = recipe.get("terrain", {}) if isinstance(recipe, dict) else {}
    found: list[dict] = list((terrain.get("ground") or {}).values())
    for b in terrain.get("biomes", []) or []:
        found += [b[k] for k in ("texture", "dry_texture") if isinstance(b.get(k), dict)]
    out: list[str] = []
    for g in found:
        off = g.get("off") or [] if isinstance(g, dict) else []
        for slot in GROUND_SLOTS:
            if slot in off:
                continue  # (switched off: not drawn, so not packed)
            t = (g.get("textures") or {}).get(slot) if isinstance(g, dict) else None
            if isinstance(t, str) and t and t not in out:
                out.append(t)
    return out


def placement_objects(recipe: dict[str, Any]) -> list[str]:
    """The objects a recipe's placement rules place (each once, in order)."""
    terrain = recipe.get("terrain", {}) if isinstance(recipe, dict) else {}
    out: list[str] = []
    for rule in terrain.get("placements", []) or []:
        object_id = rule.get("object") if isinstance(rule, dict) else None
        if isinstance(object_id, str) and object_id and object_id not in out:
            out.append(object_id)
    return out


def used_placement_objects(project: Project) -> list[tuple[str, str]]:
    """(who, object id) for the objects the placement rules of the world types the scenes are
    made with place."""
    out = []
    for world_type, version in sorted(used_versions(project)):
        found = recipe(project, world_type, version)
        name = next((t["name"] for t in load_all(project) if t["id"] == world_type), world_type)
        for object_id in placement_objects(found or {}):
            out.append((f"World type '{name}'", object_id))
    return out


def send_placement_objects(view, project: Project, settings: dict[str, Any],
                           sent: dict[str, int]) -> None:
    """The objects resolved scene settings' placement rules place, sent to an engine view
    (their meshes and textures, then their parts; once each, again when they change: `sent`
    remembers), so the view shows them over the ground."""
    from .objects import ObjectError, object_content
    for object_id in placement_objects({"terrain": settings.get("terrain") or {}}):
        obj = project.object(object_id)
        if obj is None:
            continue
        try:
            content = object_content(project, obj)
        except ObjectError:
            continue
        for content_id, (_path, data) in content.files.items():
            if sent.get(content_id) == hash(data):
                continue
            try:
                if data[:4] == b"glTF":
                    view.set_mesh(content_id, data)
                else:
                    view.set_texture(content_id, data)
            except RuntimeError:
                continue
            sent[content_id] = hash(data)
        parts = json.dumps(content.parts)
        if sent.get("object:" + object_id) != hash(parts):
            try:
                view.set_object(object_id, parts)
            except RuntimeError:
                continue
            sent["object:" + object_id] = hash(parts)


def paint_layer_names(project: Project, scene: str | None = None) -> dict[str, str]:
    """What the scene's world type calls its two paint layers ("Paint layer 1" unnamed)."""
    world = scene_world(project, scene)
    found = recipe(project, world["world_type"], world.get("version", 1)) if world else None
    ground = ((found or {}).get("terrain") or {}).get("ground") or {}
    return {k: (ground.get(k) or {}).get("name") or GROUND_LAYERS[k] for k in PAINT_LAYERS}


def send_ground_textures(view, project: Project, settings: dict[str, Any],
                         sent: dict[str, int]) -> None:
    """The ground textures resolved scene settings name, sent to an engine view (once each, again
    when a file changes; `sent` remembers): the view reads them as the ground needs them."""
    from . import textures
    for texture in ground_textures({"terrain": settings.get("terrain") or {}}):
        data = textures.texture_data(project.root, texture)
        if data is None or sent.get(texture) == hash(data):
            continue
        try:
            view.set_texture(texture, data)
        except RuntimeError:
            continue  # (drawn without it)
        sent[texture] = hash(data)


def used_ground_textures(project: Project) -> list[tuple[str, str]]:
    """(who, texture) for the ground textures of the world types the scenes are made with."""
    out = []
    for world_type, version in sorted(used_versions(project)):
        found = recipe(project, world_type, version)
        name = next((t["name"] for t in load_all(project) if t["id"] == world_type), world_type)
        for texture in ground_textures(found or {}):
            out.append((f"World type '{name}'", texture))
    return out


def used_versions(project: Project) -> set[tuple[str, int]]:
    """The world type versions the project's scenes are made with."""
    with project.connect() as con:
        rows = con.execute("SELECT settings FROM scene").fetchall()
    out = set()
    for (text,) in rows:
        terrain = json.loads(text).get("terrain")
        if isinstance(terrain, dict) and "world_type" in terrain:
            out.add((terrain["world_type"], int(terrain.get("version", 1))))
    return out


def mark_shipped(project: Project) -> None:
    """An export: the versions its scenes use are shipped, kept unchanged from now on."""
    with project.connect() as con:
        for world_type, version in used_versions(project):
            con.execute("UPDATE world_type_version SET shipped = 1 WHERE world_type = ? AND "
                        "version = ?", (world_type, version))


class SaveRecipeCommand(QUndoCommand):
    """Save a world type's recipe: over its latest version, or as a new one if that version has
    shipped. `self.version` is the version saved. Undo puts back what was there."""

    def __init__(self, project: Project, world_type: str, new_recipe: dict[str, Any],
                 name: str | None = None):
        super().__init__("Change world type")
        self.project, self.world_type = project, world_type
        self.recipe = copy.deepcopy(new_recipe)
        with project.connect() as con:
            row = con.execute("SELECT name, version FROM world_type WHERE id = ?",
                              (world_type,)).fetchone()
        self.created = row is None
        self.old_name = None if row is None else row[0]
        self.name = name if name is not None else (self.old_name or world_type)
        self.old_version = None if row is None else row[1]
        self.old_recipe = None if row is None else recipe(project, world_type, row[1])
        if row is None:
            self.version = 1
        elif is_shipped(project, world_type, row[1]):
            self.version = row[1] + 1
        else:
            self.version = row[1]

    def redo(self) -> None:
        with self.project.connect() as con:
            if self.created:
                con.execute("INSERT INTO world_type (id, name, version) VALUES (?, ?, ?)",
                            (self.world_type, self.name, self.version))
            else:
                con.execute("UPDATE world_type SET name = ?, version = ? WHERE id = ?",
                            (self.name, self.version, self.world_type))
            con.execute("INSERT INTO world_type_version (world_type, version, recipe) "
                        "VALUES (?, ?, ?) ON CONFLICT (world_type, version) DO UPDATE SET "
                        "recipe = excluded.recipe",
                        (self.world_type, self.version, json.dumps(self.recipe)))
        self.project.world_types_changed.emit(self.world_type)

    def undo(self) -> None:
        with self.project.connect() as con:
            if self.created:
                con.execute("DELETE FROM world_type WHERE id = ?", (self.world_type,))
            else:
                if self.version != self.old_version:  # a new version: gone again
                    con.execute("DELETE FROM world_type_version WHERE world_type = ? AND "
                                "version = ?", (self.world_type, self.version))
                else:
                    con.execute("UPDATE world_type_version SET recipe = ? WHERE world_type = ? "
                                "AND version = ?", (json.dumps(self.old_recipe),
                                                    self.world_type, self.version))
                con.execute("UPDATE world_type SET name = ?, version = ? WHERE id = ?",
                            (self.old_name, self.old_version, self.world_type))
        self.project.world_types_changed.emit(self.world_type)


class WorldTypeError(Exception):
    """A world type that can't be deleted (one of its versions shipped, or a scene uses it)."""


class DeleteWorldTypeCommand(QUndoCommand):
    """Delete a world type and every version of it (none shipped, no scene made with it). Undo
    puts them all back."""

    def __init__(self, project: Project, world_type: str):
        super().__init__("Delete world type")
        self.project, self.world_type = project, world_type
        if world_type in {w for w, _v in used_versions(project)}:
            raise WorldTypeError("A scene's world is made from this world type: choose another "
                                 "there first.")
        with project.connect() as con:
            self.row = con.execute("SELECT name, version FROM world_type WHERE id = ?",
                                   (world_type,)).fetchone()
            self.versions = con.execute("SELECT version, recipe, shipped FROM world_type_version "
                                        "WHERE world_type = ?", (world_type,)).fetchall()
        if self.row is None:
            raise WorldTypeError(f"There is no world type {world_type}.")
        if any(shipped for _v, _r, shipped in self.versions):
            raise WorldTypeError("A shipped game's worlds are made with this world type: it is "
                                 "kept so they still generate the same.")

    def redo(self) -> None:
        with self.project.connect() as con:
            con.execute("DELETE FROM world_type WHERE id = ?", (self.world_type,))
        self.project.world_types_changed.emit(self.world_type)

    def undo(self) -> None:
        with self.project.connect() as con:
            con.execute("INSERT INTO world_type (id, name, version) VALUES (?, ?, ?)",
                        (self.world_type, *self.row))
            con.executemany("INSERT INTO world_type_version (world_type, version, recipe, "
                            "shipped) VALUES (?, ?, ?, ?)",
                            [(self.world_type, *v) for v in self.versions])
        self.project.world_types_changed.emit(self.world_type)


def scene_world(project: Project, scene: str | None = None) -> dict[str, Any] | None:
    """The scene's world: {"world_type", "version", "seed"?}, or None (no terrain, or terrain
    settings from before world types)."""
    terrain = project.scene_settings(scene).get("terrain")
    if isinstance(terrain, dict) and "world_type" in terrain:
        return terrain
    return None
