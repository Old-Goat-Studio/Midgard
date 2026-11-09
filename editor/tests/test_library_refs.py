# SPDX-License-Identifier: Apache-2.0
"""Organizing a project's Asset Library keeps the game working: every path it uses to a moved
asset or collection changes to match, in the same change (all or nothing); scripts are shown,
and changed only when you say so."""
import json
import sqlite3

import pytest

from midgard_editor import asset_library as lib
from midgard_editor import engine, sound_events
from midgard_editor import library_refs as refs
from midgard_editor import library_tree as lt
from midgard_editor.scene_model import Entity


def make(project, name, type_, collection, files):
    folder = lib.type_folder(project, type_, collection) / lib.slug(name)
    folder.mkdir(parents=True)
    for rel, data in files.items():
        (folder / rel).write_bytes(data)
    asset = lib.Asset(folder=folder, name=name, type=type_, category=collection)
    lib.record_files(asset)
    lib.write(asset)
    return asset


@pytest.fixture
def game(project):
    """A project whose game uses a library model, texture and sound, and a script naming one."""
    make(project, "Hero", "models", "", {"hero.glb": b"glTF"})
    make(project, "Rusty", "textures", "metal", {"rusty_basecolor.png": b"\x89PNG"})
    make(project, "Walk", "effects", "NOX Footsteps", {"walk.wav": b"RIFF"})
    project.put_entity(Entity("e1", "Hero", components={
        "mesh": {"asset": "lib:models/hero/hero.glb"},
        "material": {"textures": {
            "base_color": "tex:library/textures/metal/rusty/rusty_basecolor.png"}}}))
    sound_events.put(project, {"id": "steps", "name": "Steps", "event": {"sounds": [
        {"sound": "snd:library/effects/nox_footsteps/walk/walk.wav", "weight": 1}]}})
    (project.root / "scripts").mkdir(exist_ok=True)
    (project.root / "scripts" / "steps.py").write_text(
        "import midgard\n\n"
        "STEP = 'snd:library/effects/nox_footsteps/walk/walk.wav'\n"
        "OTHER = 'snd:library/effects/nox_footsteps_two/x.wav'\n", "utf-8")
    project.set_config("splash", "tex:library/textures/metal/rusty/rusty_basecolor.png")
    return project


def entity(project, entity_id="e1"):
    return project.entity(entity_id).components


def step_sound(project):
    return sound_events.load(project, "steps")["event"]["sounds"][0]["sound"]


def test_paths_change_only_inside_moved_folders():
    moved = [("effects/nox_footsteps", "effects/steps")]
    text = ('"snd:library/effects/nox_footsteps/walk/walk.wav" '
            '"snd:library/effects/nox_footsteps_two/x.wav" '   # (a different folder)
            '"lib:effects/nox_footsteps/a.wav" "mylibrary/effects/nox_footsteps/a"')
    new, count = refs.rewrite(text, moved)
    assert count == 2
    assert new == ('"snd:library/effects/steps/walk/walk.wav" '
                   '"snd:library/effects/nox_footsteps_two/x.wav" '
                   '"lib:effects/steps/a.wav" "mylibrary/effects/nox_footsteps/a"')
    # Moving one folder where another was, in one change, never chains.
    swapped = [("models/a", "models/b"), ("models/b", "models/a")]
    assert refs.rewrite("lib:models/a/x lib:models/b/y", swapped)[0] == \
        "lib:models/b/x lib:models/a/y"


def test_renaming_a_collection_changes_the_game_to_match(game):
    shown = []

    def confirm(uses):
        shown.append(uses)
        return False  # (leave the scripts)
    moved = refs.rename_collection(game, "effects/nox_footsteps", "Footsteps", confirm)
    assert moved == [("effects/nox_footsteps", "effects/footsteps")]
    assert step_sound(game) == "snd:library/effects/footsteps/walk/walk.wav"
    uses = shown[0]
    assert uses.database == {"sound_event": 1}
    assert [(s.file, s.line) for s in uses.scripts] == [("scripts/steps.py", 3)]
    assert uses.summary() == "1 sound event"
    many = refs.Uses(database={"entity": 14, "sound_event": 2}, files=["Demo.config"])
    assert many.summary() == "14 entities, 2 sound events and 1 file"
    # The script was shown, not changed.
    assert "nox_footsteps/walk" in (game.root / "scripts/steps.py").read_text("utf-8")


def test_scripts_are_changed_when_you_say_so(game):
    refs.rename_collection(game, "effects/nox_footsteps", "Footsteps", lambda uses: True)
    text = (game.root / "scripts/steps.py").read_text("utf-8")
    assert "'snd:library/effects/footsteps/walk/walk.wav'" in text
    assert "nox_footsteps_two" in text  # (another folder: left alone)


def test_renaming_and_moving_assets_change_models_textures_and_the_config(game):
    refs.rename_asset(game, "models/hero", "Main Hero")
    assert entity(game)["mesh"]["asset"] == "lib:models/main_hero/hero.glb"
    lt.new_collection(lib.library_dir(game), "textures", "Sci-fi")
    refs.move(game, ["textures/metal/rusty"], "textures/sci_fi")
    new = "tex:library/textures/sci_fi/rusty/rusty_basecolor.png"
    assert entity(game)["material"]["textures"]["base_color"] == new
    # The config file and the editor's copy of it.
    assert json.loads(game.config_path.read_text("utf-8"))["splash"] == new
    assert game.config["splash"] == new
    # And a library model still resolves.
    assert lib.model_file(game, entity(game)["mesh"]["asset"]) is not None


