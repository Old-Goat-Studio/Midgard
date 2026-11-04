# SPDX-License-Identifier: Apache-2.0
"""Surface shaders a material uses (milestone 5.17).

A surface shader is a project file, shaders/<name>.glsl, holding a function

    void surface(SurfaceInput i, inout Surface s) { ... }

that changes how a mesh's surface looks (engine/render/shaders/surface.glsl has what it can
read and change). A material names one by content id, "shd:<name>". The engine compiles it to
SPIR-V (the editor does, for the package and for its views; games carry only the SPIR-V). One
that doesn't compile is drawn bright magenta, and its mistakes are reported with their lines.

Settings (step 3): a shader declares them in its code ("setting float speed = 2.0;  // Speed,
0 .. 10"); each material using it picks its own values, kept by name in the material's
"shader_settings" ({"speed": 4.0, "tint": [1, 0, 0], "pattern": "tex:textures/a.png"}). For
the engine they are laid out in the slots the compiler gave them (drawn()): "shader_values"
(a list of up to 16 [x, y, z, w]) and "shader_textures" (up to 4 content ids).
"""
import hashlib
import shutil
from dataclasses import dataclass, field
from pathlib import Path

from . import engine

PREFIX = "shd:"
FOLDER = "shaders"
SUFFIX = ".glsl"


@dataclass
class Result:
    spirv: bytes | None  # None: it didn't compile (or there's no engine to compile it)
    messages: list[dict] = field(default_factory=list)  # {"line", "error", "message"}
    # {"name", "type", "label", "value", "min", "max", "slot", "line"} (the engine's)
    settings: list[dict] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return self.spirv is not None


def ref(name: str) -> str:
    return PREFIX + name


def parse(shader: str) -> str | None:
    """The shader's name for a "shd:" id; None for others."""
    if not shader.startswith(PREFIX):
        return None
    name = shader[len(PREFIX):]
    if not name or "/" in name or "\\" in name or name.startswith("."):
        return None
    return name


def source_path(project_root: Path, shader: str) -> Path | None:
    name = parse(shader)
    return None if name is None else Path(project_root) / FOLDER / (name + SUFFIX)


def pak_path(shader: str) -> str:
    return f"shaders/{parse(shader)}.spv"


def names(project_root: Path) -> list[str]:
    """The project's surface shaders, by name."""
    folder = Path(project_root) / FOLDER
    if not folder.is_dir():
        return []
    return sorted(p.stem for p in folder.glob("*" + SUFFIX) if p.is_file())


def copy_shader(file: Path, project_root: Path) -> str:
    """A shader file for a material, as its content id. A file already in the project's
    shaders folder stays; another is copied there (never over a different file: a new name)."""
    file = Path(file).resolve()
    folder = (Path(project_root) / FOLDER).resolve()
    if file.parent == folder and file.suffix == SUFFIX:
        return ref(file.stem)
    folder.mkdir(parents=True, exist_ok=True)
    stem = "".join(c if c.isalnum() or c in "-_ " else "_" for c in file.stem).strip() or "shader"
    stem = stem.lstrip(".") or "shader"
    dest, n = folder / (stem + SUFFIX), 2
    while dest.exists() and dest.read_bytes() != file.read_bytes():
        dest, n = folder / f"{stem} {n}{SUFFIX}", n + 1
    if not dest.exists():
        shutil.copyfile(file, dest)
    return ref(dest.stem)


def components_shader(components: dict) -> str:
    return components.get("material", {}).get("shader", "") or ""


_compiled: dict[str, Result] = {}  # by the code's hash (and name)


def compile_code(code: str, name: str = "surface") -> Result:
    """The code compiled (remembered by its text, so asking again is quick)."""
    key = hashlib.sha256((name + "\0" + code).encode("utf-8")).hexdigest()
    if key in _compiled:
        return _compiled[key]
    midgard = engine.module()
    if midgard is None or not hasattr(midgard, "compile_surface_shader"):
        return Result(None, [{"line": 0, "error": True,
                              "message": "the engine isn't here to compile it"}])
    out = midgard.compile_surface_shader(code, name)
    result = Result(bytes(out["spirv"]) if out["ok"] else None, list(out["messages"]),
                    list(out.get("settings", [])))
    _compiled[key] = result
    return result


