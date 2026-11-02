# SPDX-License-Identifier: Apache-2.0
"""Characters (milestone 5.13): a .gltf and its .bin packed into one .glb; skinned models' joints
and clips read through the engine; the Inspector's Character form (the model, where its clips
come from, the clip playing), undoable; the cook packing a character's files; and the viewport
drawing characters. Uses the CC0 Quaternius sample (assets/third_party/characters/quaternius)."""
import json
import shutil
import struct
from pathlib import Path

import pytest

from midgard_editor import engine, gltf_pack
from midgard_editor.cook import cook
from midgard_editor.scene_model import Entity

needs_engine = pytest.mark.skipif(engine.module() is None, reason="engine module not built")
SAMPLES = Path(__file__).resolve().parents[2] / "assets" / "third_party" / "characters" / \
    "quaternius"
HERO = "asset:characters/superhero_male.glb"
LIBRARY = "asset:characters/animation_library.glb"


def with_samples(project) -> None:
    folder = project.objects_dir / "characters"
    folder.mkdir(parents=True, exist_ok=True)
    shutil.copy(SAMPLES / "superhero_male.glb", folder / "superhero_male.glb")
    shutil.copy(SAMPLES / "animation_library.glb", folder / "animation_library.glb")


def chain_gltf(folder: Path) -> Path:
    """A two-joint skinned triangle as a .gltf with its buffer in a .bin beside it (and an image
    by file, which packing leaves out)."""
    positions = struct.pack("<9f", 0, 0, 0, 0.1, 1, 0, 0, 1, 0)
    joints = struct.pack("<12H", 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0)
    weights = struct.pack("<12f", 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0)
    (folder / "chain.bin").write_bytes(positions + joints + weights)
    doc = {
        "asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0, 2]}],
        "nodes": [{"name": "root", "children": [1]},
                  {"name": "tip", "translation": [0, 1, 0]},
                  {"name": "Body", "mesh": 0, "skin": 0}],
        "skins": [{"joints": [0, 1]}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "JOINTS_0": 1,
                                                    "WEIGHTS_0": 2}, "material": 0}]}],
        "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}],
        "textures": [{"source": 0}], "images": [{"uri": "skin.png"}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
             "min": [0, 0, 0], "max": [0.1, 1, 0]},
            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "VEC4"},
            {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC4"}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                        {"buffer": 0, "byteOffset": 36, "byteLength": 24},
                        {"buffer": 0, "byteOffset": 60, "byteLength": 48}],
        "buffers": [{"uri": "chain.bin", "byteLength": 108}]}
    path = folder / "chain.gltf"
    path.write_text(json.dumps(doc), "utf-8")
    return path


def test_a_gltf_and_its_bin_pack_into_one_glb(tmp_path):
    glb = gltf_pack.pack(chain_gltf(tmp_path))
    assert glb[:4] == b"glTF"
    length = struct.unpack_from("<I", glb, 12)[0]
    doc = json.loads(glb[20:20 + length])
    assert "images" not in doc and "textures" not in doc  # (images by file: left out)
    assert "baseColorTexture" not in doc["materials"][0]["pbrMetallicRoughness"]
    assert doc["buffers"] == [{"byteLength": 108}]
    (tmp_path / "chain.bin").unlink()
    with pytest.raises(gltf_pack.PackError):
        gltf_pack.pack(tmp_path / "chain.gltf")


@needs_engine
def test_the_engine_reads_skinned_models_and_their_clips(tmp_path):
    midgard = engine.module()
    hero = midgard.skinned_info((SAMPLES / "superhero_male.glb").read_bytes())
    library = midgard.skinned_info((SAMPLES / "animation_library.glb").read_bytes())
    assert len(hero["joints"]) == 65 and hero["joints"] == library["joints"]
    assert hero["clips"] == []  # (its clips come from the library)
    names = [c["name"] for c in library["clips"]]
    assert len(names) == 43 and {"Idle_Loop", "Walk_Loop", "Sprint_Loop"} <= set(names)
    assert next(c for c in library["clips"] if c["name"] == "Walk_Loop")["duration"] > 0.5
    packed = midgard.skinned_info(gltf_pack.pack(chain_gltf(tmp_path)))
    assert packed["joints"] == ["root", "tip"] and packed["vertices"] == 3
    with pytest.raises(ValueError):
        midgard.skinned_info(b"not glTF")


