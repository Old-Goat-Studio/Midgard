# SPDX-License-Identifier: Apache-2.0
"""Milestone 5.11's exit: a world type built in the editor (here from a sample), previewed from
several seeds, and the game generates the same world: the preview's world hash is the one the
runtime reports. An old world still generates identically after its world type changes (its
version is shipped, so the change is saved as a new version)."""
import json
import subprocess

import pytest

from midgard_editor import engine, world_types
from midgard_editor.about import runtime_candidates
from midgard_editor.cook import cook
from midgard_editor.docks.world_type_editor import WorldTypePanel

needs_engine = pytest.mark.skipif(engine.module() is None, reason="engine module not built")


def runtime():
    return next((r for r in runtime_candidates() if r.is_file()), None)


needs_runtime = pytest.mark.skipif(runtime() is None, reason="no runtime built")


def world_hash(settings: dict) -> str:
    return engine.module().world_hash(json.dumps(settings))


def game_world_hash(project, tmp_path) -> str | None:
    """The world hash the game reports for the project's main scene."""
    package = cook(project, tmp_path / "pkg").out_dir
    run = subprocess.run([str(runtime()), "--package", str(package), "--simulate", "1",
                          "--dump-scene"], capture_output=True, text=True, timeout=120)
    assert run.returncode == 0, run.stderr
    lines = [line for line in run.stdout.splitlines() if line.startswith("{")]
    return json.loads(lines[-1])["world_hash"]


def use_world(project, body: dict, world_type: str, version: int, seed: int) -> None:
    settings = project.scene_settings()
    settings["body"] = {**body, "id": "world"}
    settings["terrain"] = {"world_type": world_type, "version": version, "seed": seed}
    project.set_scene_settings(settings)


@pytest.fixture
def panel(project, qtbot):
    p = WorldTypePanel()
    qtbot.addWidget(p)
    p.set_project(project)
    return p


@needs_engine
def test_every_sample_makes_its_world_and_seeds_make_others(panel, project):
    midgard = engine.module()
    seen = set()
    for sample in midgard.sample_world_types():
        made = panel.new_from_sample(sample["id"])
        assert world_types.recipe(project, made) == json.loads(sample["recipe"])
        body = json.loads(sample["body"])
        assert panel.preview_shape.currentData() == body["shape"]
        assert panel.seed.value() == body["seed"]
        panel.update_preview()
        assert panel.problems.count() == 0, [panel.problems.item(i).text()
                                             for i in range(panel.problems.count())]
        assert panel.map_image is not None
        for seed in (1, 2, 3):  # several seeds, several worlds
            panel.seed.setValue(seed)
            seen.add(world_hash(panel.preview_settings()))
    assert len(seen) == 3 * len(midgard.sample_world_types())


@needs_engine
@needs_runtime
def test_the_game_generates_the_world_the_preview_shows(panel, project, tmp_path):
    made = panel.new_from_sample("wt_sample_temperate_planet")
    panel.seed.setValue(7)
    preview = panel.preview_settings()
    use_world(project, preview["body"], made, 1, 7)
    # The editor's viewport sees the same (its settings are resolved from the world type).
    resolved = world_types.resolved_settings(project, project.scene_settings())
    assert world_hash(resolved) == world_hash(preview)
    assert game_world_hash(project, tmp_path) == world_hash(preview)


@needs_engine
@needs_runtime
def test_an_old_world_generates_the_same_after_its_world_type_changes(panel, project,
                                                                       tmp_path):
    made = panel.new_from_sample("wt_sample_archipelago")
    preview = panel.preview_settings()
    seed = preview["terrain"]["seed"]  # (the sample's own)
    use_world(project, preview["body"], made, 1, seed)
    before = game_world_hash(project, tmp_path / "before")
    assert before == world_hash(preview)
    world_types.mark_shipped(project)  # an export: version 1 is kept from now on
    # Lower the sea in the editor: saved as version 2.
    panel.ground_fields["sea_level"].setValue(-8)
    panel.ground_fields["sea_level"].editingFinished.emit()
    assert world_types.load_all(project)[0]["version"] == 2
    changed = panel.preview_settings()
    assert world_hash(changed) != before
    # The scene's world is still version 1: the same world, in the editor and the game.
    resolved = world_types.resolved_settings(project, project.scene_settings())
    assert world_hash(resolved) == before
    assert game_world_hash(project, tmp_path / "after") == before
    # A world made with version 2 is the changed one.
    use_world(project, changed["body"], made, 2, seed)
    assert game_world_hash(project, tmp_path / "v2") == world_hash(changed)
