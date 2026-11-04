# SPDX-License-Identifier: Apache-2.0
"""The Shader editor (milestone 5.17 step 4): starters, mistakes as you type, the live preview,
saving, and the Material editor's way in."""
import pytest
from PySide6.QtCore import Qt
from test_materials import add_box

from midgard_editor import engine, objects, shader_starters, surface_shaders
from midgard_editor.docks import shader_editor
from midgard_editor.docks.shader_editor import ShaderEditorPanel

needs_compiler = pytest.mark.skipif(
    engine.module() is None or not hasattr(engine.module(), "compile_surface_shader"),
    reason="needs the engine module with the surface shader compiler")


@needs_compiler
@pytest.mark.parametrize("starter", list(shader_starters.STARTERS))
def test_every_starter_compiles_and_has_settings(starter):
    _label, code = shader_starters.STARTERS[starter]
    result = surface_shaders.compile_code(code, starter)
    assert result.ok, result.messages
    assert result.messages == [], result.messages  # (not even a warning)
    assert result.settings


def test_completion_offers_what_a_surface_shader_has(qtbot, project):
    panel = ShaderEditorPanel()
    qtbot.addWidget(panel)
    panel.set_project(project)
    editor = panel.new_shader("blank", name="tint")
    editor.moveCursor(editor.textCursor().MoveOperation.End)
    editor.insertPlainText("s.")
    assert editor.words() == shader_editor.SURFACE_FIELDS
    editor.insertPlainText("\ni.")
    assert "sun" in editor.words() and "time" in editor.words()
    editor.insertPlainText("\nti")
    assert "tint" in editor.words()  # (its own setting)


def test_a_new_shader_is_a_copy_of_its_starter(qtbot, project):
    panel = ShaderEditorPanel()
    qtbot.addWidget(panel)
    panel.set_project(project)
    editor = panel.new_shader("glow", name="beacon")
    assert editor.path == project.root / "shaders" / "beacon.glsl"
    assert editor.path.read_text(encoding="utf-8") == shader_starters.GLOW
    again = panel.new_shader("glow", name="beacon")  # (never over another)
    assert again.path.name == "beacon 2.glsl"
    assert panel.tabs.count() == 2
    assert panel.open_shader("shd:beacon") is editor and panel.tabs.count() == 2


@needs_compiler
def test_mistakes_are_underlined_as_you_type_and_saving_tells_the_editor(qtbot, project):
    panel = ShaderEditorPanel()
    qtbot.addWidget(panel)
    panel.set_project(project)
    editor = panel.new_shader("blank", name="tint")
    assert panel.problems.item(panel.problems.count() - 1).text() == "The shader compiles."

    type_over(editor, editor.toPlainText().replace("s.color *= tint;", "s.colr *= tint;"))
    panel._compile_now()  # (as the timer would, a moment after typing)
    line = shader_starters.BLANK.splitlines().index("    s.color *= tint;") + 1
    assert line in editor.mistakes
    assert "colr" in editor.mistakes[line][0]
    first = panel.problems.item(0)
    assert first.text().startswith(f"line {line}: ")
    assert first.data(Qt.ItemDataRole.UserRole) == line
    assert "last version that worked" in panel.preview_note.text()
    panel._go_to_problem(first)
    assert editor.textCursor().blockNumber() + 1 == line

    saved = []
    panel.shader_saved.connect(saved.append)
    type_over(editor, shader_starters.BLANK.replace("vec3(1.0, 1.0, 1.0)",
                                                      "vec3(1.0, 0.5, 0.5)"))
    panel._compile_now()
    assert editor.mistakes == {}
    assert panel.tabs.tabText(0) == "tint *"
    panel.save_current()
    assert saved == ["shd:tint"]
    assert "vec3(1.0, 0.5, 0.5)" in editor.path.read_text(encoding="utf-8")
    assert panel.tabs.tabText(0) == "tint"


def type_over(editor, text: str) -> None:
    """All the code replaced, as typing would (an edit, unlike setPlainText)."""
    cursor = editor.textCursor()
    cursor.select(cursor.SelectionType.Document)
    cursor.insertText(text)


def _colour(panel) -> tuple[int, int, int]:
    """The preview's average colour at its middle."""
    image = panel.preview.pixmap().toImage()
    cx, cy = image.width() // 2, image.height() // 2
    total = [0, 0, 0]
    for dy in range(-6, 7, 3):
        for dx in range(-6, 7, 3):
            c = image.pixelColor(cx + dx, cy + dy)
            total = [total[0] + c.red(), total[1] + c.green(), total[2] + c.blue()]
    return tuple(t // 25 for t in total)


@needs_compiler
def test_the_preview_shows_the_code_as_it_is_and_moves_with_time(qtbot, project, monkeypatch):
    panel = ShaderEditorPanel()
    qtbot.addWidget(panel)
    panel.resize(900, 500)
    panel.set_project(project)
    panel.animate.setChecked(False)
    editor = panel.new_shader("blank", name="flat")
    code = """setting color glow = vec3(1.0, 0.0, 0.0);
void surface(SurfaceInput i, inout Surface s) {
    s.color = vec3(0.0);
    s.emission = glow * (0.5 + 0.5 * sin(i.time * 3.14159));
}
"""
    type_over(editor, code)
    panel._compile_now()
    pixmap = panel.preview.pixmap()
    if pixmap is None or pixmap.isNull():
        pytest.skip("no offscreen rendering here: " + panel.preview.text())
    red = _colour(panel)
    assert red[0] > red[1] + 60 and red[0] > red[2] + 60, red

    # The same shader, changed: drawn in place of the old (not another one beside it).
    type_over(editor, code.replace("vec3(1.0, 0.0, 0.0)", "vec3(0.0, 0.0, 1.0)"))
    panel._compile_now()
    blue = _colour(panel)
    assert blue[2] > blue[0] + 60, blue
    # A mistake: the last version that worked stays.
    type_over(editor, code.replace("s.color", "s.colour"))
    panel._compile_now()
    assert _colour(panel) == blue

    # With the time running, it pulses: dark at 1.5 s (sin = -1), bright at 0.5 s.
    type_over(editor, code)
    panel._compile_now()
    panel.animate.setChecked(True)
    monkeypatch.setattr(panel, "clock", lambda: 1.5)
    panel.render_preview()
    dark = _colour(panel)
    monkeypatch.setattr(panel, "clock", lambda: 0.5)
    panel.render_preview()
    bright = _colour(panel)
    assert bright[0] > dark[0] + 60, (dark, bright)
    objects.shared_renderer().view.clock = -1.0


def test_the_material_editor_opens_its_shader_or_makes_one(qtbot, project):
    from midgard_editor.docks.material_editor import EntityMaterial, MaterialEditor
    add_box(project)
    editor = MaterialEditor(project, EntityMaterial(project, "e000050"))
    qtbot.addWidget(editor)
    asked = []
    shader_editor.requests().open.connect(asked.append)
    try:
        editor.edit_shader()  # (no shader yet: a new one, used by the material)
        assert asked == ["shd:shader"]
        assert (project.root / "shaders" / "shader.glsl").is_file()
        assert project.entity("e000050").components["material"]["shader"] == "shd:shader"
        editor.edit_shader()
        assert asked == ["shd:shader", "shd:shader"]
    finally:
        shader_editor.requests().open.disconnect(asked.append)


def test_the_main_window_has_the_panel(qtbot, tmp_path, monkeypatch):
    from midgard_editor import main_window
    assert main_window.PANELS["shaders"] == "Shader Editor"
