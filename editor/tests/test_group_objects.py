# SPDX-License-Identifier: Apache-2.0
import shutil
from pathlib import Path

import pytest
from test_objects import needs_import

from midgard_editor import about, transforms
from midgard_editor.cook import cook
from midgard_editor.docks.material_editor import MaterialEditor, ObjectMaterial
from midgard_editor.docks.scene_panel import ScenePanel
from midgard_editor.group_objects import save_group_as_object, template_from_group
from midgard_editor.live_link import PlaySession, State
from midgard_editor.scene_model import Entity
from midgard_editor.watch import ContentWatcher

AVOCADO = Path(__file__).resolve().parents[2] / "assets" / "third_party" / "models" / "avocado"


def build_bench(project) -> None:
    """A bench group, turned and lifted, of a seat, a leg with a script, and a group inside it
    holding a cushion."""
    project.put_entity(Entity("e000050", "Bench", None, {
        "transform": {"position": [2.0, 1.0, 0.0], "rotation": [0.0, 90.0, 0.0]},
        "group": {}, "rigidbody": {"motion": "dynamic"}}))
    project.put_entity(Entity("e000051", "Seat", "e000050", {
        "transform": {"position": [0.0, 0.5, 0.0], "scale": [2.0, 0.2, 1.0]},
        "mesh": {"asset": "prim_cube"}, "collider": {}}))
    project.put_entity(Entity("e000052", "Leg", "e000050", {
        "transform": {"position": [0.8, 0.0, 0.0]}, "mesh": {"asset": "prim_cylinder"},
        "script": {"module": "creak"}}))
    project.put_entity(Entity("e000053", "Cushions", "e000050", {
        "transform": {"position": [0.0, 0.7, 0.0], "rotation": [0.0, 0.0, 30.0]},
        "group": {}}))
    project.put_entity(Entity("e000054", "Cushion", "e000053", {
        "transform": {"position": [0.3, 0.0, 0.0]}, "mesh": {"asset": "prim_sphere"}}))


def close(a, b) -> bool:
    # (The editor keeps transforms to 4 decimal places: a tenth of a millimetre.)
    return all(abs(x - y) < 1e-3 for ra, rb in zip(a, b, strict=True)
               for x, y in zip(ra, rb, strict=True))


def test_a_groups_contents_become_parts_placed_relative_to_it(project):
    build_bench(project)
    entities = project.entities()
    by_id = {e.id: e for e in entities}
    parts = template_from_group(entities, "e000050")
    assert [p["name"] for p in parts] == ["Seat", "Leg", "Cushion"]  # the inner group flattened
    assert parts[1]["components"]["script"] == {"module": "creak"}  # scripts and all come along
    assert all("group" not in p["components"] for p in parts)
    # Each part, placed by an instance where the group was, lands where it was in the world.
    group_world = transforms.world_matrix(by_id, "e000050")
    for part, entity_id in zip(parts, ("e000051", "e000052", "e000054"), strict=True):
        placed = transforms.multiply(group_world,
                                     transforms.compose(part["components"]["transform"]))
        assert close(placed, transforms.world_matrix(by_id, entity_id)), part["name"]


def test_saving_replaces_the_group_with_an_instance_in_one_undo_step(project):
    build_bench(project)
    before = {e.id: e for e in project.entities()}
    object_id = save_group_as_object(project, "e000050", "Park Bench")
    obj = project.object(object_id)
    assert (object_id, obj.name, obj.source) == ("obj_park_bench", "Park Bench", None)
    assert len(obj.parts) == 3
    bench = project.entity("e000050")
    assert bench.components["object"] == {"id": object_id}
    assert bench.components["rigidbody"] == {"motion": "dynamic"}  # its own components stay
    assert "group" not in bench.components
    assert bench.components["transform"] == before["e000050"].components["transform"]
    assert all(project.entity(i) is None for i in ("e000051", "e000052", "e000053", "e000054"))

    project.undo_stack.undo()
    assert project.object(object_id) is None
    assert {e.id: e for e in project.entities()} == before

    # Kept as a group: only the object is added.
    save_group_as_object(project, "e000050", replace=False)
    assert project.object("obj_bench") is not None
    assert "group" in project.entity("e000050").components


def test_the_scene_panel_offers_it_for_groups_only(qtbot, project):
    build_bench(project)
    panel = ScenePanel()
    qtbot.addWidget(panel)
    panel.set_project(project)

    def menu_for(entity_id):
        panel.select(entity_id)
        return [a.text() for a in panel.context_menu(panel.tree.currentItem()).actions()]

    assert "Save as Object…" in menu_for("e000050")
    assert "Save as Object…" not in menu_for("e000051")


