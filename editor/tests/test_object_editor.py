# SPDX-License-Identifier: Apache-2.0
import json
import sqlite3

import pytest
from test_objects import needs_import, write_crate

from midgard_editor.cook import cook
from midgard_editor.docks.entity_form import EntityForm
from midgard_editor.docks.objects_panel import ObjectsPanel
from midgard_editor.docks.scene_panel import ScenePanel
from midgard_editor.objects import ObjectDef, ObjectRenderer, new_models, object_content


@pytest.fixture
def panel(qtbot, project):
    p = ObjectsPanel()
    qtbot.addWidget(p)
    p.set_project(project)
    return p


def test_importing_a_model_copies_it_and_makes_an_undoable_object(panel, project, tmp_path):
    write_crate(tmp_path / "download")
    object_id = panel.import_file(tmp_path / "download" / "crate.gltf")
    assert object_id == "obj_crate"
    # The model and the files it refers to are now in the project.
    assert (project.objects_dir / "crate" / "crate.gltf").is_file()
    assert (project.objects_dir / "crate" / "red.png").is_file()
    obj = project.object("obj_crate")
    assert (obj.name, obj.source) == ("Crate", "crate/crate.gltf")
    assert panel.selected() == ("object", "obj_crate")
    # Importing it again finds the same object.
    assert panel.import_file(project.objects_dir / "crate" / "crate.gltf") == "obj_crate"
    assert len(project.objects()) == 1

    project.undo_stack.undo()
    assert project.objects() == []
    # The file is still there: it shows as a model that isn't an object yet.
    assert panel.selected() == ("file", "crate/crate.gltf")
    project.undo_stack.redo()
    assert project.object("obj_crate") is not None


def test_settings_and_levels_of_detail_are_undoable_edits(panel, project, tmp_path):
    write_crate(project.objects_dir / "crate")
    write_crate(tmp_path / "far")
    with open(tmp_path / "far" / "crate.gltf", "a", encoding="utf-8") as f:
        f.write(" ")  # another model with the same name
    panel.import_file(project.objects_dir / "crate" / "crate.gltf")

    panel.set_setting("scale", 0.01)
    panel.set_setting("z_up", True)
    panel.set_setting("collider", "convex")
    assert project.object("obj_crate").settings == {"scale": 0.01, "z_up": True,
                                                    "collider": "convex"}
    assert panel.scale.value() == pytest.approx(0.01)
    assert panel.up_axis.currentData() is True
    panel.set_setting("collider", "box")  # back to the default: left out
    assert "collider" not in project.object("obj_crate").settings

    panel.add_lod(tmp_path / "far" / "crate.gltf")
    panel.add_lod(project.objects_dir / "crate" / "crate.gltf")
    lods = project.object("obj_crate").settings["lods"]
    assert [lod["distance"] for lod in lods] == [20.0, 40.0]
    assert lods[0]["source"] == "crate_2/crate.gltf"  # copied in beside the model's folder
    assert panel.lods.rowCount() == 2
    # A level of detail isn't offered as a model of its own.
    assert "crate_2/crate.gltf" not in new_models(project.objects_dir, project.objects())
    panel.set_lod_distance(1, 75.0)
    assert project.object("obj_crate").settings["lods"][1]["distance"] == 75.0
    panel.remove_lod(0)
    assert [lod["distance"] for lod in project.object("obj_crate").settings["lods"]] == [75.0]

    for _ in range(8):  # back to just after the import

        project.undo_stack.undo()
    assert project.object("obj_crate").settings == {}
    panel.name.setText("Big Crate")
    panel.name.editingFinished.emit()
    assert project.object("obj_crate").name == "Big Crate"


def test_deleting_an_object_can_be_undone(panel, project):
    project.put_object(ObjectDef("obj_bench", "Bench", None, {}, [
        {"name": "seat", "components": {"mesh": {"asset": "prim_cube"}}}]))
    panel.select_object("obj_bench")
    panel.delete_selected(confirm=False)
    assert project.objects() == []
    project.undo_stack.undo()
    assert project.object("obj_bench").parts[0]["name"] == "seat"


def test_add_object_places_instances_and_new_models(qtbot, project):
    scene = ScenePanel()
    qtbot.addWidget(scene)
    scene.set_project(project)
    project.put_object(ObjectDef("obj_bench", "Bench", None, {}, [
        {"name": "seat", "components": {"mesh": {"asset": "prim_cube"}}}]))
    write_crate(project.objects_dir / "crate")
    scene.fill_object_menu()
    labels = [a.text() for a in scene.object_menu.actions() if a.text()]
    assert labels[:2] == ["Bench", "crate/crate.gltf (new)"]
    assert labels[-1] == "Objects Panel…"

    first = scene.add_object("obj_bench")
    second = scene.add_object("obj_bench")
    assert project.entity(first).components["object"] == {"id": "obj_bench"}
    assert project.entity(second).name == "Bench 2"
    # A new model: an object made of it and an instance, one step to undo.
    placed = scene.add_model("crate/crate.gltf")
    assert project.entity(placed).components["object"] == {"id": "obj_crate"}
    project.undo_stack.undo()
    assert project.entity(placed) is None and project.object("obj_crate") is None


