# SPDX-License-Identifier: Apache-2.0
"""Sound in 3D (milestone 5.14 step 4), in the game: a placed sound's level by its distance from
the listener (the camera), muffled behind a wall, and reverb zones' echo; and the editor's
reverb zones."""
import math

import pytest
from test_scripting import needs_scripting, simulate
from test_sound_events import a_library_sound, levels
from test_sounds import needs_encoder

from midgard_editor import sound_events
from midgard_editor.cook import cook
from midgard_editor.docks.entity_form import EntityForm
from midgard_editor.docks.scene_panel import ScenePanel
from midgard_editor.scene_model import Entity


def distance_gain(d: float, nearest: float = 1.0, farthest: float = 40.0) -> float:
    """The engine's curve (midgard/audio.h)."""
    if d <= nearest:
        return 1.0
    if d >= farthest:
        return 0.0
    gain = nearest / d
    fade = nearest + 0.8 * (farthest - nearest)
    return gain * ((farthest - d) / (farthest - fade) if d > fade else 1.0)


def a_hum(project, tmp_path):
    """A placed, looping 300 Hz hum (heard from 1 m, silent by 40 m)."""
    file = a_library_sound(project, tmp_path, "effects", "hum", 300.0, 1.0)
    event = sound_events.event_for_sound(project, file)
    event["event"]["loop"] = True
    sound_events.put(project, event)
    return event["id"]


def run(project, tmp_path, name, frames=60):
    package = cook(project, tmp_path / name).out_dir
    recording = tmp_path / f"{name}.wav"
    dumped, result = simulate(package, frames, "--record-audio", str(recording))
    assert result.returncode == 0, result.stderr
    return dumped, recording


@needs_scripting
@needs_encoder
def test_a_placed_sound_fades_with_distance_and_is_muffled_behind_a_wall(project, tmp_path):
    hum = a_hum(project, tmp_path)
    first, _ = run(project, tmp_path, "listen")
    ear = first["audio"]["listener"]
    # A hum 4 m from the listener (on the ground's side, away from the player).
    at = [ear[0] + 4.0, ear[1], ear[2]]
    project.put_entity(Entity("e000060", "Generator", None, {
        "transform": {"position": at}, "sound": {"event": hum}}))
    clear, heard = run(project, tmp_path, "clear")
    voice = next(v for v in clear["audio"]["voices"] if v["event"] == hum)
    distance = math.dist(clear["audio"]["listener"], at)
    assert voice["spatial"] and voice["gain"] == pytest.approx(distance_gain(distance), rel=0.01)
    assert voice["occlusion"] < 0.01
    # Heard on the right (it's to the listener's +X; the camera looks along -Z).
    with_right = levels(heard, 0.5, 0.9, channel=1)
    assert with_right > 1.5 * levels(heard, 0.5, 0.9)
    # A wall between them: muffled.
    project.put_entity(Entity("e000061", "Wall", None, {
        "transform": {"position": [ear[0] + 2.0, ear[1], ear[2]]},
        "collider": {"shape": "box", "size": [0.3, 4.0, 4.0]}}))
    walled, muffled = run(project, tmp_path, "walled")
    voice = next(v for v in walled["audio"]["voices"] if v["event"] == hum)
    assert voice["occlusion"] > 0.9
    assert levels(muffled, 0.5, 0.9, channel=1) < 0.45 * with_right


@needs_scripting
@needs_encoder
def test_reverb_zones_echo_where_the_listener_is(project, tmp_path):
    a_hum(project, tmp_path)  # (any sound: the game makes sound)
    first, _ = run(project, tmp_path, "listen", 2)
    ear = first["audio"]["listener"]
    assert first["audio"]["reverb"] == 0.0  # no zone
    project.put_entity(Entity("e000062", "Cave", None, {
        "transform": {"position": ear}, "reverb_zone": {"preset": "cave", "size": [20, 20, 20]}}))
    inside, _ = run(project, tmp_path, "inside", 2)
    assert inside["audio"]["reverb"] == pytest.approx(0.45)
    # Its box 1 m short of the listener, fading in over 4 m: three quarters of it.
    zone = project.entity("e000062")
    zone.components["transform"]["position"] = [ear[0] + 11.0, ear[1], ear[2]]
    zone.components["reverb_zone"]["edge"] = 4.0
    project.put_entity(zone)
    edge, _ = run(project, tmp_path, "edge", 2)
    assert edge["audio"]["reverb"] == pytest.approx(0.45 * 0.75, rel=0.01)


def test_the_editor_adds_reverb_zones(qtbot, project):
    panel = ScenePanel()
    qtbot.addWidget(panel)
    panel.set_project(project)
    zone = panel.add_reverb_zone()
    entity = project.entity(zone)
    assert entity.components["reverb_zone"]["preset"] == "room"
    from midgard_editor.docks.scene_panel import describe
    assert describe(entity).startswith("🏛")
    form = EntityForm(project, zone)
    qtbot.addWidget(form)
    form.reverb_preset.setCurrentIndex(form.reverb_preset.findData("cathedral"))
    form.reverb_edge.setValue(5.0)
    form.reverb_size.boxes[0].setValue(30.0)
    z = project.entity(zone).components["reverb_zone"]
    assert z["preset"] == "cathedral" and z["edge"] == 5.0 and z["size"][0] == 30.0
    project.undo_stack.undo()
    assert project.entity(zone).components["reverb_zone"]["size"][0] == 8.0
