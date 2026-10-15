# SPDX-License-Identifier: Apache-2.0
from midgard_editor.docks.entity_form import EntityForm
from midgard_editor.docks.scene_panel import ScenePanel
from midgard_editor.project import Project
from midgard_editor.scene_model import MAIN_SCENE


def test_new_projects_have_the_default_scene(project):
    names = [e.name for e in project.entities()]
    assert names == ["Camera", "Sun", "Ground", "Cube"]
    assert project.next_entity_id() == "e000005"


def test_projects_without_a_scene_get_one_when_opened(project):
    with project.connect() as con:
        con.execute("DELETE FROM entity")
        con.execute("DELETE FROM scene")
    reopened = Project.open(project.config_path)
    assert len(reopened.entities(MAIN_SCENE)) == 4


def test_scene_panel_adds_and_deletes_with_undo(qtbot, project):
    panel = ScenePanel()
    qtbot.addWidget(panel)
    panel.set_project(project)
    assert panel.list.count() == 4
    panel.add_cube_entity()
    assert panel.list.count() == 5
    assert panel.selected_id() == "e000005"
    assert project.entity("e000005").name == "Cube 2"
    panel.delete_selected()
    assert project.entity("e000005") is None
    project.undo_stack.undo()  # undo the delete
    assert project.entity("e000005") is not None
    project.undo_stack.undo()  # undo the add
    assert panel.list.count() == 4


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
    assert entity.name == "Cube"
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
    window.scene_panel.list.setCurrentRow(-1)
    assert window.inspector.entity_form is None
