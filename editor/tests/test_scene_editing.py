# SPDX-License-Identifier: Apache-2.0
import json

from PySide6.QtWidgets import QAbstractItemView

from midgard_editor.docks.entity_form import EntityForm
from midgard_editor.docks.scene_panel import ScenePanel
from midgard_editor.project import FOLDERS, LAYOUT, Project
from midgard_editor.scene_model import MAIN_SCENE, Entity, descendants, parents_first

WORLD, SKY, PLAYER = "e000006", "e000005", "e000004"
Drop = QAbstractItemView.DropIndicatorPosition


def tree(panel: ScenePanel) -> dict[str, str | None]:
    """The panel's tree as entity id -> the id of the item it sits under."""
    out = {}
    for item in panel._all_items():
        parent = item.parent()
        out[item.data(0, 256)] = parent.data(0, 256) if parent is not None else None
    return out


def test_new_projects_have_the_default_scene(project):
    by_name = {e.name: e for e in project.entities()}
    assert sorted(by_name) == ["Camera", "Ground", "Player", "Sky", "Sun", "World"]
    assert by_name["World"].is_folder and by_name["World"].parent is None
    assert {e.parent for e in project.entities() if e.name != "World"} == {WORLD}
    assert by_name["Sky"].components["sky"] == {"color": [0.08, 0.11, 0.18],
                                                "ambient": [0.18, 0.2, 0.26]}
    assert project.next_entity_id() == "e000007"
    assert all((project.root / f).is_dir() for f in FOLDERS)
    assert project.config["layout"] == LAYOUT


def test_projects_without_a_scene_get_one_when_opened(project):
    with project.connect() as con:
        con.execute("DELETE FROM entity")
        con.execute("DELETE FROM scene")
    reopened = Project.open(project.config_path)
    assert len(reopened.entities(MAIN_SCENE)) == 6


def test_older_projects_get_the_new_layout_when_opened(project):
    # As an editor before layout 1 left it: no World, no Sky, the sky in the scene settings,
    # models in assets/ and some folders missing.
    with project.connect() as con:
        con.execute("UPDATE entity SET parent = NULL")
        con.execute("DELETE FROM entity WHERE id IN (?, ?)", (SKY, WORLD))
        con.execute("UPDATE scene SET settings = ?",
                    (json.dumps({"sky": [0.5, 0.6, 0.7], "ambient": [0.1, 0.1, 0.1]}),))
    del project.config["layout"]
    project._write_config()
    for folder in ("objects", "audio"):
        (project.root / folder).rmdir()
    (project.root / "assets" / "rocks").mkdir(parents=True)
    (project.root / "assets" / "rocks" / "boulder.glb").write_bytes(b"glb")

    reopened = Project.open(project.config_path)
    entities = reopened.entities()
    world = next(e for e in entities if e.is_folder)
    sky = next(e for e in entities if "sky" in e.components)
    assert world.name == "World" and world.parent is None
    assert {e.name for e in entities if e.parent == world.id} == {
        "Camera", "Sun", "Ground", "Player", "Sky"}
    assert sky.components["sky"] == {"color": [0.5, 0.6, 0.7], "ambient": [0.1, 0.1, 0.1]}
    assert world.id != sky.id
    assert (reopened.root / "objects" / "rocks" / "boulder.glb").read_bytes() == b"glb"
    assert not (reopened.root / "assets").exists()
    assert "asset:rocks/boulder.glb" in reopened.mesh_assets()  # the same id as before
    assert all((reopened.root / f).is_dir() for f in FOLDERS)
    assert json.loads(reopened.config_path.read_text("utf-8"))["layout"] == LAYOUT
    # Only once: a World the user later deletes doesn't come back.
    reopened.delete_entity(world.id)
    assert Project.open(project.config_path).entities() == []


