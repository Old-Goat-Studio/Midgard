# SPDX-License-Identifier: Apache-2.0
"""Milestone 5.17's exit: someone writes a small shader with settings, feeds it a texture from
the texture generator, uses it on two objects with different settings, and the game draws
them; then, while the game plays, they change the shader and the game changes with it."""
import subprocess

import pytest
from PySide6.QtGui import QImage

from midgard_editor import about, engine, surface_shaders
from midgard_editor import texture_gen as tg
from midgard_editor.cook import cook
from midgard_editor.live_link import PlaySession, State
from midgard_editor.scene_model import Entity

pytestmark = pytest.mark.skipif(
    engine.module() is None or not hasattr(engine.module(), "compile_surface_shader") or
    not about.runtime_candidates(), reason="needs the engine module and the runtime")

SHADER = """// Glowing tiles: a picture (the pattern) lit by its own colour.
setting texture pattern;                     // Pattern
setting color tint = vec3(1.0, 1.0, 1.0);    // Glow colour
setting float strength = 1.5;                // Strength, 0 .. 5

void surface(SurfaceInput i, inout Surface s) {
    s.color = vec3(0.0);
    s.emission = texture(pattern, i.uv).rgb * tint * strength;
}
"""


def box(project, entity_id: str, position: list[float], settings: dict) -> None:
    project.put_entity(Entity(entity_id, f"Box {entity_id}", None, {
        "transform": {"position": position, "scale": [1.2, 1.2, 1.2]},
        "mesh": {"asset": "prim_cube"},
        "material": {"color": [1.0, 1.0, 1.0, 1.0], "shader": "shd:tiles",
                     "shader_settings": settings}}))


def screenshot(package, path) -> QImage:
    run = subprocess.run([str(about.runtime_candidates()[0]), "--package", str(package),
                          "--screenshot", str(path), "--size", "480x270"],
                         capture_output=True, text=True, timeout=90)
    if run.returncode != 0 and "Vulkan" in (run.stderr + run.stdout):
        pytest.skip("no Vulkan driver available: " + run.stderr.strip())
    assert run.returncode == 0, run.stderr
    return QImage(str(path))


def counts(image: QImage, x0: int, x1: int) -> dict[str, int]:
    """Strongly green, blue and dark pixels in a band of columns."""
    out = {"green": 0, "blue": 0, "dark": 0}
    for y in range(0, image.height(), 2):
        for x in range(x0, x1, 2):
            c = image.pixelColor(x, y)
            if c.green() > c.red() + 70 and c.green() > c.blue() + 70:
                out["green"] += 1
            if c.blue() > c.red() + 70 and c.blue() > c.green() + 50:
                out["blue"] += 1
            if max(c.red(), c.green(), c.blue()) < 40:
                out["dark"] += 1
    return out


def test_the_exit_a_shader_of_your_own_in_the_game_and_changed_while_it_plays(
        project, qtbot, tmp_path):
    # A checker from the texture generator, and a shader that glows with it.
    made = tg.save(project.root, "tiles", {**tg.PRESETS["Tiles"], "size": 256, "scale": 4,
                                           "colors": [[0.0, [0.0, 0.0, 0.0]],
                                                      [1.0, [1.0, 1.0, 1.0]]]})
    pattern = "tex:" + made[0].relative_to(project.root).as_posix()
    surface_shaders.new_shader(project.root, "tiles", SHADER)
    assert surface_shaders.compile_shader(project.root, "shd:tiles").ok
    # One shader, two materials: green tiles on the left, blue on the right (side by side
    # in front of the new project's camera, at (4, 3, 6) looking at the middle).
    box(project, "e000060", [1.2, 1.2, 3.6], {"pattern": pattern, "tint": [0.0, 1.0, 0.0]})
    box(project, "e000061", [2.8, 1.2, 2.5], {"pattern": pattern, "tint": [0.0, 0.3, 1.0]})
    result = cook(project, tmp_path / "pkg")
    assert not [w for w in result.warnings if "shader" in w or "texture" in w], result.warnings

    image = screenshot(result.out_dir, tmp_path / "shot.png")
    w = image.width()
    left, right = counts(image, 0, w // 2), counts(image, w // 2, w)
    assert left["green"] > 150 and left["blue"] < 10, left
    assert right["blue"] > 150 and right["green"] < 10, right
    assert left["dark"] > 150  # (the checker's dark squares, from the generated texture)

    # While the game plays, the shader changes: the game takes the new one at once.
    session = PlaySession(project)
    received = []
    session.message_received.connect(received.append)
    try:
        session.start(about.runtime_candidates()[0], result.out_dir)
        qtbot.waitUntil(lambda: session.state == State.PLAYING, timeout=20000)
        path = surface_shaders.source_path(project.root, "shd:tiles")
        path.write_text(SHADER.replace("* strength;", "* strength * 0.5 + vec3(0.2);"),
                        encoding="utf-8")
        session.files_saved()
        qtbot.waitUntil(lambda: any(m.get("text") == "applied asset.changed shd:tiles"
                                    for m in received), timeout=10000)
        assert not [m for m in received if m.get("level") in ("warn", "error")], received
    finally:
        session.stop()
