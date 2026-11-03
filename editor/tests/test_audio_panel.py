# SPDX-License-Identifier: Apache-2.0
"""The Audio panel (milestone 5.14 step 8): sound events, playlists, ambience and the mixer edited
with undo, previewed through the engine's own mixer, and followed by a game playing under the
editor."""
import array
import math

import pytest
from test_sound_events import a_library_sound

from midgard_editor import ambience, engine, music, sound_events
from midgard_editor.docks.audio_panel import AudioPanel

needs_preview = pytest.mark.skipif(
    engine.module() is None or not hasattr(engine.module(), "AudioPreview"),
    reason="needs the engine module")


@pytest.fixture
def panel(qtbot, project):
    p = AudioPanel()
    qtbot.addWidget(p)
    p.set_project(project)
    return p


def test_the_sounds_tab_makes_and_edits_sound_events(panel, project, tmp_path):
    bell = a_library_sound(project, tmp_path, "effects", "bell", 800.0, 0.2)
    gong = a_library_sound(project, tmp_path, "effects", "gong", 200.0, 0.2)
    tab = panel.sounds_tab
    eid = tab.add_from_file(bell)
    assert tab.chosen_id() == eid and tab.list.count() == 1
    event = sound_events.load(project, eid)
    tab.change(event, "volume", [0.6, 0.9], "volume")
    tab.change(sound_events.load(project, eid), "bus", "voice", "bus")
    tab.change(sound_events.load(project, eid), "max_distance", 25.0, "heard within")
    tab.add_variant(sound_events.load(project, eid), gong)
    e = sound_events.load(project, eid)["event"]
    assert e["volume"] == [0.6, 0.9] and e["bus"] == "voice" and e["max_distance"] == 25.0
    assert len(e["sounds"]) == 2
    assert tab.sounds_table.rowCount() == 2  # (the form shows both)
    # Its weights, from the form.
    tab.sounds_table.cellWidget(1, 1).setValue(3.0)
    assert sound_events.load(project, eid)["event"]["sounds"][1]["weight"] == 3.0
    # (The weight merges with adding its sound: both change the sounds.)
    for _ in range(4):
        project.undo_stack.undo()
    assert sound_events.load(project, eid)["event"].get("volume") is None
    project.undo_stack.undo()
    assert sound_events.load_all(project) == []


def test_the_music_and_ambience_tabs_make_playlists_and_sets(panel, project, tmp_path):
    tune = sound_events.event_for_sound(
        project, a_library_sound(project, tmp_path, "music", "tune", 220.0, 0.5))
    sound_events.put(project, tune)
    pid = panel.music_tab.add("Forest days")
    tab = panel.music_tab
    tab.change(music.load(project, pid), "tracks", [tune["id"]], "tracks")
    assert tab.tracks.count() == 1
    tab.change(music.load(project, pid), "times", ["day"], "times")
    tab.change(music.load(project, pid), "crossfade", 5.0, "crossfade")
    p = music.load(project, pid)["playlist"]
    assert p["tracks"] == [tune["id"]] and p["times"] == ["day"] and p["crossfade"] == 5.0
    assert music.problems(project, sound_events.load_all(project)) == []
    # Ambience: a layer, at night, half as loud.
    aid = panel.ambience_tab.add("Crickets")
    at = panel.ambience_tab
    at.change(ambience.load(project, aid), "layers",
              [{"event": tune["id"], "times": ["night"], "volume": 0.5}], "layers")
    assert at.layers_table.rowCount() == 1
    assert at.layers_table.cellWidget(0, 4).isChecked()  # (the Night column)
    at.layers_table.cellWidget(0, 2).setChecked(True)    # and Day
    layer = ambience.load(project, aid)["ambience"]["layers"][0]
    assert layer["times"] == ["day", "night"] and layer["volume"] == 0.5


def test_the_mixer_sets_the_buses_volumes(panel, project):
    tab = panel.mixer_tab
    tab.faders["music"].setValue(60)
    tab.faders["music"].setValue(50)  # (one drag: one step)
    assert sound_events.load_buses(project)["music"] == 0.5
    assert tab.values["music"].text() == "50%"
    project.undo_stack.undo()
    assert sound_events.load_buses(project)["music"] == 1.0
    panel.refresh()
    assert tab.faders["music"].value() == 100


def heard(preview, seconds: float) -> float:
    data = array.array("f")
    data.frombytes(preview.preview.mix(seconds))
    left = data[0::2]
    return math.sqrt(sum(x * x for x in left) / max(len(left), 1))


@needs_preview
def test_previews_are_heard_as_the_game_plays_them(qtbot, project, tmp_path):
    from midgard_editor.sound_preview import SoundPreview
    preview = SoundPreview(device=False)
    assert preview.available()
    bell = sound_events.event_for_sound(
        project, a_library_sound(project, tmp_path, "effects", "bell", 800.0, 0.5))
    tune = sound_events.event_for_sound(
        project, a_library_sound(project, tmp_path, "music", "tune", 220.0, 1.0))
    for e in (bell, tune):
        sound_events.put(project, e)
    assert preview.play_event(project, bell["id"])
    assert heard(preview, 0.2) > 0.2  # (a 0.37 sine: 0.26)
    preview.stop()
    assert heard(preview, 0.1) == 0.0
    music.put(project, {"id": "mus_x", "name": "X", "playlist": {
        "tracks": [tune["id"]], "crossfade": 0}})
    assert preview.play_playlist(project, "mus_x")
    assert heard(preview, 0.3) > 0.2 and preview.preview.playlist == "mus_x"
    preview.stop()
    ambience.put(project, {"id": "amb_x", "name": "X", "ambience": {
        "layers": [{"event": tune["id"], "times": ["night"]}], "crossfade": 0}})
    assert preview.play_ambience(project, "amb_x", "day")
    assert heard(preview, 0.2) == 0.0  # (its layer is for the night)
    preview.stop()
    assert preview.play_ambience(project, "amb_x", "night")
    assert heard(preview, 0.3) > 0.2 and preview.preview.layers == [tune["id"]]


@needs_preview
def test_a_game_under_the_editor_follows_the_audio_panel(qtbot, panel, project, tmp_path):
    """Live editing: a playlist made while the game plays is playing in it a moment later."""
    from midgard_editor.about import runtime_candidates
    from midgard_editor.cook import cook
    from midgard_editor.live_link import PlaySession, State
    if not runtime_candidates():
        pytest.skip("engine not built here")
    tune = sound_events.event_for_sound(
        project, a_library_sound(project, tmp_path, "music", "tune", 220.0, 1.0))
    sound_events.put(project, tune)
    session = PlaySession(project)
    try:
        received = []
        session.message_received.connect(received.append)
        session.start(runtime_candidates()[0], cook(project).out_dir)
        qtbot.waitUntil(lambda: session.state == State.PLAYING, timeout=20000)
        pid = panel.music_tab.add("Anywhere")
        panel.music_tab.change(music.load(project, pid), "tracks", [tune["id"]], "tracks")
        qtbot.waitUntil(lambda: sum(m.get("text") == "applied audio.put"
                                    for m in received) >= 2, timeout=10000)
        session.send({"type": "scene.dump"})
        qtbot.waitUntil(lambda: any(m.get("type") == "scene" for m in received), timeout=5000)
        dumped = next(m for m in received if m.get("type") == "scene")["scene"]
        if dumped.get("audio") is not None:  # (a game with a sound device: CI has none)
            assert dumped["audio"]["music"]["playlist"] == pid
        assert not [m for m in received if m.get("level") in ("warn", "error")]
    finally:
        session.stop()
