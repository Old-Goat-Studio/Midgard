# SPDX-License-Identifier: Apache-2.0
"""A character's humanoid rig (milestone 5.13): which of its model's joints is each humanoid bone
(the hips, the spine, the left upper arm...). The engine finds them by name (the common rigs'
naming: Unreal's, Mixamo's, Blender's, 3ds Max's Biped); here a designer names the ones it found
wrongly, or couldn't find. Clips from another file are retargeted to a humanoid, so a character
of other proportions plays them as its own."""
from PySide6.QtWidgets import (
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFormLayout,
    QLabel,
    QScrollArea,
    QVBoxLayout,
    QWidget,
)

FOUND = None  # a combo's data for "as its names say"


def effective_rig(found: dict[str, str], chosen: dict[str, str]) -> dict[str, str]:
    """Each bone's joint ("" for none): what the names said, with the designer's choices over it
    (a joint chosen for one bone is no longer another's)."""
    rig = dict(found)
    for bone, joint in chosen.items():
        if joint:
            for other, j in rig.items():
                if j == joint:
                    rig[other] = ""
        rig[bone] = joint
    return rig


def missing_bones(rig: dict[str, str], bones: list[tuple[str, bool]]) -> list[str]:
    """The required bones a rig hasn't got."""
    return [name for name, required in bones if required and not rig.get(name)]


def bone_label(name: str) -> str:
    return name.replace("_", " ").capitalize()


def rig_summary(found: dict[str, str], chosen: dict[str, str],
                bones: list[tuple[str, bool]]) -> str:
    missing = missing_bones(effective_rig(found, chosen), bones)
    if not missing:
        return "Humanoid: animations from other files are retargeted to it"
    names = ", ".join(bone_label(b).lower() for b in missing)
    return f"Not a humanoid (no {names}): animations from other files play by joint name"


class RigDialog(QDialog):
    def __init__(self, joints: list[str], found: dict[str, str], chosen: dict[str, str],
                 bones: list[tuple[str, bool]], parent=None):
        super().__init__(parent)
        self.setWindowTitle("Humanoid Rig")
        self.bones = bones
        self.found = found
        layout = QVBoxLayout(self)
        layout.addWidget(QLabel("Each humanoid bone's joint in the model. Bones in bold are needed "
                                "for animations to be retargeted to it."))
        body = QWidget()
        form = QFormLayout(body)
        self.combos: dict[str, QComboBox] = {}
        for name, required in bones:
            combo = QComboBox()
            by_name = found.get(name, "")
            combo.addItem(f"{by_name} (by its name)" if by_name else "(none found by name)", FOUND)
            combo.addItem("(none)", "")
            for j in joints:
                combo.addItem(j, j)
            want = chosen.get(name, FOUND)
            if want and combo.findData(want) < 0:
                combo.addItem(f"{want} (missing)", want)
            combo.setCurrentIndex(max(0, combo.findData(want)) if want is not FOUND else 0)
            combo.currentIndexChanged.connect(lambda _i: self._update())
            label = QLabel(bone_label(name))
            if required:
                label.setStyleSheet("font-weight: bold")
            form.addRow(label, combo)
            self.combos[name] = combo
        scroll = QScrollArea()
        scroll.setWidget(body)
        scroll.setWidgetResizable(True)
        layout.addWidget(scroll)
        self.summary = QLabel()
        self.summary.setWordWrap(True)
        layout.addWidget(self.summary)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok |
                                   QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        self.resize(420, 560)
        self._update()

    def chosen(self) -> dict[str, str]:
        """The designer's choices: bones not left to their names."""
        return {name: combo.currentData() for name, combo in self.combos.items()
                if combo.currentData() is not FOUND}

    def _update(self) -> None:
        self.summary.setText(rig_summary(self.found, self.chosen(), self.bones))
