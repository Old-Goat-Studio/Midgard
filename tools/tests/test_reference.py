# SPDX-License-Identifier: Apache-2.0
"""The Python reference implementations against the shared tables in schema/tests/ (the engine
is checked against the same tables in engine/tests/test_reference_tables.cpp)."""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from reference import TABLES, table  # noqa: E402
from reference.hashing import fnv1a64  # noqa: E402


def case_bytes(case: dict) -> bytes:
    return case["text"].encode("utf-8") if "text" in case else bytes.fromhex(case["hex"])


def test_fnv1a64_matches_its_table():
    cases = table("fnv1a64")
    assert len(cases) >= 3
    for case in cases:
        assert f"{fnv1a64(case_bytes(case)):016x}" == case["hash"], case


def test_every_table_is_well_formed():
    tables = sorted(TABLES.glob("*.json"))
    assert tables
    for path in tables:
        data = json.loads(path.read_text("utf-8"))
        assert data["_about"] and data["cases"], path.name
