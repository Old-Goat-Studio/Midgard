# SPDX-License-Identifier: Apache-2.0
"""Ambience (milestone 5.14 step 6): sets kept in the project, and in the game layers that fade
in and out as day turns to night, with sprinkles around the listener."""
from test_music import NIGHTFALL, strength
from test_scripting import needs_scripting, simulate, write_script
from test_sound_events import a_library_sound
from test_sounds import needs_encoder

from midgard_editor import ambience, sound_events
from midgard_editor.cook import cook
from midgard_editor.scene_model import Entity


def a_loop(project, tmp_path, name, hz):
    file = a_library_sound(project, tmp_path, "ambiance", name, hz, 1.0, category="nature")
    event = sound_events.event_for_sound(project, file)
    event["event"]["spatial"] = False  # (all around)
    sound_events.put(project, event)
    return event["id"]


def test_ambience_sets_are_kept_in_the_project_with_undo(project):
    assert ambience.load_all(project) == []
    forest = {"id": ambience.new_id(project, "Forest"), "name": "Forest",
              "ambience": {"layers": [{"event": "sfx_birds", "times": ["day"]}],
                           "sprinkles": [{"event": "sfx_owl", "times": ["teatime"]}],
                           "biomes": ["Forest"]}}
    assert forest["id"] == "amb_forest"
    project.undo_stack.push(ambience.AmbienceCommand(project, None, forest, ""))
    assert ambience.load(project, "amb_forest")["ambience"]["biomes"] == ["Forest"]
    found = ambience.problems(project, [{"id": "sfx_birds", "name": "Birds"}])
    assert any("sfx_owl" in p for p in found) and any("teatime" in p for p in found)
    assert not any("sfx_birds" in p for p in found)
    project.undo_stack.undo()
    assert ambience.load_all(project) == []


@needs_scripting
@needs_encoder
def test_the_ambience_changes_with_the_day_in_the_game(project, tmp_path):
    birds = a_loop(project, tmp_path, "birds", 220.0)
    crickets = a_loop(project, tmp_path, "crickets", 330.0)
    owl = a_library_sound(project, tmp_path, "effects", "owl", 880.0, 0.2)
    owl_event = sound_events.event_for_sound(project, owl)
    sound_events.put(project, owl_event)
    ambience.put(project, {"id": "amb_meadow", "name": "Meadow", "ambience": {
        "layers": [{"event": birds, "times": ["dawn", "day"]},
                   {"event": crickets, "times": ["dusk", "night"]}],
        "sprinkles": [{"event": owl_event["id"], "every": [0.5, 1.0], "distance": [4, 8]}],
        "crossfade": 1.0}})
    project.put_entity(Entity("e000072", "Clock", None, {
        "transform": {}, "script": {"module": "nightfall"}}))
    write_script(project, "nightfall", NIGHTFALL)
    package = cook(project, tmp_path / "pkg").out_dir
    first, _ = simulate(package, 1)
    layers = first["audio"]["ambience"]["layers"]
    assert first["audio"]["ambience"]["set"] == "amb_meadow" and len(layers) == 1
    starting = layers[0]
    later = crickets if starting == birds else birds
    tones = {birds: 220.0, crickets: 330.0}
    recording = tmp_path / "heard.wav"
    dumped, run = simulate(package, 300, "--record-audio", str(recording))
    assert run.returncode == 0, run.stderr
    assert dumped["audio"]["ambience"]["layers"] == [later]
    # (It fades in over its first second; the clock moves at 1 s, and as the set is the same
    # the layers crossfade at once, over a second.)
    assert strength(recording, tones[starting], 0.75, 0.98) > 0.25
    assert strength(recording, tones[later], 0.75, 0.98) < 0.01
    assert strength(recording, tones[later], 2.5, 4.5) > 0.25
    assert strength(recording, tones[starting], 2.5, 4.5) < 0.01
    # The owl now and then (any time of day): five seconds, every half to one second.
    assert 5 <= dumped["audio"]["ambience"]["sprinkled"] <= 10
