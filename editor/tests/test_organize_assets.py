# SPDX-License-Identifier: Apache-2.0
"""Organizing in the Asset Library window: the same commands as the Master's (organize_ui, the
same file in both), with the game changed to match, and scripts naming what moves asked
about first."""
import pytest
from test_asset_window import OGG, a_picture, type_row

from midgard_editor import asset_library as lib
from midgard_editor import sound_events
from midgard_editor.docks.asset_window import AssetWindow
from midgard_editor.organize_ui import KIND_ROLE
from midgard_editor.scene_model import Entity


@pytest.fixture
def window(project, qtbot, tmp_path):
    """The window on a project whose game uses a texture and a sound from its library, and
    a script naming the sound."""
    lib.import_asset(project, [a_picture(tmp_path, "zinc.png")], "textures", "Zinc",
                     category="metal")
    (tmp_path / "step.ogg").write_bytes(OGG)
    lib.import_asset(project, [tmp_path / "step.ogg"], "effects", "Step",
                     category="NOX Footsteps")
    project.put_entity(Entity("e1", "Wall", components={"material": {"textures": {
        "base_color": "tex:library/textures/metal/zinc/zinc.png"}}}))
    sound_events.put(project, {"id": "steps", "name": "Steps", "event": {"sounds": [
        {"sound": "snd:library/effects/nox_footsteps/step/step.ogg", "weight": 1}]}})
    (project.root / "scripts").mkdir(exist_ok=True)
    (project.root / "scripts/steps.py").write_text(
        "STEP = 'snd:library/effects/nox_footsteps/step/step.ogg'\n", "utf-8")
    w = AssetWindow()
    qtbot.addWidget(w)
    w.quiet = True
    w.set_project(project)
    o = w.organizer
    w.said = {"text": None, "told": []}
    o.ask_text = lambda title, label, text: w.said["text"]
    o.ask_yes = lambda title, text: True
    o.tell = lambda title, text: w.said["told"].append(text)
    return w


def show(window, type_: str) -> None:
    window.types.setCurrentRow(type_row(window, type_))


def choose(window, path: str) -> None:
    window.tree.clearSelection()
    window.tree.select_path(path)


def names(window) -> list[str]:
    out = []

    def walk(item, depth):
        out.append("  " * depth + item.text(0))
        for i in range(item.childCount()):
            walk(item.child(i), depth + 1)
    for i in range(window.tree.topLevelItemCount()):
        walk(window.tree.topLevelItem(i), 0)
    return out


def step_sound(project) -> str:
    return sound_events.load(project, "steps")["event"]["sounds"][0]["sound"]


def test_the_tree_shows_collections_with_their_album_names(window):
    show(window, "effects")
    assert names(window) == ["NOX Footsteps", "  Step"]
    assert window.tree.topLevelItem(0).data(0, KIND_ROLE) == "collection"


def test_renaming_a_collection_changes_the_game_and_leaves_scripts_unless_asked(window):
    project = window.project
    show(window, "effects")
    choose(window, "effects/nox_footsteps")
    window.said["text"] = "Footsteps"
    window.fix_scripts = False
    assert window.organizer.rename()
    assert names(window) == ["Footsteps", "  Step"]
    assert step_sound(project) == "snd:library/effects/footsteps/step/step.ogg"
    assert "nox_footsteps" in (project.root / "scripts/steps.py").read_text("utf-8")
    # And with the scripts changed too.
    choose(window, "effects/footsteps")
    window.said["text"] = "Steps"
    window.fix_scripts = True
    assert window.organizer.rename()
    assert step_sound(project) == "snd:library/effects/steps/step/step.ogg"
    # (the script still named the first path: left, as asked then)
    assert "nox_footsteps" in (project.root / "scripts/steps.py").read_text("utf-8")


def test_scripts_changed_when_you_say_so(window):
    show(window, "effects")
    choose(window, "effects/nox_footsteps")
    window.said["text"] = "Footsteps"
    window.fix_scripts = True
    assert window.organizer.rename()
    assert "effects/footsteps/step/step.ogg" in \
        (window.project.root / "scripts/steps.py").read_text("utf-8")


def test_saying_no_to_the_scripts_question_changes_nothing(window, monkeypatch):
    from midgard_editor import library_refs as refs

    def no(uses):
        raise refs.Cancelled("you said no")
    monkeypatch.setattr(window, "_confirm", no)
    window.organizer.backend.confirm = no
    show(window, "effects")
    choose(window, "effects/nox_footsteps")
    window.said["text"] = "Footsteps"
    assert not window.organizer.rename()
    assert window.said["told"] == []  # (nothing said: you said no)
    assert names(window) == ["NOX Footsteps", "  Step"]
    assert step_sound(window.project).startswith("snd:library/effects/nox_footsteps/")


def test_moving_a_texture_into_a_new_collection_by_drop(window):
    project = window.project
    show(window, "textures")
    window.tree.clearSelection()
    window.said["text"] = "Building"
    assert window.organizer.new_collection()
    assert names(window) == ["Metal", "  Zinc", "Building"]
    assert window.organizer.drop(["textures/metal"], "textures/building", None)
    assert names(window) == ["Building", "  Metal", "    Zinc"]
    assert project.entity("e1").components["material"]["textures"]["base_color"] == \
        "tex:library/textures/building/metal/zinc/zinc.png"
    assert window.selected() is None  # (the collection moved is what is chosen)
    assert window.tree.currentItem().text(0) == "Metal"


def test_a_drop_on_a_type_moves_a_sound_and_shows_it_there(window):
    show(window, "effects")
    window.types.dropped.emit(["effects/nox_footsteps/step"], "ambiance")
    assert window.current_type() == "ambiance"
    assert window.selected().name == "Step" and window.selected().type == "ambiance"
    assert step_sound(window.project) == "snd:library/ambiance/step/step.ogg"


def test_a_sound_shown_in_the_player_can_still_be_moved(window):
    show(window, "effects")
    choose(window, "effects/nox_footsteps/step")
    assert window.selected().name == "Step"
    window.said["text"] = "Walk"
    window.fix_scripts = False
    assert window.organizer.rename(), window.said["told"]
    assert window.selected().name == "Walk"


def test_undo_is_cleared_after_organizing(window):
    from midgard_editor import undo
    project = window.project
    project.undo_stack.push(undo.SetConfigCommand(project, "splash", "x", "splash"))
    show(window, "textures")
    choose(window, "textures/metal/zinc")
    window.said["text"] = "Grey Zinc"
    assert window.organizer.rename(), window.said["told"]  # (a texture shown: let go)
    assert project.undo_stack.count() == 0