@needs_import
@pytest.mark.skipif(not about.runtime_candidates(), reason="engine not built here")
def test_exit_check_a_cc0_model_placed_and_its_material_changed_while_playing(
        qtbot, project, tmp_path):
    """Milestone 5.5's exit: a CC0 model dropped into objects/, placed from Add ▸ Object, its
    material changed in the editor while the game runs, and the change seen in the game."""
    watcher = ContentWatcher(project)
    scene = ScenePanel()
    qtbot.addWidget(scene)
    scene.set_project(project)

    # Dropped into objects/: the editor notices, and Add ▸ Object offers it.
    shutil.copytree(AVOCADO, project.objects_dir / "avocado")
    assert "objects/avocado/Avocado.gltf" in watcher.check()
    scene.fill_object_menu()
    assert "avocado/Avocado.gltf (new)" in [a.text() for a in scene.object_menu.actions()]
    instance = scene.add_model("avocado/Avocado.gltf")
    obj = project.object(project.entity(instance).components["object"]["id"])
    # It's 6 cm tall: scaled up in the Object editor's import settings.
    project.put_object(type(obj)(obj.id, obj.name, obj.source, {"scale": 20.0}))

    received = []
    session = PlaySession(project)
    session.message_received.connect(received.append)
    try:
        session.start(about.runtime_candidates()[0], cook(project, tmp_path / "pkg").out_dir)
        qtbot.waitUntil(lambda: session.state == State.PLAYING, timeout=20000)

        def part_colour():
            session.send({"type": "scene.dump"})
            count = len(received)
            qtbot.waitUntil(lambda: any(m.get("type") == "scene" for m in received[count:]),
                            timeout=5000)
            dump = next(m for m in received[count:] if m.get("type") == "scene")["scene"]
            return next(e["color"] for e in dump["entities"] if e["id"] == f"{instance}/0")

        assert part_colour()[:3] == pytest.approx([1.0, 1.0, 1.0])  # the model's own
        # The Material editor, while the game runs: the avocado turns red in the game.
        editor = MaterialEditor(project, ObjectMaterial(project, obj.id, 0))
        qtbot.addWidget(editor)
        editor.set_color("color", [1.0, 0.0, 0.0])
        qtbot.waitUntil(lambda: any(m.get("text") == f"applied object.put {obj.id}"
                                    for m in received), timeout=10000)
        assert part_colour()[:3] == pytest.approx([1.0, 0.0, 0.0])
        assert not [m for m in received if m.get("level") in ("warn", "error")], received
    finally:
        session.stop()


@needs_import
def test_the_cc0_corpus_imports_at_any_scale_axis_and_with_files_missing(tmp_path):
    """The awkward cases: a tiny model, huge and tiny scales, Z up, a texture gone."""
    import json

    from midgard_editor import engine

    module = engine.module()
    model = AVOCADO / "Avocado.gltf"

    def bounds(settings) -> list[float]:
        b = module.import_model(model, json.dumps(settings))["bounds"]
        return [hi - lo for lo, hi in zip(b[:3], b[3:], strict=True)]

    size = bounds({})
    assert 0.05 < size[1] < 0.07  # the avocado is about 6 cm tall
    assert bounds({"scale": 20.0})[1] == pytest.approx(size[1] * 20.0, rel=1e-4)
    assert bounds({"scale": 1000.0})[1] == pytest.approx(size[1] * 1000.0, rel=1e-4)
    assert bounds({"scale": 0.001})[1] == pytest.approx(size[1] * 0.001, rel=1e-3)
    z_up = bounds({"z_up": True})  # its height is now along Z, as a Z-up model would have it
    assert z_up[1] == pytest.approx(size[2], rel=1e-4) and z_up[2] == pytest.approx(size[1],
                                                                                    rel=1e-4)
    imported = module.import_model(model, "{}")
    assert len(imported["textures"]) == 3 and not imported["warnings"]
    assert imported["materials"][0]["textures"].keys() == {"base_color", "metallic_roughness",
                                                           "normal"}

    # A texture missing: imported without it, and a warning says which.
    broken = tmp_path / "avocado"
    shutil.copytree(AVOCADO, broken)
    (broken / "Avocado_normal.png").unlink()
    imported = module.import_model(broken / "Avocado.gltf", "{}")
    assert len(imported["textures"]) == 2
    assert any("Avocado_normal.png" in w and "missing" in w for w in imported["warnings"])
