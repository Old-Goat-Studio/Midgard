# SPDX-License-Identifier: Apache-2.0
"""Footsteps (milestone 5.14 step 7): steps sound like what's underfoot: an object's surface, set
on its physics material, or the terrain's ground; set up in the Inspector."""
from test_music import strength
from test_scripting import needs_scripting, simulate, write_script
from test_sound_events import a_library_sound
from test_sounds import needs_encoder

from midgard_editor import sound_events
from midgard_editor.cook import cook
from midgard_editor.docks.entity_form import EntityForm
from midgard_editor.scene_model import Entity


def a_step(project, tmp_path, name, hz):
    file = a_library_sound(project, tmp_path, "effects", name, hz, 0.1, category="steps")
    event = sound_events.event_for_sound(project, file)
    sound_events.put(project, event)
    return event["id"]


def test_the_inspector_sets_up_footsteps_and_surfaces(qtbot, project, tmp_path):
    wood = a_step(project, tmp_path, "creak", 400.0)
    project.put_entity(Entity("e000080", "Walker", None, {"transform": {}}))
    form = EntityForm(project, "e000080")
    qtbot.addWidget(form)
    form.add("footsteps")
    assert project.entity("e000080").components["footsteps"] == {
        "event": "step", "every": 0.0, "sounds": {}}
    form = EntityForm(project, "e000080")
    qtbot.addWidget(form)
    assert set(form.footsteps_sounds) == {"grass", "sand", "rock", "snow", "water", "wood",
                                          "stone", "metal", "default"}
    form.footsteps_sounds["wood"].setCurrentIndex(form.footsteps_sounds["wood"].findData(wood))
    form.footsteps_every.setValue(0.6)
    steps = project.entity("e000080").components["footsteps"]
    assert steps["sounds"] == {"wood": wood} and steps["every"] == 0.6
    form.footsteps_sounds["wood"].setCurrentIndex(0)  # back to "as anything else"
    assert project.entity("e000080").components["footsteps"]["sounds"] == {}
    # An object's surface, on its physics material.
    project.put_entity(Entity("e000081", "Bridge", None, {
        "transform": {}, "collider": {}, "physics_material": {"friction": 0.5}}))
    bridge = EntityForm(project, "e000081")
    qtbot.addWidget(bridge)
    bridge.surface.setCurrentIndex(bridge.surface.findData("wood"))
    assert project.entity("e000081").components["physics_material"]["surface"] == "wood"
    project.undo_stack.undo()
    assert "surface" not in project.entity("e000081").components["physics_material"]


WALKER = """
import midgard

def on_update(entity, dt):
    entity.move(2.0 * dt, 0, 0)   # 2 m/s along +x, over the wood, then the stone
"""


@needs_scripting
@needs_encoder
def test_steps_sound_like_what_is_underfoot(project, tmp_path):
    wood = a_step(project, tmp_path, "creak", 400.0)
    stone = a_step(project, tmp_path, "clack", 700.0)
    for eid, name, x, surface in (("e000082", "Boards", 0.0, "wood"),
                                  ("e000083", "Flagstones", 4.0, "stone")):
        project.put_entity(Entity(eid, name, None, {
            "transform": {"position": [x, 0.5, 20.0], "scale": [4.0, 1.0, 4.0]},
            "collider": {"shape": "box", "size": [1.0, 1.0, 1.0]},
            "physics_material": {"surface": surface}}))
    project.put_entity(Entity("e000084", "Walker", None, {
        "transform": {"position": [-1.5, 1.05, 20.0]},
        "footsteps": {"every": 0.5, "sounds": {"wood": wood, "stone": stone}},
        "script": {"module": "walker"}}))
    write_script(project, "walker", WALKER)
    package = cook(project, tmp_path / "pkg").out_dir
    recording = tmp_path / "heard.wav"
    dumped, run = simulate(package, 180, "--record-audio", str(recording))
    assert run.returncode == 0, run.stderr
    steps = [s for s in dumped["audio"]["footsteps"] if s["entity"] == "e000084"]
    surfaces = [s["surface"] for s in steps]
    # Three seconds at 2 m/s, a step each half metre: about twelve, wood then stone.
    assert 10 <= len(steps) <= 12
    assert surfaces[:3] == ["wood"] * 3 and surfaces[-3:] == ["stone"] * 3
    assert surfaces == sorted(surfaces, key=lambda s: s != "wood")  # (never back to wood)
    assert all(s["sound"] == (wood if s["surface"] == "wood" else stone) for s in steps)
    # Heard: creaks first, clacks after.
    assert strength(recording, 400.0, 0.2, 1.0) > 5 * strength(recording, 700.0, 0.2, 1.0)
    assert strength(recording, 700.0, 2.2, 3.0) > 5 * strength(recording, 400.0, 2.2, 3.0)


@needs_scripting
@needs_encoder
def test_a_characters_walk_cycle_steps_and_its_events_play_sounds(project, tmp_path):
    """The hero's walk cycle passes "step" twice a cycle: footsteps on each, on whatever the
    ground is ("default" here), and the character's own map plays a sound for it too."""
    from test_anim_graphs import HERO, LIBRARY, WALKING, with_samples
    from test_anim_graphs import WALKER as DRIVER
    step = a_step(project, tmp_path, "tap", 500.0)
    swish = a_step(project, tmp_path, "swish", 900.0)
    with_samples(project)
    project.put_entity(Entity("e000085", "Walker", None, {
        "transform": {"position": [3.0, 0.0, 0.0]},
        "character": {"model": HERO, "clips": LIBRARY, "graph": WALKING,
                      "sounds": {"step": swish}},
        "footsteps": {"sounds": {"default": step}},
        "script": {"module": "walker"}}))
    write_script(project, "walker", DRIVER)
    package = cook(project, tmp_path / "pkg").out_dir
    recording = tmp_path / "heard.wav"
    dumped, run = simulate(package, 120, "--record-audio", str(recording))
    assert run.returncode == 0, run.stderr
    steps = [s for s in dumped["audio"]["footsteps"] if s["entity"] == "e000085"]
    assert len(steps) >= 2 and all(s["sound"] == step for s in steps)
    assert "event step" in run.stdout  # (the graph passed them, as scripts hear)
    # Heard: short taps (and swishes), so the loudest tenth of a second of each.
    def loudest(hz):
        return max(strength(recording, hz, t / 20, t / 20 + 0.1) for t in range(39))

    assert loudest(500.0) > 0.01 and loudest(900.0) > 0.01
