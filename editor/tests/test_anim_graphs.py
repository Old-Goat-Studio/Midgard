# SPDX-License-Identifier: Apache-2.0
"""Animation graphs in the game (milestone 5.13 step 4): a character's root motion moving it
along its own facing; scripts setting a graph's parameters, reading its state and hearing its
events; its "speed" following how it moves, through a blend of the Quaternius clips."""
import json
import struct
from pathlib import Path

import pytest
from test_characters import HERO, LIBRARY, with_samples
from test_scripting import by_name, needs_scripting, simulate, write_script

from midgard_editor import engine
from midgard_editor.cook import cook
from midgard_editor.scene_model import Entity

needs_engine = pytest.mark.skipif(engine.module() is None, reason="engine module not built")


def sliding_gltf(folder: Path) -> str:
    """A two-joint skinned triangle whose one clip, "Slide", moves its root 2 m along +Z each
    second (a .gltf and its .bin in the project's objects/ folder)."""
    positions = struct.pack("<9f", 0, 0, 0, 0.1, 1, 0, 0, 1, 0)
    joints = struct.pack("<12H", 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0)
    weights = struct.pack("<12f", 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0)
    times = struct.pack("<2f", 0, 1)
    moves = struct.pack("<6f", 0, 0, 0, 0, 0, 2)
    (folder / "slider.bin").write_bytes(positions + joints + weights + times + moves)
    doc = {
        "asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0, 2]}],
        "nodes": [{"name": "root", "children": [1]},
                  {"name": "tip", "translation": [0, 1, 0]},
                  {"name": "Body", "mesh": 0, "skin": 0}],
        "skins": [{"joints": [0, 1]}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "JOINTS_0": 1,
                                                    "WEIGHTS_0": 2}}]}],
        "animations": [{"name": "Slide",
                        "channels": [{"sampler": 0, "target": {"node": 0,
                                                               "path": "translation"}}],
                        "samplers": [{"input": 3, "output": 4}]}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
             "min": [0, 0, 0], "max": [0.1, 1, 0]},
            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "VEC4"},
            {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC4"},
            {"bufferView": 3, "componentType": 5126, "count": 2, "type": "SCALAR",
             "min": [0], "max": [1]},
            {"bufferView": 4, "componentType": 5126, "count": 2, "type": "VEC3"}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                        {"buffer": 0, "byteOffset": 36, "byteLength": 24},
                        {"buffer": 0, "byteOffset": 60, "byteLength": 48},
                        {"buffer": 0, "byteOffset": 108, "byteLength": 8},
                        {"buffer": 0, "byteOffset": 116, "byteLength": 24}],
        "buffers": [{"uri": "slider.bin", "byteLength": 140}]}
    (folder / "slider.gltf").write_text(json.dumps(doc), "utf-8")
    return "asset:slider.gltf"


SLIDE = {"root_motion": True,
         "layers": [{"name": "base", "states": {"slide": {"clip": "Slide"}}}]}


@needs_engine
def test_root_motion_moves_a_character_along_its_facing(project, tmp_path):
    model = sliding_gltf(project.objects_dir)
    for name, x, yaw in (("Ahead", 0.0, 0.0), ("Turned", 10.0, 90.0)):
        project.put_entity(Entity(project.next_entity_id(), name, None, {
            "transform": {"position": [x, 0.0, 0.0], "rotation": [0.0, yaw, 0.0]},
            "character": {"model": model, "graph": SLIDE}}))
    package = cook(project, tmp_path / "pkg").out_dir
    dumped, run = simulate(package, 60)  # a second
    assert run.returncode == 0, run.stderr
    ahead, turned = by_name(dumped, "Ahead"), by_name(dumped, "Turned")
    assert ahead["position"] == pytest.approx([0.0, 0.0, 2.0], abs=0.05)
    assert turned["position"] == pytest.approx([12.0, 0.0, 0.0], abs=0.05)  # (facing +X)


WALKER = """
import midgard

seen = set()

def once(*words):
    if words not in seen:
        seen.add(words)
        midgard.log(*words)

def on_start(entity):
    entity.set_animation("waving", True)
    try:
        entity.set_animation("flying", 1)
    except ValueError as e:
        once("refused:", str(e))

def on_update(entity, dt):
    if midgard.time() > 0.5:
        entity.move(0, 0, 1.4 * dt)  # walking pace
    once("state", entity.animation_state(), "waving", entity.animation("waving"))
    if entity.animation("speed") > 1.3:
        once("walking")

def on_animation_event(entity, name):
    once("event", name)
"""

WALKING = {
    "parameters": {"waving": False},
    "layers": [
        {"name": "base", "start": "idle",
         "states": {
             "idle": {"clip": "Idle_Loop"},
             "move": {"blend": "speed", "clips": [["Idle_Loop", 0], ["Walk_Loop", 1.4],
                                                  ["Sprint_Loop", 5.0]],
                      "events": [[0.0, "step"], [0.5, "step"]]}},
         "transitions": [
             {"from": "idle", "to": "move", "when": [["speed", ">", 0.2]], "blend": 0.2},
             {"from": "move", "to": "idle", "when": [["speed", "<", 0.1]], "blend": 0.3}]},
        {"name": "wave", "mask": "spine_02", "weight": "waving",
         "states": {"wave": {"clip": "Spell_Simple_Idle_Loop"}}}]}


@needs_scripting
def test_scripts_drive_a_characters_graph(project, tmp_path):
    with_samples(project)
    project.put_entity(Entity(project.next_entity_id(), "Walker", None, {
        "transform": {"position": [3.0, 0.0, 0.0]},
        "character": {"model": HERO, "clips": LIBRARY, "graph": WALKING},
        "script": {"module": "walker"}}))
    write_script(project, "walker", WALKER)
    package = cook(project, tmp_path / "pkg").out_dir
    dumped, run = simulate(package, 120)
    assert run.returncode == 0, run.stderr
    log = run.stdout
    assert "refused: the animation graph has no parameter 'flying'" in log
    assert "state idle waving 1.0" in log  # (set in on_start, before the graph began)
    assert "walking" in log
    assert "state move waving 1.0" in log
    assert log.index("state idle") < log.index("state move")
    assert "event step" in log
    assert by_name(dumped, "Walker")["position"][2] > 0.8
