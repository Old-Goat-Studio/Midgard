# SPDX-License-Identifier: Apache-2.0
"""The Asset Library's exit (milestone 5.13a): Midgard's CC0 samples and a CC BY texture brought
into a project's library (a non-commercial one refused), used in a scene (the Superhero walking
with the library's animations, the Avocado, a floor in the CC BY texture), cooked and run by the
engine, with the game's credits made from the library."""
import pytest
from test_asset_window import a_picture
from test_scenes import run_game

from midgard_editor import asset_library as lib
from midgard_editor import engine
from midgard_editor.cook import cook
from midgard_editor.scene_model import Entity

needs_engine = pytest.mark.skipif(engine.module() is None, reason="engine module not built")


@needs_engine
def test_the_exit_samples_and_a_cc_by_texture_in_a_game_with_its_credits(
        project, qtbot, tmp_path, settings):
    from midgard_editor.docks.project_page import ProjectPage
    from midgard_editor.main_window import MainWindow
    # The samples, from the Assets menu (once: again, they are there already).
    window = MainWindow(settings)
    qtbot.addWidget(window)
    window.set_project(project)
    added, left = window.add_sample_assets()
    assert [a.name for a in added] == ["Superhero", "Universal Animation Library", "Avocado"]
    assert left == []
    assert all(a.licence == "CC0-1.0" and a.author for a in added)
    assert added[0].author == "Quaternius" and added[2].author == "Microsoft"
    assert window.asset_window.isVisible()
    assert "3 assets" in window.asset_window.title.text()
    again, left = window.add_sample_assets()
    assert again == [] and len(left) == 3 and all("already" in why for why in left)
    window.asset_window.close()

    # A CC BY texture (and a non-commercial one refused).
    plates = lib.import_asset(project, [a_picture(tmp_path, "plates.png")], "textures",
                              "Rusty Plates", category="metal", licence="CC-BY-4.0",
                              author="Ana Ruiz", source="https://example.org/rusty",
                              title="Rusty Plates")
    with pytest.raises(lib.LibraryError, match="isn't allowed"):
        lib.import_asset(project, [a_picture(tmp_path, "nc.png")], "textures", "NC",
                         category="metal", licence="CC-BY-NC-4.0", author="X", title="NC")

    # A scene of them.
    hero = "lib:models/superhero/superhero_male.glb"
    moves = "lib:models/universal_animation_library/animation_library.glb"
    assert {hero, moves, "lib:models/avocado/Avocado.gltf"} <= set(project.mesh_assets())
    project.put_entity(Entity(project.next_entity_id(), "Hero", None, {
        "transform": {"position": [2.0, 0.0, 0.0]},
        "character": {"model": hero, "clips": moves, "clip": "Walk_Loop"},
        "material": {"textures": {
            "base_color": "tex:library/models/superhero/superhero_male_basecolor.png"}}}))
    project.put_entity(Entity(project.next_entity_id(), "Avocado", None, {
        "transform": {"position": [-2.0, 0.5, 0.0], "scale": [10, 10, 10]},
        "mesh": {"asset": "lib:models/avocado/Avocado.gltf"}}))
    project.put_entity(Entity(project.next_entity_id(), "Floor", None, {
        "transform": {"position": [0.0, -0.05, 0.0], "scale": [8, 0.1, 8]},
        "mesh": {"asset": "prim_cube"},
        "material": {"textures": {
            "base_color": "tex:library/textures/metal/rusty_plates/plates.png"}}}))

    # Cooked: the library's files in the game, its credits made.
    result = cook(project, tmp_path / "pkg")
    content = {c["id"] for c in result.manifest["content"]}
    assert {hero, moves, "lib:models/avocado/Avocado.gltf"} <= content
    assert not [w for w in result.warnings if "Library" in w or "not in the project" in w], \
        result.warnings
    assert result.credits == [lib.credit(plates)]
    credits = (result.out_dir / "CREDITS.txt").read_text("utf-8")
    assert lib.credit(plates) in credits
    assert "by Quaternius" in credits and "by Microsoft" in credits  # (CC0: thanked)
    page = ProjectPage()
    qtbot.addWidget(page)
    page.set_project(project)
    assert page.credits.text() == lib.credit(plates)

    # Run by the engine: the hero walks, with the library's animations.
    dumped = run_game(result.out_dir, 30)
    character = next(e for e in dumped["entities"] if e["name"] == "Hero")["character"]
    assert character["state"] == "Walk_Loop"
    assert {"hips", "head", "left_foot", "right_hand"} <= set(character["bones"])
