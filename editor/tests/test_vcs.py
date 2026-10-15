# SPDX-License-Identifier: Apache-2.0
"""Putting projects under Git (File ▸ Put Project Under Git…, and New Project's tick box)."""
import pytest

from midgard_editor import scripting, vcs

pytestmark = pytest.mark.skipif(not vcs.available(), reason="git is not installed")


@pytest.fixture
def no_git_identity(tmp_path, monkeypatch):
    """A machine where Git knows no name or email (like a fresh CI runner)."""
    empty = tmp_path / "gitconfig"
    empty.write_text("", "utf-8")
    monkeypatch.setenv("GIT_CONFIG_GLOBAL", str(empty))
    monkeypatch.setenv("GIT_CONFIG_NOSYSTEM", "1")


@pytest.fixture
def git_identity(tmp_path, monkeypatch):
    config = tmp_path / "gitconfig"
    config.write_text("[user]\n\tname = Midgard Tests\n\temail = tests@example.com\n", "utf-8")
    monkeypatch.setenv("GIT_CONFIG_GLOBAL", str(config))
    monkeypatch.setenv("GIT_CONFIG_NOSYSTEM", "1")


def test_a_project_goes_under_git(project, no_git_identity):
    script = scripting.create_script(project, "door")
    assert vcs.status_text(project) == (True, "Put Project Under &Git…")
    with pytest.raises(vcs.VcsError, match="name and email"):
        vcs.put_under_git(project)  # no identity, none given
    assert not (project.root / ".git").exists()  # nothing half-made is left behind

    commit = vcs.put_under_git(project, vcs.Identity("Ada Builder", "ada@example.com"))
    assert commit
    assert vcs.repository_root(project.root).resolve() == project.root.resolve()
    assert vcs.identity(project.root) == vcs.Identity("Ada Builder", "ada@example.com")
    assert scripting.committed(script).text.startswith('"""The door script')
    assert scripting.file_statuses(project.root) == {}  # everything was committed
    assert vcs.status_text(project) == (False, "Project Is in &Git")
    with pytest.raises(vcs.VcsError, match="already"):
        vcs.put_under_git(project)


def test_the_ignore_file_keeps_editor_files_out(project, git_identity):
    (project.root / ".midgard" / "assistant").mkdir(parents=True)
    (project.root / ".midgard" / "assistant" / "conversation.json").write_text("{}", "utf-8")
    (project.root / "build").mkdir()
    (project.root / "build" / "game.pak").write_bytes(b"pak")
    vcs.put_under_git(project)
    tracked = scripting._git(project.root, "ls-files").split()
    assert "HelloCube.db" in tracked and "HelloCube.config" in tracked
    assert not [f for f in tracked if f.startswith((".midgard/", "build/"))]


def test_new_project_can_start_in_git(qtbot, settings, tmp_path, git_identity):
    from midgard_editor.dialogs import NewProjectDialog

    dialog = NewProjectDialog(settings)
    qtbot.addWidget(dialog)
    assert dialog.git.isChecked()  # on by default when Git is installed
    dialog.name.setText("Northreach")
    dialog.location.setText(str(tmp_path / "projects"))
    dialog._create()
    assert dialog.project is not None
    assert vcs.repository_root(dialog.project.root) is not None
    assert settings.get("new_project_git") is True

    dialog = NewProjectDialog(settings)
    qtbot.addWidget(dialog)
    dialog.git.setChecked(False)
    dialog.name.setText("Southreach")
    dialog.location.setText(str(tmp_path / "other"))
    dialog._create()
    assert vcs.repository_root(dialog.project.root) is None
    assert NewProjectDialog(settings).git.isChecked() is False  # the choice is remembered


def test_the_file_menu_command(qtbot, settings, project, git_identity):
    from midgard_editor.main_window import MainWindow

    window = MainWindow(settings)
    qtbot.addWidget(window)
    window.set_project(project)
    window._update_git_action()
    assert window.git_action.isEnabled()
    window.put_under_git()
    assert vcs.repository_root(project.root) is not None
    assert not window.git_action.isEnabled() and "Is in" in window.git_action.text()


# ---- committing ---------------------------------------------------------------------------
@pytest.fixture
def in_git(project, git_identity):
    scripting.create_script(project, "door")
    scripting.create_script(project, "old")
    vcs.put_under_git(project)
    return project


def test_changed_files_and_committing_some_of_them(in_git):
    project = in_git
    folder = project.root / "scripts"
    (folder / "door.py").write_text("print('changed')\n", "utf-8")
    scripting.create_script(project, "gate")
    (folder / "old.py").unlink()
    files = {f.path: f.state for f in vcs.changed_files(project)}
    assert files == {"scripts/door.py": "modified", "scripts/gate.py": "new",
                     "scripts/old.py": "deleted"}
    assert vcs.suggest_message([vcs.ChangedFile("scripts/gate.py", "new")]) == "Add gate.py"
    assert vcs.suggest_message(vcs.changed_files(project)) == "Update 3 files"

    with pytest.raises(vcs.VcsError, match="message"):
        vcs.commit(project, ["scripts/door.py"], "  ")
    commit = vcs.commit(project, ["scripts/gate.py", "scripts/old.py"], "Add the gate")
    assert commit
    left = {f.path: f.state for f in vcs.changed_files(project)}
    assert left == {"scripts/door.py": "modified"}  # not ticked, so not committed
    log = scripting._git(project.root, "log", "-1", "--format=%s")
    assert log.strip() == "Add the gate"


def test_the_commit_dialog(qtbot, in_git):
    from midgard_editor.dialogs import CommitDialog

    project = in_git
    (project.root / "scripts" / "door.py").write_text("print('v2')\n", "utf-8")
    scripting.create_script(project, "gate")
    dialog = CommitDialog(project)
    qtbot.addWidget(dialog)
    assert dialog.list.count() == 2 and dialog.ticked() == ["scripts/door.py", "scripts/gate.py"]
    assert dialog.message.toPlainText() == "Update 2 files"
    dialog.list.item(1).setCheckState(dialog.list.item(1).checkState().Unchecked)
    dialog.message.setPlainText("")
    assert not dialog.commit_button.isEnabled()  # no message
    dialog.message.setPlainText("Door v2")
    assert dialog.commit_button.isEnabled()
    dialog._commit()
    assert dialog.commit_id
    assert [f.path for f in vcs.changed_files(project)] == ["scripts/gate.py"]


def test_the_commit_button_follows_the_project(qtbot, settings, project, git_identity,
                                               monkeypatch):
    from midgard_editor.dialogs import CommitDialog
    from midgard_editor.main_window import MainWindow

    window = MainWindow(settings)
    qtbot.addWidget(window)
    window.set_project(project)
    assert not window.script_editor.commit_button.isEnabled()  # not in Git yet
    window.put_under_git()
    assert window.script_editor.commit_button.isEnabled()
    assert window.commit_action.isEnabled()

    editor = window.script_editor.open_module("door")  # a new script, unsaved edits below
    editor.selectAll()
    editor.insertPlainText("print('door')\n")
    opened = []
    monkeypatch.setattr(CommitDialog, "exec", lambda self: opened.append(self.ticked()) or 0)
    window.script_editor.commit_button.click()
    assert opened == [["scripts/door.py"]]  # the script was saved first, then offered
    assert not editor.document().isModified()
