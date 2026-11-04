# SPDX-License-Identifier: Apache-2.0
"""Surface shaders from the editor's side (milestone 5.17 step 2): a material names one, the
cook compiles it into the package, and a broken one is warned of with its line."""
import subprocess
import zipfile

import pytest
from test_materials import add_box

from midgard_editor import about, engine, surface_shaders
from midgard_editor.cook import cook

needs_compiler = pytest.mark.skipif(
    engine.module() is None or not hasattr(engine.module(), "compile_surface_shader"),
    reason="needs the engine module with the surface shader compiler")

STRIPES = """void surface(SurfaceInput i, inout Surface s) {
    float band = step(0.5, fract(i.uv.x * 6.0));
    s.color = mix(srgb(vec3(1.0, 0.0, 0.0)), vec3(0.0, 0.0, 0.0), band);
    s.emission = s.color * 2.0;
}
"""


def write_shader(project, name: str, code: str) -> str:
    folder = project.root / surface_shaders.FOLDER
    folder.mkdir(exist_ok=True)
    (folder / (name + surface_shaders.SUFFIX)).write_text(code, encoding="utf-8")
    return surface_shaders.ref(name)


def test_shader_names_and_paths(tmp_path):
    assert surface_shaders.parse("shd:stripes") == "stripes"
    assert surface_shaders.parse("tex:a.png") is None
    assert surface_shaders.parse("shd:../secret") is None
    assert surface_shaders.pak_path("shd:stripes") == "shaders/stripes.spv"
    assert surface_shaders.source_path(tmp_path, "shd:stripes") == \
        tmp_path / "shaders" / "stripes.glsl"
    (tmp_path / "shaders").mkdir()
    (tmp_path / "shaders" / "b.glsl").write_text("")
    (tmp_path / "shaders" / "a.glsl").write_text("")
    assert surface_shaders.names(tmp_path) == ["a", "b"]
    assert surface_shaders.components_shader({"material": {"shader": "shd:a"}}) == "shd:a"
    assert surface_shaders.components_shader({}) == ""


def test_without_the_engine_a_shader_is_not_compiled(monkeypatch):
    monkeypatch.setattr(engine, "module", lambda: None)
    result = surface_shaders.compile_code("void surface(SurfaceInput i, inout Surface s) {}",
                                          "nothing-here")
    assert not result.ok and "engine" in result.messages[0]["message"]


@needs_compiler
def test_a_shader_compiles_and_a_mistake_has_its_line():
    assert surface_shaders.compile_code(STRIPES).ok
    broken = surface_shaders.compile_code(STRIPES.replace("s.emission", "s.emision"))
    assert not broken.ok
    assert broken.messages[0]["line"] == 4
    assert surface_shaders.describe(broken.messages[0]).startswith("line 4: ")


@needs_compiler
def test_the_cook_puts_a_materials_shader_in_the_package(project, tmp_path):
    shader = write_shader(project, "stripes", STRIPES)
    add_box(project, {"material": {"color": [0.8, 0.8, 0.8, 1.0], "shader": shader}})
    result = cook(project, tmp_path / "pkg")
    assert not [w for w in result.warnings if "shader" in w], result.warnings
    with zipfile.ZipFile(result.out_dir / "game.pak") as pak:
        assert pak.read("shaders/stripes.spv")[:4] == b"\x03\x02\x23\x07"  # (SPIR-V)


@needs_compiler
def test_the_cook_warns_of_a_broken_or_missing_shader(project, tmp_path):
    shader = write_shader(project, "broken", STRIPES.replace("s.emission", "s.emision"))
    add_box(project, {"material": {"shader": shader}})
    result = cook(project, tmp_path / "pkg")
    said = [w for w in result.warnings if "surface shader" in w]
    assert len(said) == 1, result.warnings  # (the engine's own warning isn't repeated)
    assert "'broken', which doesn't compile" in said[0] and "line 4: " in said[0]
    assert "magenta" in said[0]
    with zipfile.ZipFile(result.out_dir / "game.pak") as pak:
        assert "shaders/broken.spv" not in pak.namelist()

    add_box(project, {"material": {"shader": "shd:gone"}})
    said = [w for w in cook(project, tmp_path / "pkg2").warnings if "surface shader" in w]
    assert len(said) == 1 and "'gone', which is not in the project" in said[0]


