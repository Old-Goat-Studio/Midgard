# SPDX-License-Identifier: Apache-2.0
import json
import sqlite3
import subprocess
import zipfile

import pytest
from test_cook import parse_glb
from test_objects import needs_import, png, write_crate

from midgard_editor import about, engine, meshes, textures
from midgard_editor.cook import cook, used_mesh_assets
from midgard_editor.docks.material_editor import (
    EntityMaterial,
    MaterialDialog,
    MaterialEditor,
    ObjectMaterial,
)
from midgard_editor.docks.objects_panel import ObjectsPanel
from midgard_editor.objects import ObjectDef, object_content
from midgard_editor.scene_model import Entity

needs_encode = pytest.mark.skipif(
    engine.module() is None or not hasattr(engine.module(), "encode_texture"),
    reason="the engine module (with encode_texture) isn't built here")


@pytest.mark.parametrize("asset", list(meshes.PRIMITIVES))
def test_primitives_have_texture_coordinates(asset):
    mesh = meshes.primitive(asset)
    assert len(mesh.uvs) == len(mesh.positions)
    assert all(-1e-6 <= c <= 1 + 1e-6 for uv in mesh.uvs for c in uv)
    doc, _binary = parse_glb(meshes.to_glb(mesh))
    attributes = doc["meshes"][0]["primitives"][0]["attributes"]
    assert doc["accessors"][attributes["TEXCOORD_0"]]["count"] == len(mesh.positions)
    if engine.module() is not None:  # the engine reads it (and makes tangents from the uvs)
        engine.module().Viewport().set_mesh(asset, meshes.to_glb(mesh))


def test_texture_ids_name_a_file_and_its_colour_space(project, tmp_path):
    assert textures.ref("textures/bricks.png", "base_color") == "tex:textures/bricks.png"
    assert textures.ref("textures/bricks_n.png", "normal") == "tex:textures/bricks_n.png?linear"
    assert textures.parse("tex:textures/bricks_n.png?linear") == ("textures/bricks_n.png", False)
    assert textures.parse("obj:obj_crate/tex0") is None
    assert textures.pak_path("tex:a/b.png") == "images/a/b.png.ktx2"
    assert textures.pak_path("tex:a/b.png?linear") == "images/a/b.png.linear.ktx2"
    assert textures.describe("obj:obj_crate/tex2") == "the model's texture 2"

    (tmp_path / "bricks.png").write_bytes(png((200, 80, 60, 255)))
    assert textures.copy_texture(tmp_path / "bricks.png", project.root) == "textures/bricks.png"
    # The same file again is the same texture; another with the same name gets its own.
    assert textures.copy_texture(tmp_path / "bricks.png", project.root) == "textures/bricks.png"
    (tmp_path / "other").mkdir()
    (tmp_path / "other" / "bricks.png").write_bytes(png((10, 80, 60, 255)))
    assert textures.copy_texture(tmp_path / "other" / "bricks.png",
                                 project.root) == "textures/bricks_2.png"
    # A file already in the project stays where it is.
    write_crate(project.objects_dir / "crate")
    assert textures.copy_texture(project.objects_dir / "crate" / "red.png",
                                 project.root) == "objects/crate/red.png"


@needs_encode
def test_the_engine_encodes_image_files(tmp_path):
    (tmp_path / "a.png").write_bytes(png((200, 80, 60, 255), size=8))
    data = engine.module().encode_texture(tmp_path / "a.png", True)
    assert data[:12] == b"\xabKTX 20\xbb\r\n\x1a\n"
    (tmp_path / "bad.png").write_bytes(b"not an image")
    with pytest.raises(RuntimeError, match="can't be read"):
        engine.module().encode_texture(tmp_path / "bad.png", True)


def add_box(project, components=None) -> str:
    project.put_entity(Entity("e000050", "Box", None, {
        "transform": {"position": [0.0, 0.5, -1.5]}, "mesh": {"asset": "prim_cube"},
        "material": {"color": [0.8, 0.8, 0.8, 1.0]}, **(components or {})}))
    return "e000050"


def test_editing_an_entitys_material_is_undoable_and_drags_are_one_step(qtbot, project,
                                                                         tmp_path):
    add_box(project)
    editor = MaterialEditor(project, EntityMaterial(project, "e000050"))
    qtbot.addWidget(editor)
    material = lambda: project.entity("e000050").components["material"]  # noqa: E731
    depth = project.undo_stack.index()

    for value in (100, 300, 650):  # a slider dragged across
        editor.metallic.slider.setValue(value)
    assert material()["metallic"] == pytest.approx(0.65)
    assert project.undo_stack.index() == depth + 1
    editor.roughness.box.setValue(0.25)
    editor.set_color("color", [1.0, 0.0, 0.0])
    editor.set_alpha(0.5)
    editor.set_color("emissive", [0.2, 0.4, 0.6])
    assert material()["color"] == [1.0, 0.0, 0.0, 0.5]
    assert material()["emissive"] == [0.2, 0.4, 0.6]
    assert editor.roughness.slider.value() == 250

    (tmp_path / "bricks.png").write_bytes(png((200, 80, 60, 255)))
    editor.use_image("base_color", tmp_path / "bricks.png")
    editor.use_image("normal", tmp_path / "bricks.png")
    assert material()["textures"] == {"base_color": "tex:textures/bricks.png",
                                      "normal": "tex:textures/bricks.png?linear"}
    assert editor.slot_labels["normal"].text() == "textures/bricks.png"
    editor.set_texture("normal", None)
    assert material()["textures"] == {"base_color": "tex:textures/bricks.png"}
    assert used_mesh_assets(project) >= {"tex:textures/bricks.png"}  # the live link cooks it in

    while project.undo_stack.index() > depth:
        project.undo_stack.undo()
    assert material() == {"color": [0.8, 0.8, 0.8, 1.0]}
    assert editor.metallic.value() == 0.0  # the editor follows Undo


