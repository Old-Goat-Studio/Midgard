# SPDX-License-Identifier: Apache-2.0
"""A surface shader's settings in the Material editor (milestone 5.17 step 3).

A shader declares its settings in its code (surface_shaders.py); here each gets the control
its type wants (a slider, a tick box, a colour, numbers, a texture), showing the material's own
value or, until it picks one, the shader's. Changes go out as value_changed(name, value); a
value of None means "the shader's own again".
"""
from pathlib import Path
from typing import Any

from PySide6.QtCore import Signal
from PySide6.QtWidgets import (
    QCheckBox,
    QColorDialog,
    QDoubleSpinBox,
    QFileDialog,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QToolButton,
    QWidget,
)

from .. import surface_shaders, textures


class ShaderSettings(QWidget):
    value_changed = Signal(str, object)

    def __init__(self, project_root: Path, parent: QWidget | None = None):
        super().__init__(parent)
        self.project_root = Path(project_root)
        self.form = QFormLayout(self)
        self.form.setContentsMargins(0, 0, 0, 0)
        self._layout_key: tuple = ()
        self._settings: list[dict] = []
        self._setters: dict[str, Any] = {}  # name -> a function showing a value
        self.controls: dict[str, QWidget] = {}  # name -> its control (for tests)

    # ---- showing ----------------------------------------------------------------------------
    def show_settings(self, settings: list[dict], chosen: dict) -> None:
        """The shader's settings with the material's values (rebuilt only when they change)."""
        key = tuple((s["name"], s["type"], s["label"], tuple(s.get("value", [])), s["min"],
                     s["max"]) for s in settings)
        if key != self._layout_key:
            self._build(settings)
            self._layout_key = key
        for setting in settings:
            own = chosen.get(setting["name"]) if isinstance(chosen, dict) else None
            self._setters[setting["name"]](surface_shaders.setting_value(setting, own),
                                           own is not None)
        self.setVisible(bool(settings))

    def _build(self, settings: list[dict]) -> None:
        while self.form.rowCount():
            self.form.removeRow(0)
        self._settings = settings
        self._setters.clear()
        self.controls.clear()
        if settings:
            title = QLabel("<b>Shader settings</b>")
            title.setToolTip("The settings the surface shader declares; each material using it "
                             "has its own")
            self.form.addRow(title)
        for setting in settings:
            control, setter = self._control(setting)
            row = QWidget()
            layout = QHBoxLayout(row)
            layout.setContentsMargins(0, 0, 0, 0)
            layout.addWidget(control, 1)
            back = QToolButton()
            back.setText("↺")
            back.setToolTip("Back to the shader's own value")
            back.clicked.connect(lambda _c=False, n=setting["name"]:
                                 self.value_changed.emit(n, None))
            layout.addWidget(back)
            label = QLabel(setting["label"])
            label.setToolTip(f"'{setting['name']}' in the shader (line {setting['line']})")
            self.form.addRow(label, row)
            self.controls[setting["name"]] = control

            def show(value, own, setter=setter, back=back):
                setter(value)
                back.setEnabled(own)
            self._setters[setting["name"]] = show

    def _control(self, setting: dict):
        """A control for a setting, and a function showing a value in it."""
        name, kind = setting["name"], setting["type"]
        emit = lambda value: self.value_changed.emit(name, value)  # noqa: E731
        if kind == "bool":
            box = QCheckBox()
            box.toggled.connect(emit)
            return box, lambda v: _quietly(box, lambda: box.setChecked(v[0] > 0.5))
        if kind in ("float", "int"):
            from .material_editor import _Amount  # (it uses this module)
            amount = _Amount(float(setting["min"]), float(setting["max"]),
                             0 if kind == "int" else 2)
            amount.changed.connect(lambda v: emit(int(round(v)) if kind == "int" else v))
            return amount, lambda v: amount.set_value(v[0])
        if kind == "color":
            button = QPushButton()

            def pick():
                from .material_editor import _rgb
                current = surface_shaders.setting_value(setting, None)
                chosen = QColorDialog.getColor(_rgb(button.property("rgb") or current), self,
                                               setting["label"])
                if chosen.isValid():
                    emit([round(c, 4) for c in (chosen.redF(), chosen.greenF(), chosen.blueF())])
            button.clicked.connect(pick)

            def show(v):
                from .material_editor import _style
                button.setProperty("rgb", list(v))
                _style(button, v)
            return button, show
        if kind in ("vec2", "vec3", "vec4"):
            row = QWidget()
            layout = QHBoxLayout(row)
            layout.setContentsMargins(0, 0, 0, 0)
            boxes = []
            for _ in range(int(kind[-1])):
                box = QDoubleSpinBox()
                box.setRange(-1e6, 1e6)
                box.setDecimals(3)
                box.setSingleStep(0.05)
                layout.addWidget(box)
                boxes.append(box)
            for box in boxes:
                box.valueChanged.connect(lambda _v: emit([round(b.value(), 4) for b in boxes]))

            def show(v):
                for box, x in zip(boxes, v, strict=False):
                    _quietly(box, lambda b=box, x=x: b.setValue(x))
            return row, show
        # A texture: a picture (sRGB) or data (linear).
        row = QWidget()
        layout = QHBoxLayout(row)
        layout.setContentsMargins(0, 0, 0, 0)
        shown = QLabel("None")
        choose = QPushButton("Choose…")
        clear = QToolButton()
        clear.setText("✕")
        clear.setToolTip("No texture (white)")
        layout.addWidget(shown, 1)
        layout.addWidget(choose)
        layout.addWidget(clear)
        slot = "base_color" if kind == "texture" else "normal"  # (sRGB or linear)

        def pick():
            file, _ = QFileDialog.getOpenFileName(self, setting["label"], str(self.project_root),
                                                  "Pictures (*.png *.jpg *.jpeg)")
            if file:
                emit(textures.ref(textures.copy_texture(Path(file), self.project_root), slot))
        choose.clicked.connect(pick)
        clear.clicked.connect(lambda: emit(""))

        def show(v):
            shown.setText(textures.describe(v) if v else "None")
            shown.setToolTip(v)
        return row, show


def _quietly(widget: QWidget, change) -> None:
    widget.blockSignals(True)
    try:
        change()
    finally:
        widget.blockSignals(False)
