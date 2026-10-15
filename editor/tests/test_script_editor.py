# SPDX-License-Identifier: Apache-2.0
import sqlite3

import pytest

from midgard_editor import scripting
from midgard_editor.cook import cook
from midgard_editor.docks.entity_form import EntityForm
from midgard_editor.docks.script_editor import ScriptEditorPanel


# ---- helpers (no Qt) ----------------------------------------------------------------------------
def test_scripts_are_created_and_found(project):
    assert scripting.modules(project) == {}
    assert scripting.new_module_name(project, "Front Door!") == "front_door"
    file = scripting.create_script(project, "front_door", "The door.")
    assert file == project.root / "scripts" / "front_door.py"
    assert "def on_start(entity)" in file.read_text("utf-8")
    assert scripting.new_module_name(project, "front door") == "front_door_2"
    assert scripting.new_module_name(project, "9 lives") == "script_9_lives"
    (project.root / "scripts" / "enemies").mkdir()
    (project.root / "scripts" / "enemies" / "wolf.py").write_text("", "utf-8")
    assert set(scripting.modules(project)) == {"front_door", "enemies.wolf"}
    assert scripting.module_for(project, project.root / "scripts" / "enemies" / "wolf.py") == \
        "enemies.wolf"
    assert scripting.module_file(project, "enemies.wolf").name == "wolf.py"


def test_syntax_checks():
    assert scripting.check_syntax("x = 1\n") is None
    line, message = scripting.check_syntax("def f(:\n    pass\n")
    assert line == 1 and "SyntaxError" in message


def test_completion_knows_the_midgard_api(project):
    names = {c.name: c for c in scripting.completions("import midgard\nmidgard.fi", 2, 10)}
    assert names["find"].insert == "nd"
    assert "Entity" in names["find"].signature
    source = "import midgard\ne = midgard.player()\ne.col"
    assert [c.name for c in scripting.completions(source, 3, 5, project=project)] == ["color"]


def test_snippet_libraries(project, settings):
    assert any(s.name == "On trigger enter" for s in scripting.BUILTIN_SNIPPETS)
    for snippet in scripting.BUILTIN_SNIPPETS:  # the starter set is valid Python
        assert scripting.check_syntax(snippet.body) is None, snippet.name
    mine = scripting.add_user_snippet(settings, "Greet", "midgard.log('hi')\n", "debug")
    ours = scripting.add_project_snippet(project, "Open gate", "gate.position = (0, 3, 0)\n")
    everything = scripting.all_snippets(project, settings)
    assert {(s.name, s.source) for s in everything} >= {("Greet", "user"),
                                                        ("Open gate", "project")}
    scripting.delete_user_snippet(settings, mine.id)
    scripting.delete_project_snippet(project, ours.id)
    assert not [s for s in scripting.all_snippets(project, settings) if s.source != "built-in"]


def test_the_game_never_gets_the_snippet_table(project, tmp_path):
    scripting.add_project_snippet(project, "Open gate", "pass\n")
    package = cook(project, tmp_path / "pkg").out_dir
    with sqlite3.connect(package / "HelloCube.db") as con:
        tables = {r[0] for r in con.execute("SELECT name FROM sqlite_master WHERE type='table'")}
    con.close()
    assert "snippet" not in tables and "entity" in tables


# ---- the Script editor panel --------------------------------------------------------------
def type_text(editor, text: str) -> None:
    """Replaces the text as typing would (setPlainText doesn't count as an edit)."""
    editor.selectAll()
    editor.insertPlainText(text)


def test_editing_saving_and_marking_errors(qtbot, project, settings):
    panel = ScriptEditorPanel(settings)
    qtbot.addWidget(panel)
    panel.set_project(project)
    editor = panel.open_module("door")  # a new module is created from the template
    assert editor is not None and (project.root / "scripts" / "door.py").is_file()
    type_text(editor, "import midgard\n\ndef on_start(entity)\n    pass\n")
    assert panel.tabs.tabText(0) == "door.py*"
    panel.save_current()
    assert panel.tabs.tabText(0) == "door.py"
    assert editor.error_line == 3 and "line 3" in panel.status.text()
    type_text(editor, "import midgard\n\ndef on_start(entity):\n    pass\n")
    panel.save_current()
    assert editor.error_line == 0
    assert (project.root / "scripts" / "door.py").read_text("utf-8").startswith("import midgard")

    # An error from the running game opens the file at its line.
    panel.close_tab(0)
    panel.show_error("door.py", 4, "Traceback ...\nNameError: name 'x' is not defined\n")
    assert panel.current().path.name == "door.py" and panel.current().error_line == 4
    assert "NameError" in panel.status.text()


def test_snippets_are_inserted_at_the_cursors_indentation(qtbot, project, settings):
    panel = ScriptEditorPanel(settings)
    qtbot.addWidget(panel)
    panel.set_project(project)
    editor = panel.open_module("gate")
    editor.setPlainText("def on_start(entity):\n    ")
    cursor = editor.textCursor()
    cursor.movePosition(cursor.MoveOperation.End)
    editor.setTextCursor(cursor)
    panel.insert_snippet(scripting.Snippet("two", "a = 1\nb = 2\n"))
    assert editor.toPlainText() == "def on_start(entity):\n    a = 1\n    b = 2"


