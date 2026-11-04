# SPDX-License-Identifier: Apache-2.0
"""The World Tour sample (milestone 5.18 step 1): a hub and one world of each shape, each the
engine's own sample world type at its own size and seed."""
import json

import pytest

from midgard_editor import engine, samples, scenes, world_tour
from midgard_editor.cook import cook
from midgard_editor.project import MAIN_SCENE

needs_engine = pytest.mark.skipif(
    engine.module() is None or not hasattr(engine.module(), "sample_world_types"),
    reason="needs the engine module (its sample world types)")


@pytest.fixture(scope="module")
def tour(tmp_path_factory, qapp):
    if engine.module() is None:
        pytest.skip("needs the engine module")
    return world_tour.make_world_tour(tmp_path_factory.mktemp("tour"))


def by_name(project, scene):
    return {e.name: e for e in project.entities(scene)}


@needs_engine
def test_a_hub_and_a_world_of_each_shape_at_the_samples_sizes(tour):
    names = {s["id"]: s["name"] for s in scenes.load_all(tour)}
    assert set(names.values()) == {"Main", "Archipelago", "Habitat", "Planet", "Hollow World"}
    samples_by_id = {s["id"]: s for s in engine.module().sample_world_types()}
    shapes = set()
    for scene, name in names.items():
        if scene == MAIN_SCENE:
            continue
        settings = tour.scene_settings(scene)
        sample = samples_by_id[settings["terrain"]["world_type"]]
        body = json.loads(sample["body"])
        assert settings["terrain"]["seed"] == body["seed"]
        for key in ("shape", "radius", "length", "width"):
            assert settings["body"].get(key) == body.get(key), (name, key)
        shapes.add(settings["body"]["shape"])
        assert settings["weather"]  # (each its own)
        # The arrival stands on the ground, above the sea, with the pad home behind it.
        entities = by_name(tour, scene)
        arrival = entities["From the Hub"].components["transform"]["position"]
        pad = entities["Pad Home"].components
        assert arrival[1] >= world_tour.ABOVE_SEA + 1.0
        assert pad["scene_exit"]["scene"] == MAIN_SCENE
        assert abs(pad["transform"]["position"][1] - arrival[1]) < 0.01
        assert entities["Sun"].components["light"]["follows_clock"] is True
    assert shapes == {"bounded_area", "curved_cylinder", "sphere_outside", "sphere_inside"}
    assert tour.scene_settings(scenes.load_all(tour)[0]["id"]) is not None


@needs_engine
def test_every_door_and_pad_leads_to_an_arrival_there(tour):
    arrivals = {}
    for s in scenes.load_all(tour):
        for e in tour.entities(s["id"]):
            if "arrival" in e.components:
                arrivals.setdefault(s["id"], set()).add(e.id)
    exits = 0
    for s in scenes.load_all(tour):
        for e in tour.entities(s["id"]):
            leads = e.components.get("scene_exit")
            if not leads:
                continue
            exits += 1
            target = s["id"] if leads["scene"] == "@space" else leads["scene"]
            assert leads["arrival"] in arrivals[target], (s["name"], e.name)
    assert exits == 4 + 4 + 2  # (four doors, four pads home, the dock and the exit site)
    planet = next(s["id"] for s in scenes.load_all(tour) if s["name"] == "Planet")
    assert tour.scene_settings(planet)["ceiling"] == world_tour.PLANET_CEILING
    assert "player.flying = True" in (tour.root / "scripts" / "launch_pad.py").read_text("utf-8")
    assert by_name(tour, planet)["Launch Pad"].components["script"]["module"] == "launch_pad"


@needs_engine
def test_the_tour_cooks_and_the_engine_loads_it(tour, tmp_path):
    result = cook(tour, tmp_path / "pkg")  # (the engine's own check: raises if it can't load)
    # (The hollow world's sample ground is a little over the streaming budget: step 4.)
    assert [w for w in result.warnings if "costs" not in w] == []


def test_a_sample_gets_a_name_not_taken(tmp_path):
    assert samples.free_name(tmp_path, "WorldTour") == "WorldTour"
    (tmp_path / "WorldTour").mkdir()
    (tmp_path / "WorldTour2").mkdir()
    assert samples.free_name(tmp_path, "WorldTour") == "WorldTour3"
    assert [label for label, *_ in samples.samples()] == ["World Tour", "Scene Tour"]


@needs_engine
def test_file_new_sample_project_makes_and_opens_it(window, settings, tmp_path):
    settings.set("projects_dir", str(tmp_path / "projects"))
    project = window.new_sample(world_tour.TOUR)
    assert project is not None and project.name == "WorldTour"
    assert project.root == tmp_path / "projects" / "WorldTour"
    again = window.new_sample(world_tour.TOUR)
    assert again.name == "WorldTour2"
    window.new_sample(samples.TOUR)
    assert window.project.name == "SceneTour"
