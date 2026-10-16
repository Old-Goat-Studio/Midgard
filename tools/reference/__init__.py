# SPDX-License-Identifier: Apache-2.0
"""Reference implementations: short, readable Python versions of the engine's specified formulas
(hashing and deterministic maths now; shape mappings, solar phase and calendar dates later in
Phase 5).

Each formula has a shared table of inputs and expected outputs in schema/tests/<name>.json. The
Python here and the C++ engine are both checked against the same table (tools/tests/
test_reference.py and engine/tests/test_reference_tables.cpp), so they can't drift apart.
"""
import json
from pathlib import Path

TABLES = Path(__file__).resolve().parent.parent.parent / "schema" / "tests"


def table(name: str) -> list[dict]:
    """The cases of schema/tests/<name>.json."""
    return json.loads((TABLES / f"{name}.json").read_text("utf-8"))["cases"]
