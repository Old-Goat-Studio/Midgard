# SPDX-License-Identifier: Apache-2.0
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import budgets  # noqa: E402

BUDGETS = {
    "margin": 0.25,
    "metric": "median_ms",
    "budgets": {
        "fast": {"default": 10},
        "gpu": {"default": 16.7, "ci-linux-release": 500},
    },
}


def write(tmp_path: Path, results: dict) -> Path:
    path = tmp_path / "results.json"
    path.write_text(json.dumps({"midgard": "0.1.0", "results": results}), "utf-8")
    (tmp_path / "budgets.json").write_text(json.dumps(BUDGETS), "utf-8")
    return path


def run(tmp_path: Path, results: dict, platform: str = "default") -> int:
    return budgets.main([str(write(tmp_path, results)), "--platform", platform,
                         "--budgets", str(tmp_path / "budgets.json")])


def test_within_budget_and_the_margin_passes(tmp_path, capsys):
    # 12 ms is over the 10 ms budget but inside the 25% margin (12.5 ms).
    assert run(tmp_path, {"fast": {"median_ms": 12.0}, "gpu": {"median_ms": 5.0}}) == 0
    assert "2 of 2 benchmarks within budget" in capsys.readouterr().out


def test_over_budget_fails_on_purpose(tmp_path, capsys, monkeypatch):
    monkeypatch.setenv("GITHUB_ACTIONS", "true")
    summary = tmp_path / "summary.md"
    monkeypatch.setenv("GITHUB_STEP_SUMMARY", str(summary))
    assert run(tmp_path, {"fast": {"median_ms": 12.6}, "gpu": {"median_ms": 5.0}}) == 1
    out = capsys.readouterr().out
    assert "::error title=Budget fast on default::over: 12.600 ms against a budget of 10 ms" in out
    assert "::notice title=Budget gpu on default::ok" in out
    assert "| fast | 12.600 | 10 | over |" in summary.read_text("utf-8")


def test_platforms_have_their_own_budgets(tmp_path):
    slow_gpu = {"fast": {"median_ms": 1.0}, "gpu": {"median_ms": 300.0}}
    assert run(tmp_path, slow_gpu, "ci-linux-release") == 0  # software Vulkan
    assert run(tmp_path, slow_gpu, "default") == 1


def test_a_benchmark_without_a_budget_fails(tmp_path, capsys):
    assert run(tmp_path, {"fast": {"median_ms": 1.0}, "new_thing": {"median_ms": 1.0}}) == 1
    assert "no budget (add it to schema/budgets.json)" in capsys.readouterr().out


def test_skipped_benchmarks_pass(tmp_path, capsys):
    assert run(tmp_path, {"gpu": {"skipped": "no Vulkan: no device"}}) == 0
    assert "skipped (no Vulkan: no device)" in capsys.readouterr().out


def test_the_real_budgets_cover_every_benchmark():
    """Every benchmark midgard_bench has (its source names them) has a budget."""
    real = json.loads(budgets.BUDGETS.read_text("utf-8"))
    source = (budgets.ROOT / "engine" / "bench" / "main.cpp").read_text("utf-8")
    names = set(re.findall(r'\{"([a-z0-9_]+\.[a-z0-9_]+)"', source))
    assert names and names == set(real["budgets"])
    for entry in real["budgets"].values():
        assert entry["default"] > 0