def test_parents_come_before_their_children():
    entities = [Entity("e3", "Child", "e2"), Entity("e1", "Top"), Entity("e2", "Folder", "e1"),
                Entity("e4", "Loop A", "e5"), Entity("e5", "Loop B", "e4")]
    order = [e.id for e in parents_first(entities)]
    assert order.index("e1") < order.index("e2") < order.index("e3")
    assert sorted(order) == ["e1", "e2", "e3", "e4", "e5"]  # a loop doesn't hang or drop
    assert [e.id for e in descendants(entities, "e1")] == ["e2", "e3"]


def test_scene_panel_adds_and_deletes_with_undo(qtbot, project):
    panel = ScenePanel()
    qtbot.addWidget(panel)
    panel.set_project(project)
    assert tree(panel) == {WORLD: None, "e000001": WORLD, "e000002": WORLD, "e000003": WORLD,
                           PLAYER: WORLD, SKY: WORLD}
    panel.add_cube_entity()  # nothing selected: at the top
    assert panel.selected_id() == "e000007" and tree(panel)["e000007"] is None
    assert project.entity("e000007").name == "Cube"
    panel.delete_selected()
    assert project.entity("e000007") is None
    project.undo_stack.undo()  # undo the delete
    assert project.entity("e000007") is not None
    project.undo_stack.undo()  # undo the add
    assert len(tree(panel)) == 6

    panel.select(PLAYER)  # an entity in a folder: new ones go beside it
    panel.add_primitive("prim_sphere")
    assert project.entity(panel.selected_id()).parent == WORLD


def test_folders_group_entities_and_drops_move_them(qtbot, project):
    panel = ScenePanel()
    qtbot.addWidget(panel)
    panel.set_project(project)
    panel.add_folder()  # at the top
    props = panel.selected_id()
    assert project.entity(props).name == "Folder" and project.entity(props).is_folder
    panel.add_folder()  # inside the selected folder
    inner = panel.selected_id()
    assert project.entity(inner).parent == props and project.entity(inner).name == "Folder 2"

    panel.move_entity(PLAYER, props)
    assert tree(panel)[PLAYER] == props
    assert project.undo_stack.undoText() == "Change Player moved to Folder"
    panel.move_entity(props, inner)  # a folder into its own folder: refused
    assert project.entity(props).parent is None
    panel.move_entity(PLAYER, None)
    assert project.entity(PLAYER).parent is None
    project.undo_stack.undo()
    project.undo_stack.undo()
    assert project.entity(PLAYER).parent == WORLD

    # What a drop means: onto a folder goes in, onto anything else is refused, and beside an
    # entity goes into that entity's folder.
    items = {item.data(0, 256): item for item in panel._all_items()}
    assert panel.tree.drop_parent(items[props], Drop.OnItem) == (True, props)
    assert panel.tree.drop_parent(items[SKY], Drop.OnItem) == (False, None)
    assert panel.tree.drop_parent(items[SKY], Drop.AboveItem) == (True, WORLD)
    assert panel.tree.drop_parent(items[WORLD], Drop.BelowItem) == (True, None)
    assert panel.tree.drop_parent(None, Drop.OnViewport) == (True, None)

    # Closed folders stay closed when the tree is rebuilt.
    items[WORLD].setExpanded(False)
    panel.refresh()
    assert not {i.data(0, 256): i for i in panel._all_items()}[WORLD].isExpanded()


def test_deleting_a_folder_deletes_its_contents_and_undo_brings_them_back(qtbot, project):
    panel = ScenePanel()
    qtbot.addWidget(panel)
    panel.set_project(project)
    panel.select(WORLD)
    panel.delete_selected()
    assert project.entities() == [] and tree(panel) == {}
    project.undo_stack.undo()
    assert tree(panel) == {WORLD: None, "e000001": WORLD, "e000002": WORLD, "e000003": WORLD,
                           PLAYER: WORLD, SKY: WORLD}
    assert project.entity(SKY).components["sky"]["color"] == [0.08, 0.11, 0.18]


def test_one_sky_per_scene(qtbot, project):
    panel = ScenePanel()
    qtbot.addWidget(panel)
    panel.set_project(project)
    panel._update_add_menu()
    assert not panel.add_sky_action.isEnabled()
    panel.select(SKY)
    panel.delete_selected()
    panel._update_add_menu()
    assert panel.add_sky_action.isEnabled()
    panel.add_sky()
    assert project.entity(panel.selected_id()).components["sky"]["color"] == [0.08, 0.11, 0.18]