def compile_shader(project_root: Path, shader: str) -> Result:
    """A material's shader compiled from its file."""
    path = source_path(project_root, shader)
    if path is None:
        return Result(None, [{"line": 0, "error": True,
                              "message": f"'{shader}' isn't a surface shader's name"}])
    try:
        code = path.read_text(encoding="utf-8")
    except OSError:
        return Result(None, [{"line": 0, "error": True,
                              "message": f"{FOLDER}/{path.name} isn't in the project"}])
    return compile_code(code, parse(shader) or "surface")


TEXTURE_TYPES = ("texture", "data_texture")
MAX_VALUES = 16
MAX_TEXTURES = 4


def setting_value(setting: dict, chosen) -> list[float] | str:
    """A setting's value as the engine takes it: its numbers (as many as its type has), or a
    texture's content id ("": none). A chosen value that doesn't fit is its default."""
    if setting["type"] in TEXTURE_TYPES:
        return chosen if isinstance(chosen, str) else ""
    default = [float(v) for v in setting.get("value", [])] or [0.0]
    if isinstance(chosen, bool):
        chosen = [1.0 if chosen else 0.0]
    elif isinstance(chosen, int | float):
        chosen = [float(chosen)] * len(default)
    if not isinstance(chosen, list) or len(chosen) != len(default) or \
            not all(isinstance(v, int | float) and not isinstance(v, bool) for v in chosen):
        return default
    if setting["type"] == "int":
        chosen = [float(round(chosen[0]))]
    return [float(v) for v in chosen]


def pack(settings: list[dict], chosen: dict) -> tuple[list[list[float]], list[str]]:
    """The values a material chose (by name), laid out in the shader's slots: (values,
    textures); a setting it didn't choose has the shader's own value."""
    count = max([s["slot"] + 1 for s in settings if s["type"] not in TEXTURE_TYPES],
                default=0)
    maps = max([s["slot"] + 1 for s in settings if s["type"] in TEXTURE_TYPES], default=0)
    values = [[0.0, 0.0, 0.0, 0.0] for _ in range(min(count, MAX_VALUES))]
    textures = [""] * min(maps, MAX_TEXTURES)
    for setting in settings:
        value = setting_value(setting, chosen.get(setting["name"]))
        if setting["type"] in TEXTURE_TYPES:
            if setting["slot"] < len(textures):
                textures[setting["slot"]] = value
        elif setting["slot"] < len(values):
            values[setting["slot"]] = (list(value) + [0.0, 0.0, 0.0])[:4]
    return values, textures


def setting_textures(material: dict) -> list[str]:
    """The textures a material's shader settings use (by content id)."""
    chosen = material.get("shader_settings", {})
    if not isinstance(chosen, dict):
        return []
    return [v for v in chosen.values() if isinstance(v, str) and v]


def drawn(project_root: Path, components: dict) -> dict:
    """The components as the engine draws them: a material's shader settings laid out in its
    shader's slots (unchanged without a shader, or one that doesn't compile)."""
    material = components.get("material")
    if not isinstance(material, dict) or not material.get("shader"):
        return components
    result = compile_shader(project_root, material["shader"])
    if not result.ok:
        return components
    chosen = material.get("shader_settings", {})
    values, textures = pack(result.settings, chosen if isinstance(chosen, dict) else {})
    material = {**material}
    material.pop("shader_values", None)
    material.pop("shader_textures", None)
    if values:
        material["shader_values"] = values
    if any(textures):
        material["shader_textures"] = textures
    return {**components, "material": material}


def describe(message: dict) -> str:
    """A mistake as it reads in a warning: "line 3: 'roughnes' : no such field ..."."""
    line = message.get("line", 0)
    where = f"line {line}: " if line else ""
    return where + message.get("message", "")
