# SPDX-License-Identifier: Apache-2.0
"""Environments (milestone 5.8): every project has the Earth-like one, the Sky links to one from
the Inspector, and the editor's view draws its sky through the day."""
import json

import pytest

from midgard_editor import engine, environments

SKY = "e000005"  # the new scene's Sky entity


def test_every_project_has_the_earth_like_environment(project):
    earth = environments.load(project, "env_earth")
    assert earth == {"id": "env_earth", "name": "Earth-like", "curves": {}, "settings": {}}


def test_the_inspector_links_the_sky_to_an_environment(qtbot, project):
    from midgard_editor.docks.entity_form import EntityForm

    form = EntityForm(project, SKY)
    qtbot.addWidget(form)
    assert form.environment.currentData() == ""  # new projects: flat colours
    form.environment.setCurrentIndex(form.environment.findData("env_earth"))
    assert project.entity(SKY).components["sky"]["environment"] == "env_earth"
    project.undo_stack.undo()
    assert "environment" not in project.entity(SKY).components["sky"]
    assert form.environment.currentData() == ""  # the form follows Undo


@pytest.mark.skipif(engine.module() is None, reason="engine module not built")
def test_the_editors_view_draws_the_environments_sky_through_the_day(project):
    view = engine.module().Viewport()
    try:
        view.attach_offscreen(64, 48)
    except RuntimeError:
        pytest.skip("no offscreen rendering here")
    for env in environments.load_all(project):
        view.set_environment(env["id"], env["name"], json.dumps(env["curves"]),
                             json.dumps(env["settings"]))
    view.put_entity(json.dumps({"id": "sun", "name": "Sun", "components": {
        "transform": {"rotation": [-60.0, 30.0, 0.0]}, "light": {"type": "directional"}}}))
    view.put_entity(json.dumps({"id": "sky", "name": "Sky", "components": {
        "sky": {"color": [0.5, 0.0, 0.5], "environment": "env_earth"}}}))
    view.set_camera([0.0, 1.0, 0.0], [0.0, 2.0, -5.0], 60.0)  # looking up at the sky

    def sky_rgb(phase):
        view.solar_phase = phase
        view.render()
        pixels = view.read_pixels()
        n = len(pixels) // 4
        return [sum(pixels[i + c] for i in range(0, len(pixels), 4)) / n for c in range(3)]

    flat = sky_rgb(None)  # no clock: the flat colour (purple)
    noon = sky_rgb(0.25)
    midnight = sky_rgb(0.75)
    assert flat[0] > flat[1] + 50 and flat[2] > flat[1] + 50
    assert noon[2] > noon[0] and sum(noon) > 2 * sum(midnight)  # blue by day, dark at night
