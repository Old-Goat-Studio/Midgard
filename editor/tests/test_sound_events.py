# SPDX-License-Identifier: Apache-2.0
"""Sound events and the sound component (milestone 5.14 step 3): made in the project, set on an
entity in the Inspector, played by the game and by scripts, and heard (mixed offline)."""
import array
import math
import struct

import pytest
from test_scripting import needs_scripting, scripting_runtime, write_script
from test_sounds import a_wav, needs_encoder

from midgard_editor import asset_library as lib
from midgard_editor import sound_events, sounds
from midgard_editor.cook import cook
from midgard_editor.docks.entity_form import EntityForm
from midgard_editor.scene_model import Entity


def a_library_sound(project, tmp_path, type_, name, hz, seconds, category="misc"):
    asset = lib.import_asset(project, [a_wav(tmp_path / f"{name}.wav", hz, seconds)], type_,
                             name.capitalize(), category=category)
    return asset.folder / f"{name}.wav"


def test_sound_events_are_kept_in_the_project_with_undo(project, tmp_path):
    assert sound_events.load_all(project) == []  # (migration 14 made the table)
    file = a_library_sound(project, tmp_path, "effects", "bell", 800.0, 0.2)
    event = sound_events.event_for_sound(project, file)
    assert event["id"] == "sfx_bell" and event["name"] == "Bell"
    assert event["event"]["bus"] == "effects" and "loop" not in event["event"]
    assert sound_events.sounds_of(event) == [sounds.ref(
        file.relative_to(project.root).as_posix())]
    stack = project.undo_stack
    stack.push(sound_events.SoundEventCommand(project, None, event, ""))
    assert sound_events.load(project, "sfx_bell")["name"] == "Bell"
    assert sound_events.find_for_sound(project, sound_events.sounds_of(event)[0])["id"] == \
        "sfx_bell"
    changed = {**event, "event": {**event["event"], "volume": [0.5, 0.7]}}
    stack.push(sound_events.SoundEventCommand(project, event, changed, "volume", "volume"))
    assert sound_events.load(project, "sfx_bell")["event"]["volume"] == [0.5, 0.7]
    stack.undo()
    assert "volume" not in sound_events.load(project, "sfx_bell")["event"]
    stack.undo()
    assert sound_events.load_all(project) == []
    # Music and ambiance loop, on their own buses; music isn't placed in the world.
    song = sound_events.event_for_sound(
        project, a_library_sound(project, tmp_path, "music", "theme", 220.0, 1.0))
    assert song["event"]["bus"] == "music" and song["event"]["loop"]
    assert song["event"]["spatial"] is False
    wind = sound_events.event_for_sound(
        project, a_library_sound(project, tmp_path, "ambiance", "wind", 150.0, 1.0))
    assert wind["event"]["bus"] == "ambience" and wind["event"]["loop"]
    assert sound_events.new_id(project, "Bell") == "sfx_bell"


def test_the_inspector_gives_an_entity_a_sound_from_the_library(qtbot, project, tmp_path):
    file = a_library_sound(project, tmp_path, "effects", "chime", 600.0, 0.3)
    project.put_entity(Entity("e000050", "Wind Chime", None, {"transform": {}}))
    form = EntityForm(project, "e000050")
    qtbot.addWidget(form)
    form.add("sound")
    assert project.entity("e000050").components["sound"] == {
        "event": "", "autoplay": True, "volume": 1.0}
    form = EntityForm(project, "e000050")
    qtbot.addWidget(form)
    assert form.sound_event is not None
    # From the library: an event made for it, and the entity plays it (one step).
    event = form.use_library_sound(file)
    assert project.entity("e000050").components["sound"]["event"] == event
    assert sound_events.load(project, event)["name"] == "Chime"
    # The same sound again: the same event (not another).
    project.put_entity(Entity("e000051", "Second Chime", None, {
        "transform": {}, "sound": {"event": ""}}))
    other = EntityForm(project, "e000051")
    qtbot.addWidget(other)
    assert other.use_library_sound(file) == event
    assert len(sound_events.load_all(project)) == 1
    # Its rows: the event chosen, whether it plays at the start, its volume.
    form = EntityForm(project, "e000050")
    qtbot.addWidget(form)
    assert form.sound_event.currentData() == event
    form.sound_autoplay.setChecked(False)
    form.sound_volume.setValue(0.5)
    sound = project.entity("e000050").components["sound"]
    assert sound["autoplay"] is False and sound["volume"] == 0.5
    project.undo_stack.undo()
    project.undo_stack.undo()
    project.undo_stack.undo()  # (the second chime's choice)
    project.undo_stack.undo()  # the first's: the event and the choice go together
    assert project.entity("e000050").components["sound"]["event"] == ""
    assert sound_events.load_all(project) == []