def test_the_sky_and_folders_have_their_own_forms(qtbot, project):
    sky = EntityForm(project, SKY)
    folder = EntityForm(project, WORLD)
    for form in (sky, folder):
        qtbot.addWidget(form)
        assert form.position is None and form.mesh is None  # no transform, no components
    assert set(sky.sky) == {"color", "ambient"} and folder.sky == {}
    sky.set_sky_color("color", [0.5, 0.25, 0.0])
    assert project.entity(SKY).components["sky"] == {"color": [0.5, 0.25, 0.0],
                                                    "ambient": [0.18, 0.2, 0.26]}
    assert sky.sky["color"].text() == "#804000"
    assert project.undo_stack.undoText() == "Change Sky sky colour"
    project.undo_stack.undo()
    assert sky.sky["color"].text() == "#141c2e"


def test_entity_form_edits_are_undoable(qtbot, project):
    form = EntityForm(project, "e000004")  # the Cube
    qtbot.addWidget(form)
    form.position.boxes[1].setValue(2.0)
    assert project.entity("e000004").transform()["position"] == [0.0, 2.0, 0.0]
    form.set_color([1.0, 0.0, 0.0])
    assert project.entity("e000004").components["material"]["color"] == [1.0, 0.0, 0.0, 1.0]
    form.name.setText("Red Cube")
    form.name.editingFinished.emit()
    assert project.entity("e000004").name == "Red Cube"
    project.undo_stack.undo()
    project.undo_stack.undo()
    project.undo_stack.undo()
    entity = project.entity("e000004")
    assert entity.name == "Player"
    assert entity.transform()["position"] == [0.0, 0.5, 0.0]
    assert form.position.boxes[1].value() == 0.5  # the form followed the undo


def test_camera_and_light_settings_appear_only_where_they_apply(qtbot, project):
    camera = EntityForm(project, "e000001")
    sun = EntityForm(project, "e000002")
    cube = EntityForm(project, "e000004")
    for w in (camera, sun, cube):
        qtbot.addWidget(w)
    assert camera.fov is not None and camera.color is None
    assert sun.intensity is not None and sun.fov is None
    assert cube.color is not None and cube.intensity is None
    camera.fov.setValue(70.0)
    assert project.entity("e000001").components["camera"]["fov"] == 70.0


def test_main_window_selection_shows_the_entity(window, project):
    window.open_project(project.config_path)
    window.scene_panel.select("e000004")
    assert window.inspector.entity_form is not None
    assert window.inspector.entity_form.entity_id == "e000004"
    window.scene_panel.select(SKY)
    assert window.inspector.entity_form.sky  # the sky's colours
    window.scene_panel.tree.setCurrentItem(None)
    assert window.inspector.entity_form is None


def test_shapes_and_the_mesh_picker(qtbot, project):
    panel = ScenePanel()
    qtbot.addWidget(panel)
    panel.set_project(project)
    panel.add_primitive("prim_sphere")
    sphere = project.entity(panel.selected_id())
    assert sphere.name == "Sphere" and sphere.components["mesh"]["asset"] == "prim_sphere"

    (project.root / "objects" / "rocks").mkdir(parents=True)
    (project.root / "objects" / "rocks" / "boulder.glb").write_bytes(b"glb")
    assert project.mesh_assets()["asset:rocks/boulder.glb"] == "rocks/boulder.glb"
    form = EntityForm(project, sphere.id)
    qtbot.addWidget(form)
    assert form.mesh.currentData() == "prim_sphere"
    form.mesh.setCurrentIndex(form.mesh.findData("asset:rocks/boulder.glb"))
    assert project.entity(sphere.id).components["mesh"]["asset"] == "asset:rocks/boulder.glb"
    project.undo_stack.undo()
    assert form.mesh.currentData() == "prim_sphere"
