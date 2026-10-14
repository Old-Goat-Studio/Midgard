# Midgard editor

The Python / PySide6 editor. In Phase 0 it provides the contract tools only; the main window
arrives in Phase 1.

```sh
python -m venv .venv
.venv/bin/pip install -e "editor[dev]"      # Windows: .venv\Scripts\pip ...
.venv/bin/python -m midgard_editor --version
.venv/bin/pytest editor
```
