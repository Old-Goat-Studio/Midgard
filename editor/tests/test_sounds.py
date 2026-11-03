# SPDX-License-Identifier: Apache-2.0
"""Sounds into the game (milestone 5.14 step 2): the cook makes the sounds the scenes name into
Ogg Vorbis (music, ambience) or Ogg Opus (the rest), reads each back as a game would, keeps
them for next time, and credits the library's."""
import math
import struct
import wave
import zipfile
from pathlib import Path

import pytest

from midgard_editor import asset_library as lib
from midgard_editor import engine, sounds
from midgard_editor.cook import cook
from midgard_editor.scene_model import Entity

needs_encoder = pytest.mark.skipif(
    engine.module() is None or not hasattr(engine.module(), "encode_sound"),
    reason="needs the engine module")


def a_wav(path: Path, hz: float = 440.0, seconds: float = 0.5, channels: int = 1,
          rate: int = 44100) -> Path:
    """A 16-bit tone."""
    frames = int(seconds * rate)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(
            struct.pack("<h", int(12000 * math.sin(2 * math.pi * hz * i / rate))) * channels
            for i in range(frames)))
    return path


def test_ids_codecs_and_where_they_go():
    assert sounds.ref("library/effects/misc/door/creak.wav") == \
        "snd:library/effects/misc/door/creak.wav"
    assert sounds.parse("snd:audio/a.wav") == "audio/a.wav" and sounds.parse("tex:x") is None
    assert sounds.codec("snd:library/music/misc/theme/song.mp3") == "vorbis"
    assert sounds.codec("snd:library/ambiance/forest/birds/dawn.wav") == "vorbis"
    assert sounds.codec("snd:audio/music/song.wav") == "vorbis"
    assert sounds.codec("snd:library/effects/misc/door/creak.wav") == "opus"
    assert sounds.codec("snd:audio/click.wav") == "opus"
    assert sounds.pak_path("snd:audio/click.wav") == "audio/audio/click.wav.opus"
    assert sounds.pak_path("snd:library/music/a/b.mp3") == "audio/library/music/a/b.mp3.ogg"
    # Found however deep a component holds them, once each.
    assert sounds.sounds_in({"a": "snd:x.wav", "b": [{"c": "snd:y.wav"}, "snd:x.wav"],
                             "d": 3, "e": "lib:z"}) == ["snd:x.wav", "snd:y.wav"]


@needs_encoder
def test_the_cook_packs_the_sounds_the_scenes_use(project, tmp_path, monkeypatch):
    monkeypatch.setattr(sounds, "CACHE", sounds.SoundCache())
    (project.root / "audio").mkdir(exist_ok=True)
    a_wav(project.root / "audio" / "click.wav", 1000.0, 0.2)
    music = lib.import_asset(project, [a_wav(tmp_path / "theme.wav", 220.0, 1.0, channels=2)],
                             "music", "Theme", category="Tales", licence="CC-BY-4.0",
                             author="A. Composer", source="https://example.com/theme",
                             title="Theme")
    song = sounds.ref((music.folder / "theme.wav").relative_to(project.root).as_posix())
    project.put_entity(Entity(project.next_entity_id(), "Jukebox", None, {
        "transform": {}, "test_sounds": {"click": "snd:audio/click.wav", "song": song,
                                         "lost": "snd:audio/nowhere.wav"}}))
    result = cook(project, tmp_path / "out")
    # The one that isn't there: a warning, and the game goes on silent.
    assert any("audio/nowhere.wav" in w and "silent" in w for w in result.warnings)
    with zipfile.ZipFile(result.out_dir / "game.pak") as pak:
        click = pak.read("audio/audio/click.wav.opus")
        theme = pak.read(sounds.pak_path(song))
    assert sounds.pak_path(song).endswith(".ogg")
    ids = {c["id"]: c["path"] for c in result.manifest["content"]}
    assert ids["snd:audio/click.wav"] == "audio/audio/click.wav.opus"
    # The game's decoders read them: Opus (48 kHz) and Vorbis (its own rate), their lengths.
    module = engine.module()
    info = module.sound_info(click)
    assert info["sample_rate"] == 48000 and info["seconds"] == pytest.approx(0.2, abs=0.01)
    info = module.sound_info(theme)
    assert info["sample_rate"] == 44100 and info["channels"] == 2
    assert info["seconds"] == pytest.approx(1.0, abs=0.01)
    assert len(theme) < (music.folder / "theme.wav").stat().st_size / 4
    # The CC BY music is credited.
    credits = (result.out_dir / lib.CREDITS_FILE).read_text("utf-8")
    assert "A. Composer" in credits and "Theme" in credits
    # Made once: cooked again, nothing is encoded (kept on disk, even with memory cleared).
    made = sounds.CACHE.encodes
    assert made == 2
    monkeypatch.setattr(sounds, "CACHE", sounds.SoundCache())
    cook(project, tmp_path / "again")
    assert sounds.CACHE.encodes == 0
    # Changed: made again.
    a_wav(project.root / "audio" / "click.wav", 500.0, 0.3)
    cook(project, tmp_path / "third")
    assert sounds.CACHE.encodes == 1


@needs_encoder
def test_a_sound_that_cant_be_read_is_a_warning(project, tmp_path):
    (project.root / "audio").mkdir(exist_ok=True)
    (project.root / "audio" / "broken.wav").write_bytes(b"RIFF....WAVEnot really")
    project.put_entity(Entity(project.next_entity_id(), "Radio", None, {
        "transform": {}, "test_sounds": {"s": "snd:audio/broken.wav"}}))
    result = cook(project, tmp_path / "out")
    assert any("broken.wav" in w and "can't be read" in w for w in result.warnings)
    with zipfile.ZipFile(result.out_dir / "game.pak") as pak:
        assert not [n for n in pak.namelist() if n.startswith("audio/")]