@needs_compiler
@pytest.mark.skipif(not about.runtime_candidates(), reason="engine not built here")
def test_the_game_draws_a_surface_shader(project, tmp_path):
    shader = write_shader(project, "stripes", STRIPES)
    add_box(project, {"transform": {"position": [0.0, 0.75, -1.5],
                                    "scale": [1.5, 1.5, 1.5]},
                      "material": {"color": [1.0, 1.0, 1.0, 1.0], "shader": shader}})
    out = cook(project, tmp_path / "pkg").out_dir
    shot = tmp_path / "shot.png"
    run = subprocess.run([str(about.runtime_candidates()[0]), "--package", str(out),
                          "--screenshot", str(shot), "--size", "320x180"],
                         capture_output=True, text=True, timeout=60)
    if run.returncode != 0 and "Vulkan" in (run.stderr + run.stdout):
        pytest.skip("no Vulkan driver available: " + run.stderr.strip())
    assert run.returncode == 0, run.stderr
    from PySide6.QtGui import QImage
    image = QImage(str(shot))
    red = sum(1 for y in range(0, image.height(), 2) for x in range(0, image.width(), 2)
              if (c := image.pixelColor(x, y)).red() > c.green() + 80
              and c.red() > c.blue() + 80)
    assert red > 50, f"only {red} red pixels: the shader wasn't drawn"


def test_a_shader_file_is_copied_into_the_project(project, tmp_path):
    outside = tmp_path / "glow.glsl"
    outside.write_text(STRIPES, encoding="utf-8")
    assert surface_shaders.copy_shader(outside, project.root) == "shd:glow"
    assert (project.root / "shaders" / "glow.glsl").read_text(encoding="utf-8") == STRIPES
    assert surface_shaders.copy_shader(outside, project.root) == "shd:glow"  # (the same file)
    outside.write_text(STRIPES + "// changed\n", encoding="utf-8")
    assert surface_shaders.copy_shader(outside, project.root) == "shd:glow 2"  # (never over)
    inside = project.root / "shaders" / "glow.glsl"
    assert surface_shaders.copy_shader(inside, project.root) == "shd:glow"


def test_the_material_editor_picks_a_shader_and_undoes_it(qtbot, project):
    from midgard_editor.docks.material_editor import EntityMaterial, MaterialEditor
    write_shader(project, "stripes", STRIPES)
    write_shader(project, "broken", STRIPES.replace("s.emission", "s.emision"))
    add_box(project)
    editor = MaterialEditor(project, EntityMaterial(project, "e000050"))
    qtbot.addWidget(editor)
    names = [editor.shader.itemText(i) for i in range(editor.shader.count())]
    assert names == ["None (Midgard's own material)", "broken", "stripes"]
    assert editor.shader_status.text() == ""

    editor.set_shader("shd:stripes")
    material = lambda: project.entity("e000050").components["material"]  # noqa: E731
    assert material()["shader"] == "shd:stripes"
    assert editor.shader.currentData() == "shd:stripes"
    if engine.module() is not None and hasattr(engine.module(), "compile_surface_shader"):
        assert editor.shader_status.text() == "The shader compiles."
        editor.set_shader("shd:broken")
        assert "magenta" in editor.shader_status.text()
        assert "line 4: " in editor.shader_status.text()
        project.undo_stack.undo()
    project.undo_stack.undo()
    assert "shader" not in material()
    assert editor.shader.currentData() == ""
