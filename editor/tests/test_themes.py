# SPDX-License-Identifier: Apache-2.0
from PySide6.QtCore import QDir
from PySide6.QtWidgets import QApplication, QWidget

from midgard_editor.themes import QSS_PREFIX, ThemeApplier, qss_themes, theme_title


def test_titles_come_from_file_names():
    assert theme_title("solarized-light") == "Solarized Light"
    assert theme_title("yggdrasil") == "Yggdrasil"
    assert theme_title("rock&roll") == "Rock&&roll"


def test_bundled_yggdrasil_theme_is_listed():
    assert "yggdrasil" in qss_themes()


def test_user_themes_are_added_and_replace_bundled_ones(tmp_path):
    user = tmp_path / "Themes"
    user.mkdir()
    (user / "aurora.qss").write_text("QWidget { color: red; }", "utf-8")
    (user / "yggdrasil.qss").write_text("/* mine */", "utf-8")
    themes = qss_themes(tmp_path)
    assert themes["aurora"].parent == user
    assert themes["yggdrasil"].parent == user
    assert list(themes) == sorted(themes, key=lambda n: theme_title(n).casefold())


def test_qss_theme_sets_stylesheet_and_icon_path(qapp):
    applier = ThemeApplier()
    assert applier.apply(QSS_PREFIX + "yggdrasil")
    assert "ads--CDockWidgetTab" in QApplication.instance().styleSheet()
    assert any(p.endswith("yggdrasil-icons") for p in QDir.searchPaths("renoir"))
    assert applier.apply("dark")
    assert QApplication.instance().styleSheet() == ""


def test_unknown_or_missing_themes_are_refused(qapp):
    applier = ThemeApplier()
    assert not applier.apply("qss:does-not-exist")
    assert not applier.apply("purple")
    assert not applier.apply(None)


def test_dock_stylesheet_steps_aside_for_qss_themes(qapp):
    dock = QWidget()
    dock.setStyleSheet("QWidget { color: blue; }")
    applier = ThemeApplier(dock_manager=dock)
    applier.apply(QSS_PREFIX + "yggdrasil")
    assert dock.styleSheet() == ""
    applier.apply("light")
    assert dock.styleSheet() == "QWidget { color: blue; }"
    QApplication.instance().setStyleSheet("")