def test_saying_no_changes_nothing(game):
    before = {p.relative_to(game.root).as_posix(): p.read_bytes()
              for p in game.root.rglob("*") if p.is_file()}

    def no(uses):
        raise refs.Cancelled("you said no")
    with pytest.raises(refs.Cancelled):
        refs.rename_collection(game, "effects/nox_footsteps", "Footsteps", no)
    after = {p.relative_to(game.root).as_posix(): p.read_bytes()
             for p in game.root.rglob("*") if p.is_file()}
    assert after == before


def test_when_the_game_cant_be_changed_the_library_isnt_either(game, monkeypatch):
    real_connect, calls = game.connect, []

    def connect():  # reading works; writing finds the database locked
        calls.append(1)
        if len(calls) > 2:
            raise sqlite3.OperationalError("database is locked")
        return real_connect()
    monkeypatch.setattr(game, "connect", connect)
    config_before = game.config_path.read_bytes()
    with pytest.raises(lt.OrganizeError, match="locked"):
        refs.rename_asset(game, "textures/metal/rusty", "Old Rust")
    monkeypatch.undo()
    assert (lib.library_dir(game) / "textures/metal/rusty/asset.json").is_file()
    assert not (lib.library_dir(game) / "textures/metal/old_rust").exists()
    assert game.config_path.read_bytes() == config_before  # (it had been changed: put back)
    assert entity(game)["material"]["textures"]["base_color"].startswith(
        "tex:library/textures/metal/rusty/")


def test_undo_is_cleared_after_organizing(game):
    from midgard_editor import undo
    game.undo_stack.push(undo.SetConfigCommand(game, "splash", "x", "splash"))
    assert game.undo_stack.count() == 1
    refs.rename_asset(game, "models/hero", "Main Hero")
    assert game.undo_stack.count() == 0


def test_build_and_library_folders_are_never_rewritten(game):
    cooked = game.root / "build" / "cook" / "manifest.json"
    cooked.parent.mkdir(parents=True)
    cooked.write_text('{"x": "library/models/hero/hero.glb"}', "utf-8")
    refs.rename_asset(game, "models/hero", "Main Hero")
    assert "models/hero/" in cooked.read_text("utf-8")  # (made again by the next cook)


# ---- the exit: a reorganized game still cooks and plays ---------------------------------------
def _sample_game(project):
    """A game using Midgard's samples: the Avocado model (with its textures) and footsteps."""
    from midgard_editor import sounds
    added, _ = lib.add_samples(project)
    avocado = next(a for a in added if a.name == "Avocado")
    steps = next(a for a in added if a.type == "effects")
    model = next(p for p in avocado.file_paths() if p.suffix == ".gltf")
    sound = next(p for p in steps.file_paths() if p.suffix == ".ogg")
    project.put_entity(Entity(project.next_entity_id(), "Avocado", components={
        "transform": {"position": [0.0, 1.0, 0.0]},
        "mesh": {"asset": lib.model_id(project, model)}}))
    sound_events.put(project, {"id": "steps", "name": "Steps", "event": {"sounds": [
        {"sound": sounds.ref(sound.relative_to(project.root).as_posix()), "weight": 1}]}})
    return avocado, steps


def _missing(result) -> list[str]:
    return [w for w in result.warnings if "which is not" in w or "which the project" in w]


def _reorganize(project, avocado, steps, game_too: bool):
    """Renames the Avocado and moves it and the footsteps album into new collections: with the
    game changed to match (library_refs), or the library alone (library_tree)."""
    root = lib.library_dir(project)
    where = project if game_too else root
    organize = refs if game_too else lt
    lt.new_collection(root, "models", "Food")
    lt.new_collection(root, "effects", "Footsteps")
    organize.rename_asset(where, avocado.id, "Ripe Avocado")
    organize.move(where, ["models/ripe_avocado"], "models/food")
    organize.move(where, [steps.id.rsplit("/", 1)[0]], "effects/footsteps")


needs_engine = pytest.mark.skipif(engine.module() is None, reason="engine module not built")


@needs_engine
def test_the_exit_a_reorganized_game_still_cooks_and_plays(project, tmp_path):
    from test_scenes import run_game

    from midgard_editor.cook import cook
    avocado, steps = _sample_game(project)
    assert _missing(cook(project, tmp_path / "before")) == []
    _reorganize(project, avocado, steps, game_too=True)
    result = cook(project, tmp_path / "after")
    assert _missing(result) == []
    assert entity_mesh(project).startswith("lib:models/food/ripe_avocado/")
    dump = run_game(tmp_path / "after", 5)
    assert any(e.get("name") == "Avocado" for e in dump.get("entities", []))


@needs_engine
def test_without_the_game_changed_to_match_the_cook_would_miss_them(project, tmp_path):
    # (What step 3 stops: the library alone reorganized leaves the game naming old paths.)
    from midgard_editor.cook import cook
    avocado, steps = _sample_game(project)
    _reorganize(project, avocado, steps, game_too=False)
    missing = _missing(cook(project, tmp_path / "pkg"))
    assert any("Avocado" in w for w in missing) and any("sound" in w for w in missing)


def entity_mesh(project) -> str:
    return next(e.components["mesh"]["asset"] for e in project.all_entities()
                if e.name == "Avocado")
