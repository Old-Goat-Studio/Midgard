# SPDX-License-Identifier: Apache-2.0
"""Checks that the GUI dependencies install and load. The editor window arrives in Phase 1."""
import pytest

pytestmark = pytest.mark.qt


def test_pyside6_is_new_enough():
    import PySide6

    major, minor = (int(p) for p in PySide6.__version__.split(".")[:2])
    assert (major, minor) >= (6, 8), "QStyleHints.setColorScheme needs Qt 6.8+"


def test_qt_advanced_docking_loads():
    import PySide6QtAds as QtAds

    assert hasattr(QtAds, "CDockManager")
