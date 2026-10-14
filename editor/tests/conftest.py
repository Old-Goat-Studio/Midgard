# SPDX-License-Identifier: Apache-2.0
import os

# Tests never open real windows: Qt draws offscreen (set before Qt is imported).
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest  # noqa: E402

from midgard_editor.project import Project  # noqa: E402
from midgard_editor.settings import Settings  # noqa: E402


@pytest.fixture
def settings(tmp_path):
    return Settings(tmp_path / "settings")


@pytest.fixture
def project(tmp_path, qapp):
    return Project.create(tmp_path / "projects", "HelloCube", "system_map", "Hello Cube")


@pytest.fixture
def window(qtbot, settings):
    from midgard_editor.main_window import MainWindow

    win = MainWindow(settings)
    qtbot.addWidget(win)
    return win
