# SPDX-License-Identifier: Apache-2.0
"""A model used as a mesh brings its own material (milestone 5.13a): the Avocado from the Asset
Library, placed as a plain mesh, keeps its textures in the cooked game and in the editor's
viewport; what the entity's own material says wins."""
import json
import sqlite3
import subprocess

import pytest

from midgard_editor import asset_library as lib
from midgard_editor import engine, model_materials
from midgard_editor.cook import cook
from midgard_editor.scene_model import Entity

needs_engine = pytest.mark.skipif(engine.module() is None, reason="engine module not built")
AVOCADO = "lib:models/avocado/Avocado.gltf"


def with_avocado(project, material=None) -> Entity:
    lib.add_samples(project)
    components = {"transform": {"position": [0, 0, 0], "scale": [30, 30, 30]},
                  "mesh": {"asset": AVOCADO}}
    if material is not None:
        components["material"] = material
    entity = Entity(project.next_entity_id(), "Avocado", None, components)
    project.put_entity(entity)
    return entity


@needs_engine
def test_the_models_own_material_fills_in_under_the_entitys(project):
    entity = with_avocado(project)
    drawn = model_materials.effective(project, entity.components)["material"]
    assert set(drawn["textures"]) == {"base_color", "normal", "metallic_roughness"}
    assert drawn["textures"]["base_color"].startswith(f"mdl:{AVOCADO}#")
    assert len(model_materials.textures_of(project, entity.components)) == 3
    # The entity's own: its colour and its base colour texture win; the rest is the model's.
    own = {"color": [1, 0, 0, 1], "textures": {"base_color": "tex:textures/mine.png"}}
    drawn = model_materials.effective(project, {**entity.components, "material": own})
    assert drawn["material"]["color"] == [1, 0, 0, 1]
    assert drawn["material"]["textures"]["base_color"] == "tex:textures/mine.png"
    assert drawn["material"]["textures"]["normal"].startswith("mdl:")
    assert len(model_materials.textures_of(project, {**entity.components,
                                                     "material": own})) == 2
    # What the running game is sent (the live link) is the same.
    from midgard_editor.live_link import entity_message
    sent = entity_message(project, entity.id)["entity"]["components"]["material"]
    assert set(sent["textures"]) == {"base_color", "normal", "metallic_roughness"}
    # Not a model: unchanged.
    cube = {"mesh": {"asset": "prim_cube"}}
    assert model_materials.effective(project, cube) is cube


@needs_engine
def test_the_cooked_game_draws_the_model_with_its_textures(project, tmp_path):
    from midgard_editor.about import runtime_candidates
    entity = with_avocado(project)
    for e in project.entities():
        if e.name in ("Ground", "Player"):
            project.delete_entity(e.id)
        if "camera" in e.components:  # close up, looking at it
            e.components["transform"] = {"position": [0, 1.0, 2.2], "rotation": [-20, 0, 0]}
            project.put_entity(e)
    result = cook(project, tmp_path / "pkg")
    content = {c["id"]: c["path"] for c in result.manifest["content"]}
    model_textures = [t for t in content if t.startswith("mdl:")]
    assert len(model_textures) == 3
    assert all(content[t].startswith("images/models/") for t in model_textures)
    db = sqlite3.connect(result.out_dir / f"{project.name}.db")
    (text,) = db.execute("SELECT components FROM entity WHERE id = ?", (entity.id,)).fetchone()
    db.close()
    assert json.loads(text)["material"]["textures"]["base_color"] in model_textures
    assert "material" not in project.entity(entity.id).components  # (the project's unchanged)
    runtime = next((r for r in runtime_candidates() if r.is_file()), None)
    if runtime is None:
        pytest.skip("no runtime built")
    shot = tmp_path / "shot.png"
    run = subprocess.run([str(runtime), "--package", str(result.out_dir), "--screenshot",
                          str(shot), "--size", "320x180", "--frames", "5"],
                         capture_output=True, text=True, timeout=60)
    if run.returncode != 0 and "Vulkan" in (run.stderr + run.stdout):
        pytest.skip("no Vulkan: " + run.stderr.strip())
    assert run.returncode == 0, run.stderr
    from PIL import Image
    pixels = Image.open(shot).convert("RGB").tobytes()
    # The avocado's skin: dark green and brown (green and red over blue), not white.
    coloured = sum(1 for i in range(0, len(pixels), 3)
                   if pixels[i + 1] > pixels[i + 2] + 15 and pixels[i] < 200)
    white = sum(1 for i in range(0, len(pixels), 3) if min(pixels[i:i + 3]) > 200)
    assert coloured > 2000 and white < 200, (coloured, white)


@needs_engine
def test_the_viewport_shows_the_model_with_its_material(project, qtbot):
    from test_object_editor import RecordingView

    from midgard_editor.docks.viewport import ViewportPanel
    panel = ViewportPanel()
    qtbot.addWidget(panel)
    view = panel.view = RecordingView()
    panel.set_project(project)
    with_avocado(project)
    textures = [c[1] for c in view.calls if c[0] == "set_texture"]
    assert len(textures) == 3 and all(t.startswith(f"mdl:{AVOCADO}#") for t in textures)
    put = [json.loads(c[1]) for c in view.calls if c[0] == "put_entity"]
    avocado = next(p for p in put if p["name"] == "Avocado")
    assert set(avocado["components"]["material"]["textures"].values()) == set(textures)