def test_the_inspector_and_objects_panel_open_the_material_editor(qtbot, project):
    from midgard_editor.docks.entity_form import EntityForm
    add_box(project)
    form = EntityForm(project, "e000050")
    qtbot.addWidget(form)
    dialog = form.edit_material()
    assert dialog.windowTitle() == "Material — Box"
    assert form.edit_material() is dialog  # the same material: the same window
    MaterialDialog.close_all()

    panel = ObjectsPanel()
    qtbot.addWidget(panel)
    panel.set_project(project)
    project.put_object(ObjectDef("obj_bench", "Bench", None, {}, []))
    panel.select_object("obj_bench")
    assert panel.edit_material(0) is None  # made in the editor: its parts have their own


@needs_import
def test_an_objects_material_changes_replace_the_models(qtbot, project, tmp_path):
    write_crate(project.objects_dir / "crate")
    project.put_object(ObjectDef("obj_crate", "Crate", "crate/crate.gltf"))
    target = ObjectMaterial(project, "obj_crate", 1)  # "green"
    editor = MaterialEditor(project, target)
    qtbot.addWidget(editor)
    assert target.title == "Crate: green"
    assert not editor.reset.isEnabled()

    editor.set_value("roughness", 0.1, "roughness")
    (tmp_path / "rust.png").write_bytes(png((150, 60, 20, 255)))
    editor.use_image("base_color", tmp_path / "rust.png")
    obj = project.object("obj_crate")
    assert obj.settings["materials"]["1"]["roughness"] == 0.1
    content = object_content(project, obj)
    green = content.parts[1]["components"]["material"]
    assert green["textures"] == {"base_color": "tex:textures/rust.png"}
    assert content.parts[0]["components"]["material"].get("roughness") != 0.1  # red as it was
    assert editor.reset.isEnabled()

    # Cooked: the changed material, with its image, goes into the package.
    project.put_entity(Entity("e000050", "Crate", None, {"object": {"id": "obj_crate"}}))
    if engine.module() is not None and hasattr(engine.module(), "encode_texture"):
        result = cook(project, tmp_path / "pkg")
        con = sqlite3.connect(result.out_dir / "HelloCube.db")
        parts = json.loads(con.execute("SELECT parts FROM object").fetchone()[0])
        con.close()
        assert parts[1]["components"]["material"]["roughness"] == 0.1
        with zipfile.ZipFile(result.out_dir / "game.pak") as pak:
            assert pak.read("images/textures/rust.png.ktx2")[:4] == b"\xabKTX"

    editor.reset_to_original()
    assert "materials" not in project.object("obj_crate").settings
    project.undo_stack.undo()
    assert project.object("obj_crate").settings["materials"]["1"]["roughness"] == 0.1


@needs_encode
def test_the_preview_shows_the_material(qtbot, project, tmp_path):
    add_box(project)
    editor = MaterialEditor(project, EntityMaterial(project, "e000050"))
    qtbot.addWidget(editor)
    editor.resize(400, 700)
    (tmp_path / "blue.png").write_bytes(png((20, 40, 230, 255)))
    editor.set_color("color", [1.0, 1.0, 1.0])
    editor.use_image("base_color", tmp_path / "blue.png")
    pixmap = editor.preview.pixmap()
    if pixmap is None or pixmap.isNull():
        pytest.skip("no offscreen rendering here: " + editor.preview.text())
    image = pixmap.toImage()
    blue = sum(1 for y in range(0, image.height(), 3) for x in range(0, image.width(), 3)
               if (c := image.pixelColor(x, y)).blue() > c.red() + 60
               and c.blue() > c.green() + 40)
    assert blue > 100, blue  # the textured ball


@needs_encode
@pytest.mark.skipif(not about.runtime_candidates(), reason="engine not built here")
def test_the_game_draws_a_primitive_with_an_image_texture(project, tmp_path):
    (tmp_path / "blue.png").write_bytes(png((20, 40, 230, 255)))
    relative = textures.copy_texture(tmp_path / "blue.png", project.root)
    add_box(project, {"transform": {"position": [0.0, 0.75, -1.5],
                                    "scale": [1.5, 1.5, 1.5]},
                      "material": {"color": [1.0, 1.0, 1.0, 1.0],
                                   "textures": {"base_color": textures.ref(relative,
                                                                           "base_color")}}})
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
    blue = sum(1 for y in range(0, image.height(), 2) for x in range(0, image.width(), 2)
               if (c := image.pixelColor(x, y)).blue() > c.red() + 60
               and c.blue() > c.green() + 40)
    assert blue > 50, f"only {blue} blue pixels: the texture wasn't drawn"
