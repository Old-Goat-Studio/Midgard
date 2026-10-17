# SPDX-License-Identifier: Apache-2.0
import json

from midgard_editor.settings import DEFAULTS, MAX_RECENT, Settings


def test_defaults_when_nothing_is_saved(settings):
    assert settings.get("theme") is None
    assert settings.get("backup_keep") == 10
    assert settings.get("recent_projects") == []


def test_values_persist_in_midgard_json(tmp_path):
    first = Settings(tmp_path)
    first.set("theme", "qss:yggdrasil")
    assert json.loads((tmp_path / "midgard.json").read_text("utf-8"))["theme"] == "qss:yggdrasil"
    assert Settings(tmp_path).get("theme") == "qss:yggdrasil"
    assert not (tmp_path / "midgard.json.tmp").exists()


def test_a_corrupt_file_falls_back_to_defaults(tmp_path):
    (tmp_path / "midgard.json").write_text("{broken", "utf-8")
    assert Settings(tmp_path).get("backup_minutes") == DEFAULTS["backup_minutes"]


def test_returned_defaults_are_copies(settings):
    settings.get("layouts")["x"] = 1
    assert DEFAULTS["layouts"] == {}


def test_recent_projects_are_newest_first_without_duplicates(settings, tmp_path):
    configs = []
    for i in range(MAX_RECENT + 2):
        path = tmp_path / f"p{i}.config"
        path.write_text("{}", "utf-8")
        configs.append(path)
        settings.add_recent(path)
    settings.add_recent(configs[5])
    recent = settings.recent_projects()
    assert recent[0].name == "p5.config"
    assert len(recent) == MAX_RECENT
    assert len({p.name for p in recent}) == MAX_RECENT


def test_missing_recent_projects_are_skipped(settings, tmp_path):
    gone = tmp_path / "gone.config"
    gone.write_text("{}", "utf-8")
    settings.add_recent(gone)
    gone.unlink()
    assert settings.recent_projects() == []


def test_the_last_project_opens_on_starting_unless_turned_off(qapp, settings, tmp_path):
    from midgard_editor.app import startup_project
    from midgard_editor.dialogs import PreferencesDialog

    assert settings.get("open_last_project") is True  # the default
    assert startup_project(settings, None) is None  # nothing worked on yet
    older, newer = tmp_path / "Old.config", tmp_path / "New.config"
    for config in (older, newer):
        config.write_text("{}", "utf-8")
        settings.add_recent(config)
    assert startup_project(settings, None) == newer
    assert startup_project(settings, older) == older  # the command line wins
    newer.unlink()  # moved or deleted since: the one before it
    assert startup_project(settings, None) == older

    dialog = PreferencesDialog(settings)
    assert dialog.open_last.isChecked()
    dialog.open_last.setChecked(False)
    dialog._save()
    assert settings.get("open_last_project") is False
    assert startup_project(settings, None) is None
