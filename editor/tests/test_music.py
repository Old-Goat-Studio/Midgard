# SPDX-License-Identifier: Apache-2.0
"""Music (milestone 5.14 step 5): playlists kept in the project, and in the game the one that fits
the time of day, crossfading when night falls; scripts' stingers and choices."""
import math

from test_scripting import needs_scripting, simulate, write_script
from test_sound_events import a_library_sound, read_float_wav
from test_sounds import needs_encoder

from midgard_editor import music, sound_events
from midgard_editor.cook import cook
from midgard_editor.scene_model import Entity


def strength(path, hz: float, start: float, end: float) -> float:
    """How strong one frequency is in the recording's left channel between two times."""
    data, rate, channels = read_float_wav(path)
    left = data[int(start * rate) * channels:int(end * rate) * channels:channels]
    w = 2.0 * math.pi * hz / rate
    k = 2.0 * math.cos(w)
    s1 = s2 = 0.0
    for x in left:
        s1, s2 = x + k * s1 - s2, s1
    return 2.0 * math.sqrt(max(0.0, s1 * s1 + s2 * s2 - k * s1 * s2)) / max(len(left), 1)


def a_tune(project, tmp_path, name, hz):
    """A looping tone in the library's music, as a sound event."""
    file = a_library_sound(project, tmp_path, "music", name, hz, 1.0, category="tunes")
    event = sound_events.event_for_sound(project, file)
    sound_events.put(project, event)
    return event["id"]


def test_playlists_are_kept_in_the_project_with_undo(project):
    assert music.load_all(project) == []
    playlist = {"id": music.new_id(project, "Forest by day"), "name": "Forest by day",
                "playlist": {"tracks": ["sfx_x"], "biomes": ["Forest"], "times": ["day"]}}
    assert playlist["id"] == "mus_forest_by_day"
    project.undo_stack.push(music.PlaylistCommand(project, None, playlist, ""))
    assert music.load(project, "mus_forest_by_day")["playlist"]["biomes"] == ["Forest"]
    # Its problems: a track no event plays.
    assert any("sfx_x" in p for p in music.problems(project, []))
    project.undo_stack.undo()
    assert music.load_all(project) == []


NIGHTFALL = """
import midgard

frames = 0

def on_update(entity, dt):
    global frames
    frames += 1
    if frames == 60:
        print("before", midgard.music.playlist, midgard.music.track)
        # On until day and night change over (whatever the template's season), and past
        # dawn or dusk.
        was = midgard.time.is_day()
        for _ in range(24):
            if midgard.time.is_day() != was:
                break
            midgard.time.advance(hours=1)
        midgard.time.advance(hours=2)
"""


@needs_scripting
@needs_encoder
def test_the_music_fits_the_time_of_day_and_crossfades_at_nightfall(project, tmp_path):
    day = a_tune(project, tmp_path, "sunny", 220.0)
    night = a_tune(project, tmp_path, "moony", 330.0)
    for pid, name, track, times in (("mus_day", "Day", day, ["dawn", "day"]),
                                    ("mus_night", "Night", night, ["dusk", "night"])):
        music.put(project, {"id": pid, "name": name, "playlist": {
            "tracks": [track], "times": times, "crossfade": 1.0}})
    project.put_entity(Entity("e000070", "Clock", None, {
        "transform": {}, "script": {"module": "nightfall"}}))
    write_script(project, "nightfall", NIGHTFALL)
    package = cook(project, tmp_path / "pkg").out_dir
    # Which it starts with depends on the template's clock: the other comes when the script
    # moves the clock on past sunrise or sunset.
    first, _ = simulate(package, 1)
    starting = first["audio"]["music"]["playlist"]
    assert starting in ("mus_day", "mus_night")
    later = "mus_night" if starting == "mus_day" else "mus_day"
    tones = {"mus_day": 220.0, "mus_night": 330.0}
    recording = tmp_path / "heard.wav"
    dumped, run = simulate(package, 300, "--record-audio", str(recording))
    assert run.returncode == 0, run.stderr
    assert f"before {starting}" in run.stdout
    assert dumped["audio"]["music"]["playlist"] == later
    # Heard: the first tune (faded in over a second), then (the change settled for two
    # seconds after the clock moved at 1 s, then a second's crossfade) the other alone.
    assert strength(recording, tones[starting], 1.2, 1.6) > 0.25
    assert strength(recording, tones[later], 1.2, 1.6) < 0.01
    assert strength(recording, tones[later], 4.3, 4.9) > 0.25
    assert strength(recording, tones[starting], 4.3, 4.9) < 0.01


STINGER = """
import midgard

frames = 0

def on_update(entity, dt):
    global frames
    frames += 1
    if frames == 30:
        print("sting", midgard.music.stinger("Sting") is not None)
    if frames == 120:
        midgard.music.play("Other")
    if frames == 200:
        try:
            midgard.music.play("Jazz")
        except ValueError as e:
            print("refused:", e)
        midgard.music.stop()
"""


@needs_scripting
@needs_encoder
def test_scripts_choose_music_and_play_stingers(project, tmp_path):
    hum = a_tune(project, tmp_path, "hum", 220.0)
    other = a_tune(project, tmp_path, "other", 330.0)
    sting = a_library_sound(project, tmp_path, "effects", "sting", 880.0, 0.4)
    event = sound_events.event_for_sound(project, sting)
    event["name"] = "Sting"
    sound_events.put(project, event)
    music.put(project, {"id": "mus_hum", "name": "Hum", "playlist": {
        "tracks": [hum], "crossfade": 0.0}})
    music.put(project, {"id": "mus_other", "name": "Other", "playlist": {
        "tracks": [other], "crossfade": 0.0, "scenes": ["nowhere"]}})
    project.put_entity(Entity("e000071", "Director", None, {
        "transform": {}, "script": {"module": "director"}}))
    write_script(project, "director", STINGER)
    package = cook(project, tmp_path / "pkg").out_dir
    recording = tmp_path / "heard.wav"
    dumped, run = simulate(package, 260, "--record-audio", str(recording))
    assert run.returncode == 0, run.stderr
    assert "sting True" in run.stdout
    assert "refused: there is no playlist 'Jazz'" in run.stdout
    # The hum dips under the stinger (0.5 s in), then back; "Other" (forced, though it fits
    # nowhere) from 2 s; silence after the stop at 3.3 s.
    assert strength(recording, 220.0, 0.1, 0.45) > 0.25
    assert strength(recording, 880.0, 0.65, 0.85) > 0.2
    assert strength(recording, 220.0, 0.65, 0.85) < 0.6 * strength(recording, 220.0, 0.1, 0.45)
    assert strength(recording, 220.0, 1.6, 1.95) > 0.25
    assert strength(recording, 330.0, 2.2, 3.2) > 0.25
    assert dumped["audio"]["music"]["playlist"] == ""
    assert strength(recording, 330.0, 4.0, 4.3) < 0.01
