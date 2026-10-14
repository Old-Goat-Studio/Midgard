# SPDX-License-Identifier: Apache-2.0
import json
import sqlite3

import pytest

from midgard_editor import backup, contract
from midgard_editor.project import GAME_TITLE_KEY, Project, ProjectError
from midgard_editor.undo import SetConfigCommand, SetStringCommand


def test_create_makes_config_database_and_title(project):
    assert project.config_path.name == "HelloCube.config"
    config = json.loads(project.config_path.read_text("utf-8"))
    assert config["world_mode"] == "system_map"
    assert config["max_players"] == 4
    assert project.schema_version() == len(contract.migrations())
    assert project.string(GAME_TITLE_KEY) == "Hello Cube"
    assert (project.root / ".gitignore").read_text("utf-8") == ".midgard/\n"


@pytest.mark.parametrize("name", ["", "1Game", "my game", "a/b", "x" * 65])
def test_bad_names_are_refused(tmp_path, qapp, name):
    with pytest.raises(ProjectError):
        Project.create(tmp_path, name, "system_map", "")


def test_a_non_empty_folder_is_refused(tmp_path, qapp):
    (tmp_path / "Taken").mkdir()
    (tmp_path / "Taken" / "file.txt").write_text("x", "utf-8")
    with pytest.raises(ProjectError, match="not empty"):
        Project.create(tmp_path, "Taken", "system_map", "")


def test_open_round_trips(project):
    reopened = Project.open(project.config_path)
    assert reopened.name == "HelloCube"
    assert reopened.game_title == "Hello Cube"


@pytest.mark.parametrize("content, message", [
    ("{broken", "not valid JSON"),
    ('{"format": 99}', "not a Midgard project"),
])
def test_bad_configs_are_reported(tmp_path, qapp, content, message):
    config = tmp_path / "Bad.config"
    config.write_text(content, "utf-8")
    with pytest.raises(ProjectError, match=message):
        Project.open(config)


def test_missing_database_is_reported(project):
    project.db_path.unlink()
    with pytest.raises(ProjectError, match="missing"):
        Project.open(project.config_path)


def test_newer_database_is_reported(project):
    con = sqlite3.connect(project.db_path)
    con.execute("PRAGMA user_version = 999")
    con.close()
    with pytest.raises(ProjectError, match="newer than this editor"):
        Project.open(project.config_path)


def test_config_edits_undo_and_redo(project):
    project.undo_stack.push(SetConfigCommand(project, "max_players", 16, "max players"))
    assert json.loads(project.config_path.read_text("utf-8"))["max_players"] == 16
    project.undo_stack.undo()
    assert json.loads(project.config_path.read_text("utf-8"))["max_players"] == 4
    project.undo_stack.redo()
    assert project.config["max_players"] == 16


def test_string_edits_undo_and_redo(project):
    project.undo_stack.push(SetStringCommand(project, GAME_TITLE_KEY, "Cube Quest", "title"))
    assert project.game_title == "Cube Quest"
    project.undo_stack.undo()
    assert project.game_title == "Hello Cube"


def test_undoing_a_new_string_removes_it(project):
    project.undo_stack.push(SetStringCommand(project, "item.sword", "Sword", "item name"))
    assert project.string("item.sword") == "Sword"
    project.undo_stack.undo()
    assert project.string("item.sword") is None


def test_backups_rotate(project):
    for _ in range(4):
        backup.make_backup(project, keep=3)
    copies = backup.list_backups(project)
    assert len(copies) == 3
    for db in copies:
        assert db.with_suffix(".config").is_file()
        con = sqlite3.connect(db)
        assert con.execute("SELECT english FROM string WHERE key = ?",
                           (GAME_TITLE_KEY,)).fetchone()[0] == "Hello Cube"
        con.close()


def test_scheduler_backs_up_on_open_and_only_after_edits(project):
    scheduler = backup.BackupScheduler(project, keep=10, minutes=10)
    assert len(backup.list_backups(project)) == 1
    assert scheduler.backup_if_needed() is None
    project.undo_stack.push(SetConfigCommand(project, "max_players", 8, "max players"))
    assert scheduler.backup_if_needed() is not None
    assert len(backup.list_backups(project)) == 2
