# SPDX-License-Identifier: Apache-2.0
"""Characters' IK in the game (milestone 5.13 step 6): the Superhero's feet on what physics has
under them (one on a step), a hand reached by a script (which turns its head too); the player's
arms in first person; and the Inspector's IK and first-person settings."""
import math
import subprocess

import pytest
from test_characters import HERO, LIBRARY, with_samples
from test_scripting import by_name, needs_scripting, simulate, write_script

from midgard_editor import engine
from midgard_editor.cook import cook
from midgard_editor.scene_model import Entity

needs_engine = pytest.mark.skipif(engine.module() is None, reason="engine module not built")

STEP = 0.15
REACHER = """
import midgard

def on_update(entity, dt):
    x, y, z = entity.position
    entity.look_at((x + 1.0, y + 1.6, z + 1.0))       # 45 degrees to its left
    entity.reach("right", (x - 0.2, y + 1.15, z + 0.15))  # (0.4 m from the shoulder)
    try:
        entity.reach("middle", None)
    except ValueError as e:
        midgard.log("refused:", e)
"""


def add_hero(project, name: str, x: float, ik: dict, script: str = "") -> None:
    components = {"transform": {"position": [x, 0.0, 0.0]},
                  "character": {"model": HERO, "clips": LIBRARY, "clip": "Idle_Loop", "ik": ik}}
    if script:
        components["script"] = {"module": script}
    project.put_entity(Entity(project.next_entity_id(), name, None, components))


def add_box(project, name: str, centre, size) -> None:
    project.put_entity(Entity(project.next_entity_id(), name, None, {
        "transform": {"position": list(centre)}, "collider": {"shape": "box", "size": list(size)}}))


@needs_scripting
def test_feet_stand_on_a_step_and_a_script_reaches_a_hand_out(project, tmp_path):
    with_samples(project)
    # Flat ground far from the rest, and a 15 cm step under the stepping hero's left foot (+X).
    add_box(project, "Ground", (60.0, -0.5, 0.0), (40.0, 1.0, 20.0))
    add_box(project, "Step", (50.93, STEP / 2, 0.0), (1.8, STEP, 2.0))  # (from x 50.03)
    add_hero(project, "Plain", 70.0, {})
    add_hero(project, "Stepping", 50.0, {"feet": True})
    add_hero(project, "Reaching", 75.0, {}, "reacher")
    write_script(project, "reacher", REACHER)
    package = cook(project, tmp_path / "pkg").out_dir
    dumped, run = simulate(package, 90)  # (1.5 s: eased all the way in)
    assert run.returncode == 0, run.stderr
    assert "refused: a hand is 'left' or 'right'" in run.stdout
    plain = by_name(dumped, "Plain")["character"]["bones"]
    stepping = by_name(dumped, "Stepping")["character"]["bones"]
    assert by_name(dumped, "Plain")["character"]["state"] == "Idle_Loop"
    # The left foot up on the step; the right where it was; the hips lowered no lower than
    # they were (the step is under the higher foot).
    assert stepping["left_foot"][1] - plain["left_foot"][1] == pytest.approx(STEP, abs=0.02)
    assert stepping["right_foot"][1] == pytest.approx(plain["right_foot"][1], abs=0.02)
    assert stepping["hips"][1] == pytest.approx(plain["hips"][1], abs=0.02)
    # The script's: the right hand at the point (where the head turns: test_character_ik.cpp).
    reaching = by_name(dumped, "Reaching")["character"]["bones"]
    hand = reaching["right_hand"]
    assert math.dist(hand, (75.0 - 0.2, 1.15, 0.15)) < 0.02
    assert math.dist(plain["right_hand"], (70.0 - 0.2, 1.15, 0.15)) > 0.2  # (it reached)


@needs_engine
def test_the_inspector_sets_a_characters_ik(project, qtbot):
    from midgard_editor.docks.entity_form import EntityForm
    with_samples(project)
    add_hero(project, "Hero", 0.0, {})
    hero = next(e for e in project.entities() if e.name == "Hero")
    form = EntityForm(project, hero.id)
    qtbot.addWidget(form)
    assert not form.character_feet.isChecked()
    form.character_feet.setChecked(True)
    form.character_look.setCurrentIndex(form.character_look.findData("player"))
    form.character_look_limit.setValue(45.0)
    ik = project.entity(hero.id).components["character"]["ik"]
    assert ik == {"feet": True, "look": "player", "look_limit": 45.0}
    project.undo_stack.undo()
    assert "look_limit" not in project.entity(hero.id).components["character"]["ik"]


@needs_engine
def test_the_inspector_gives_the_player_first_person_arms(project, qtbot):
    from midgard_editor.docks.entity_form import EntityForm
    with_samples(project)
    add_hero(project, "Hero", 0.0, {})
    hero = next(e for e in project.entities() if e.name == "Hero")
    form = EntityForm(project, hero.id)
    qtbot.addWidget(form)
    assert not form.character_first_person.isChecked()
    form.character_first_person.setChecked(True)
    assert project.entity(hero.id).components["character"]["first_person"] == {}
    form.character_first_person.setChecked(False)
    assert "first_person" not in project.entity(hero.id).components["character"]


def first_person_shot(project, tmp_path, arms: bool):
    """The game's first frames in first person, the player the Superhero holding a pistol pose
    (its arms up in front of it), with or without first-person arms: the image's pixels."""
    from PIL import Image

    from midgard_editor import about
    for e in project.entities():
        if "player" in e.components:
            e.components["character"] = {"model": HERO, "clips": LIBRARY,
                                         "clip": "Pistol_Idle_Loop"}
            if arms:
                e.components["character"]["first_person"] = {}
            e.components.pop("mesh", None)
            e.components["material"] = {"color": [0.9, 0.6, 0.4, 1]}
            project.put_entity(e)
        if "camera" in e.components:
            e.components["camera"]["view"] = "first"
            project.put_entity(e)
    out = cook(project, tmp_path / f"pkg{arms}").out_dir
    shot = tmp_path / f"shot{arms}.png"
    run = subprocess.run([str(about.runtime_candidates()[0]), "--package", str(out),
                          "--screenshot", str(shot), "--size", "320x180", "--frames", "10"],
                         capture_output=True, text=True, timeout=60)
    if run.returncode != 0 and "Vulkan" in (run.stderr + run.stdout):
        pytest.skip("no Vulkan driver available: " + run.stderr.strip())
    assert run.returncode == 0, run.stderr
    return Image.open(shot).convert("RGB")


@needs_engine
def test_the_player_sees_its_arms_in_first_person(project, tmp_path):
    from midgard_editor import about
    if not about.runtime_candidates():
        pytest.skip("engine not built here")
    with_samples(project)

    def skin(image) -> int:  # the arms' colour, warm: red well above blue
        pixels = image.tobytes()  # (RGB)
        return sum(1 for i in range(0, len(pixels), 3)
                   if pixels[i] > 90 and pixels[i] > pixels[i + 2] + 50)

    without = skin(first_person_shot(project, tmp_path, arms=False))
    with_arms = skin(first_person_shot(project, tmp_path, arms=True))
    assert without < 50  # (its own body isn't drawn in first person)
    assert with_arms > 1500, "the arms are in view"