@needs_engine
def test_the_inspector_sets_a_character_up(project, qtbot):
    from midgard_editor.docks.entity_form import EntityForm
    with_samples(project)
    chain_gltf(project.objects_dir)  # (written into objects/)
    assert set(project.character_assets()) == {HERO, LIBRARY, "asset:chain.gltf"}
    entity = Entity(project.next_entity_id(), "Hero", None, {"transform": {}})
    project.put_entity(entity)
    form = EntityForm(project, entity.id)
    qtbot.addWidget(form)
    form.add("character")
    form = EntityForm(project, entity.id)  # (rebuilt with the new component, as the Inspector)
    qtbot.addWidget(form)
    form.character_model.setCurrentIndex(form.character_model.findData(HERO))
    assert project.entity(entity.id).components["character"]["model"] == HERO
    assert form.character_clip.count() == 1  # the hero has no clips of its own
    form.character_clips.setCurrentIndex(form.character_clips.findData(LIBRARY))
    character = project.entity(entity.id).components["character"]
    assert character["clips"] == LIBRARY and character["clip"] == "A_TPose"  # (the first)
    assert form.character_clip.findData("Walk_Loop") >= 0
    form.character_clip.setCurrentIndex(form.character_clip.findData("Walk_Loop"))
    form.character_speed.setValue(1.5)
    character = project.entity(entity.id).components["character"]
    assert (character["clip"], character["speed"]) == ("Walk_Loop", 1.5)
    project.undo_stack.undo()
    project.undo_stack.undo()
    assert project.entity(entity.id).components["character"]["clip"] == "A_TPose"
    assert form.character_clip.currentData() == "A_TPose"  # (the form follows)


@needs_engine
def test_the_cook_packs_a_characters_files(project, tmp_path):
    with_samples(project)
    chain_gltf(project.objects_dir)  # (written into objects/)
    for model, clips in ((HERO, LIBRARY), ("asset:chain.gltf", "")):
        project.put_entity(Entity(project.next_entity_id(), "Character", None, {
            "transform": {}, "character": {"model": model, "clips": clips, "clip": ""}}))
    result = cook(project, tmp_path / "pkg")
    content = {c["id"]: c["path"] for c in result.manifest["content"]}
    assert {HERO, LIBRARY, "asset:chain.gltf"} <= set(content)
    assert content["asset:chain.gltf"].endswith(".glb")  # (packed)
    assert not [w for w in result.warnings if "character" in w.lower()], result.warnings


@needs_engine
def test_the_viewport_draws_characters():
    midgard = engine.module()
    view = midgard.Viewport()
    try:
        view.attach_offscreen(160, 160)
    except RuntimeError as e:
        pytest.skip(f"no Vulkan here: {e}")
    view.set_settings(json.dumps({"sky": [0, 0, 0.3]}))
    view.put_entity(json.dumps({"id": "sun", "name": "Sun", "components": {
        "transform": {"rotation": [-40, 20, 0]}, "light": {"type": "directional"}}}))
    view.set_camera([0, 1.0, 3.0], [0, 0.9, 0], 45)

    def lit(rgba: bytes) -> int:  # pixels brighter than the dark blue sky
        return sum(1 for i in range(0, len(rgba), 4) if rgba[i] > 60)

    assert view.render()
    empty = lit(view.read_pixels())
    view.set_mesh(HERO, (SAMPLES / "superhero_male.glb").read_bytes())
    view.set_mesh(LIBRARY, (SAMPLES / "animation_library.glb").read_bytes())
    view.put_entity(json.dumps({"id": "hero", "name": "Hero", "components": {
        "transform": {}, "material": {"color": [0.9, 0.7, 0.5, 1]},
        "character": {"model": HERO, "clips": LIBRARY, "clip": "Idle_Loop"}}}))
    assert view.render()
    assert lit(view.read_pixels()) > empty + 500, "the character is drawn"
    view.detach()