# ---- the Inspector's Script and Trigger rows ----------------------------------------------------
def test_adding_a_script_and_a_trigger(qtbot, project):
    form = EntityForm(project, "e000004")
    qtbot.addWidget(form)
    opened, rebuilt = [], []
    form.open_script.connect(opened.append)
    form.rebuild.connect(rebuilt.append)
    assert form.script is None and form.add_component.isEnabled()

    form.add("script")
    assert project.entity("e000004").components["script"] == {"module": "cube"}
    assert (project.root / "scripts" / "cube.py").is_file()
    assert opened == ["cube"] and rebuilt == ["e000004"]

    form = EntityForm(project, "e000004")  # as the Inspector rebuilds it
    qtbot.addWidget(form)
    assert form.script.currentData() == "cube"
    form.add("trigger")
    form = EntityForm(project, "e000004")
    qtbot.addWidget(form)
    form.trigger.boxes[0].setValue(3.0)
    assert project.entity("e000004").components["trigger"]["size"] == [3.0, 1.0, 1.0]
    assert not form.add_component.isEnabled()  # nothing left to add

    form.remove_component("script")
    assert "script" not in project.entity("e000004").components
    project.undo_stack.undo()
    assert project.entity("e000004").components["script"] == {"module": "cube"}


def test_the_inspector_opens_scripts_in_the_script_editor(qtbot, settings, project):
    from midgard_editor.main_window import MainWindow

    window = MainWindow(settings)
    qtbot.addWidget(window)
    window.set_project(project)
    window.inspector.show_entity("e000004")
    window.inspector.entity_form.add("script")
    assert window.script_editor.current().path.name == "cube.py"
    window._script_error({"file": "cube.py", "line": 2, "message": "boom"})
    assert window.script_editor.current().error_line == 2


def test_colours_follow_the_theme(qtbot, project, settings):
    """Themes colour the text area (not the app palette); the gutter and highlighting follow."""
    from PySide6.QtWidgets import QApplication

    panel = ScriptEditorPanel(settings)
    qtbot.addWidget(panel)
    panel.set_project(project)
    editor = panel.open_module("door")
    panel.show()
    assert editor._colours()[2] is False and editor.highlighter.dark is False
    QApplication.instance().setStyleSheet(
        "QPlainTextEdit { background-color: #1e1e1e; color: #dddddd; }")
    try:
        qtbot.waitUntil(lambda: editor.highlighter.dark is True, timeout=3000)
        base, text, dark = editor._colours()
        assert dark and base.name() == "#1e1e1e" and text.name() == "#dddddd"
    finally:
        QApplication.instance().setStyleSheet("")


# ---- Git change markers -------------------------------------------------------------------------
def test_line_changes_against_the_last_commit():
    base = scripting.GitBase("a\nb\nc\nd\n")
    assert scripting.line_changes(base, "a\nb\nc\nd\n") == {}
    assert scripting.line_changes(base, "a\nB\nc\nd\n") == {2: scripting.MODIFIED}
    assert scripting.line_changes(base, "a\nb\nnew\nc\nd\n") == {3: scripting.ADDED}
    assert scripting.line_changes(base, "a\nd\n") == {2: scripting.DELETED}
    assert scripting.line_changes(scripting.GitBase(None), "x\ny") == {1: scripting.ADDED,
                                                                         2: scripting.ADDED}
    assert scripting.line_changes(None, "anything") == {}  # not in a repository


def git(folder, *args):
    import subprocess

    subprocess.run(["git", "-c", "user.name=Midgard Tests", "-c", "user.email=tests@example.com",
                    *args], cwd=folder, check=True, capture_output=True)


@pytest.fixture
def repo(project):
    import shutil

    if shutil.which("git") is None:
        pytest.skip("git is not installed")
    scripting.create_script(project, "door")
    git(project.root, "init", "-q")
    git(project.root, "add", "scripts/door.py")
    git(project.root, "commit", "-q", "-m", "door")
    return project


def test_git_markers_in_the_script_editor(qtbot, repo, settings):
    folder = repo.root / "scripts"
    assert scripting.committed(folder / "door.py").text.startswith('"""The door script')
    assert scripting.committed(folder / "gate.py").text is None  # not committed yet
    assert scripting.file_statuses(folder) == {}

    panel = ScriptEditorPanel(settings)
    qtbot.addWidget(panel)
    panel.set_project(repo)
    editor = panel.open_module("door")
    assert editor.git_changes == {}
    cursor = editor.textCursor()
    cursor.movePosition(cursor.MoveOperation.End)
    editor.setTextCursor(cursor)
    editor.insertPlainText("\nmidgard.log('hi')\n")
    qtbot.waitUntil(lambda: scripting.ADDED in editor.git_changes.values(), timeout=3000)

    panel.save_current()
    gate = panel.open_module("gate")  # a new, uncommitted script
    assert set(gate.git_changes.values()) == {scripting.ADDED}
    assert scripting.file_statuses(folder) == {(folder / "door.py").resolve(): "modified",
                                               (folder / "gate.py").resolve(): "new"}
    panel._fill_open_menu()
    labels = [a.text() for a in panel.open_menu.actions()]
    assert "door   (changed)" in labels and "gate   (new)" in labels


def test_no_markers_outside_a_repository(project):
    file = scripting.create_script(project, "door")
    assert scripting.committed(file) is None
    assert scripting.file_statuses(file.parent) == {}