def test_the_cook_packs_every_events_sounds_and_warns_of_unknown_ones(project, tmp_path):
    from midgard_editor import engine
    if engine.module() is None or not hasattr(engine.module(), "encode_sound"):
        pytest.skip("needs the engine module")
    file = a_library_sound(project, tmp_path, "effects", "knock", 300.0, 0.2)
    event = sound_events.event_for_sound(project, file)
    sound_events.put(project, event)  # (played by scripts only: still packed)
    project.put_entity(Entity("e000052", "Ghost", None, {
        "transform": {}, "sound": {"event": "sfx_nothing"}}))
    result = cook(project, tmp_path / "out")
    assert sounds.sounds_in(event["event"])[0] in {c["id"] for c in result.manifest["content"]}
    assert any("sfx_nothing" in w and "silent" in w for w in result.warnings)


SPEAKER = """
import midgard

frames = 0

def on_start(entity):
    bell = midgard.audio.play("Bell")
    print("bell", bell is not None, midgard.audio.play("Nope") is None,
          midgard.audio.enabled, midgard.audio.volume("music"))

def on_update(entity, dt):
    global frames
    frames += 1
    if frames == 60:  # a second in: everything silent
        midgard.audio.set_volume("master", 0.0)
"""


def read_float_wav(path) -> tuple[array.array, int, int]:
    """A 32-bit float WAV's samples, rate and channels (Python's wave module can't read them)."""
    data = path.read_bytes()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE"
    channels, rate = struct.unpack_from("<HI", data, 22)
    at = data.index(b"data") + 8
    samples = array.array("f")
    samples.frombytes(data[at:at + struct.unpack_from("<I", data, at - 4)[0]])
    return samples, rate, channels


def levels(path, start: float, end: float) -> float:
    """The recording's RMS (left) between two times."""
    data, rate, channels = read_float_wav(path)
    left = data[int(start * rate) * channels:int(end * rate) * channels:channels]
    return math.sqrt(sum(x * x for x in left) / max(len(left), 1))


@needs_scripting
@needs_encoder
def test_the_game_plays_an_entitys_sound_and_a_scripts(project, tmp_path):
    from test_scripting import simulate
    theme = a_library_sound(project, tmp_path, "music", "theme", 220.0, 0.5)
    bell = a_library_sound(project, tmp_path, "effects", "bell", 900.0, 0.2)
    for file in (theme, bell):
        sound_events.put(project, sound_events.event_for_sound(project, file))
    project.put_entity(Entity("e000053", "Radio", None, {
        "transform": {}, "sound": {"event": "sfx_theme", "volume": 0.5},
        "script": {"module": "speaker"}}))
    write_script(project, "speaker", SPEAKER)
    package = cook(project, tmp_path / "pkg").out_dir
    recording = tmp_path / "heard.wav"
    dumped, run = simulate(package, 120, "--record-audio", str(recording))
    assert run.returncode == 0, run.stderr
    assert "bell True True True 1.0" in run.stdout
    assert (run.stdout + run.stderr).count("no sound event 'Nope'") == 1  # (a warning, once)
    # Both events playing at the end (looped music; the bell's long over, but the dump comes
    # after the master went down, which doesn't stop anything).
    assert dumped["audio"]["playing"] == ["sfx_theme"]
    # Heard: the theme looping at half its level (a 0.366 sine: RMS 0.259), and silence once
    # the script turned the master down.
    assert levels(recording, 0.4, 0.9) == pytest.approx(0.259 * 0.5, rel=0.06)
    assert levels(recording, 1.05, 1.95) == 0.0
    # The bell rang at the start, over the music.
    assert levels(recording, 0.0, 0.15) > levels(recording, 0.4, 0.9) * 1.3
    assert scripting_runtime() is not None