def test_the_inspector_chooses_an_instances_object(qtbot, project):
    from midgard_editor.scene_model import Entity
    for oid, name in (("obj_a", "Arch"), ("obj_b", "Bench")):
        project.put_object(ObjectDef(oid, name, None, {}, []))
    project.put_entity(Entity("e000050", "Thing", None, {"object": {"id": "obj_a"}}))
    form = EntityForm(project, "e000050")
    qtbot.addWidget(form)
    assert form.object.currentData() == "obj_a"
    form.object.setCurrentIndex(form.object.findData("obj_b"))
    assert project.entity("e000050").components["object"] == {"id": "obj_b"}
    with qtbot.waitSignal(form.open_object) as signal:
        form.open_object.emit("obj_b")
    assert signal.args == ["obj_b"]
    project.delete_object("obj_b")  # gone from the library: shown as missing
    assert "missing" in form.object.currentText()


def test_the_main_window_has_an_objects_panel(window, project):
    window.set_project(project)
    project.put_object(ObjectDef("obj_bench", "Bench", None, {}, []))
    window.place_object("obj_bench")
    assert any(e.components.get("object") == {"id": "obj_bench"} for e in project.entities())
    window.edit_object("obj_bench")
    assert window.objects_panel.selected() == ("object", "obj_bench")


@needs_import
def test_levels_of_detail_are_cooked(project, tmp_path):
    write_crate(project.objects_dir / "crate")
    write_crate(project.objects_dir / "crate_far")
    project.put_object(ObjectDef("obj_crate", "Crate", "crate/crate.gltf", {
        "lods": [{"source": "crate_far/crate.gltf", "distance": 30.0},
                 {"source": "missing.glb", "distance": 60.0}]}))
    from midgard_editor.scene_model import Entity
    project.put_entity(Entity("e000050", "Crate", None, {"object": {"id": "obj_crate"}}))
    result = cook(project, tmp_path / "pkg")
    assert any("missing.glb" in w for w in result.warnings)
    ids = {c["id"] for c in result.manifest["content"]}
    assert {"obj:obj_crate/0/lod1", "obj:obj_crate/1/lod1"} <= ids
    con = sqlite3.connect(result.out_dir / "HelloCube.db")
    parts = json.loads(con.execute("SELECT parts FROM object").fetchone()[0])
    con.close()
    assert parts[0]["components"]["mesh"]["lods"] == [
        {"asset": "obj:obj_crate/0/lod1", "distance": 30.0}]


@needs_import
def test_the_preview_and_thumbnails_draw_the_object(panel, project):
    write_crate(project.objects_dir / "crate")
    panel.import_file(project.objects_dir / "crate" / "crate.gltf")
    obj = project.object("obj_crate")
    renderer = ObjectRenderer()
    content = object_content(project, obj)
    for content_id, (_path, data) in content.files.items():
        renderer.send(content_id, data)
    image = renderer.render(obj.id, content.parts, content.bounds, 160, 120)
    if image is None:
        pytest.skip(f"no offscreen rendering here: {renderer.error}")
    red = green = 0
    for y in range(0, 120, 2):
        for x in range(0, 160, 2):
            c = image.pixelColor(x, y)
            red += c.red() > 2 * c.green() + 40 and c.red() > 2 * c.blue() + 40
            green += c.green() > c.red() + 30 and c.green() > c.blue() + 30
    assert red > 50 and green > 50, (red, green)  # the textured part, and the tinted one
    # The panel keeps a thumbnail, used again while the object is unchanged.
    assert panel.thumbnail(obj) is not None
    thumb = project.root / "build" / "thumbnails" / "obj_crate.png"
    assert thumb.is_file()
    assert panel._cached_thumbnail(obj) is not None
    panel.set_setting("scale", 2.0)
    assert panel._cached_thumbnail(project.object("obj_crate")) is None  # out of date


class RecordingView:
    """Stands in for the engine's Viewport where the tests have no 3D view."""

    def __init__(self):
        self.calls = []

    def __getattr__(self, name):
        return lambda *args: self.calls.append((name, *args))


def test_the_viewport_sends_objects_its_instances_place(qtbot, project):
    from midgard_editor.docks.viewport import ViewportPanel
    from midgard_editor.scene_model import Entity

    panel = ViewportPanel()
    qtbot.addWidget(panel)
    view = panel.view = RecordingView()
    project.put_object(ObjectDef("obj_bench", "Bench", None, {}, [
        {"name": "seat", "components": {"mesh": {"asset": "prim_wedge"}}}]))
    panel.set_project(project)
    assert not [c for c in view.calls if c[0] == "set_object"]  # nothing places it yet

    project.put_entity(Entity("e000050", "Bench", None, {"object": {"id": "obj_bench"}}))
    sent = [c[:2] for c in view.calls if c[0] in ("set_object", "set_mesh")]
    assert sent[-2:] == [("set_mesh", "prim_wedge"), ("set_object", "obj_bench")]
    # The template changes: the view gets it again; an unchanged one isn't sent twice.
    view.calls.clear()
    project.put_object(ObjectDef("obj_bench", "Bench", None, {}, []))
    assert [c[:3] for c in view.calls if c[0] == "set_object"] == [
        ("set_object", "obj_bench", "[]")]
    view.calls.clear()
    project.put_object(ObjectDef("obj_bench", "Long Bench", None, {}, []))
    assert not [c for c in view.calls if c[0] == "set_object"]
    # Deleted: its instances show nothing (and Undo would bring it back the same way).
    project.put_object(ObjectDef("obj_bench", "Bench", None, {}, [{"name": "seat"}]))
    view.calls.clear()
    project.delete_object("obj_bench")
    assert [c[:3] for c in view.calls if c[0] == "set_object"] == [
        ("set_object", "obj_bench", "[]")]
