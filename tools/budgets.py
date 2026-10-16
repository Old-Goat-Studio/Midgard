# SPDX-License-Identifier: Apache-2.0
"""Check midgard_bench's timings against the performance budgets in schema/budgets.json.

    midgard_bench --json results.json
    python tools/budgets.py results.json [--platform ci-linux-release]

A benchmark fails when its median is over its budget plus the file's margin, and when it has no
budget at all (every new benchmark needs one). In GitHub Actions each timing is also posted as a
notice and added to the job summary, so CI keeps a record of every run.
"""
import argparse
import json
import os
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUDGETS = ROOT / "schema" / "budgets.json"


@dataclass
class Check:
    name: str
    status: str  # "ok", "over", "no budget", "skipped"
    measured: float | None
    budget: float | None
    limit: float | None
    note: str = ""


def check(results: dict, budgets: dict, platform: str) -> list[Check]:
    metric = budgets.get("metric", "median_ms")
    margin = float(budgets.get("margin", 0.0))
    out = []
    for name, result in sorted(results.items()):
        entry = budgets["budgets"].get(name)
        budget = None if entry is None else float(entry.get(platform, entry["default"]))
        limit = None if budget is None else budget * (1 + margin)
        if "skipped" in result:
            out.append(Check(name, "skipped", None, budget, limit, result["skipped"]))
        elif budget is None:
            out.append(Check(name, "no budget", result[metric], None, None,
                             f"add it to {BUDGETS.relative_to(ROOT).as_posix()}"))
        else:
            measured = float(result[metric])
            out.append(Check(name, "ok" if measured <= limit else "over", measured, budget,
                             limit))
    return out


def report(checks: list[Check], platform: str) -> int:
    github = bool(os.environ.get("GITHUB_ACTIONS"))
    rows = ["| Benchmark | Measured (ms) | Budget (ms) | Result |", "|---|---:|---:|---|"]
    failures = 0
    for c in checks:
        measured = "-" if c.measured is None else f"{c.measured:.3f}"
        budget = "-" if c.budget is None else f"{c.budget:g}"
        line = f"{c.name:<32} {measured:>10} {budget:>10}  {c.status}"
        if c.note:
            line += f" ({c.note})"
        print(line)
        rows.append(f"| {c.name} | {measured} | {budget} | {c.status} |")
        bad = c.status in ("over", "no budget")
        failures += bad
        if github:
            kind = "error" if bad else "notice"
            detail = (f"{measured} ms against a budget of {budget} ms (limit {c.limit:.3f} ms)"
                      if c.limit is not None and c.measured is not None
                      else c.note or c.status)
            print(f"::{kind} title=Budget {c.name} on {platform}::{c.status}: {detail}")
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write(f"### Performance budgets ({platform})\n\n" + "\n".join(rows) + "\n\n")
    print(f"{len(checks) - failures} of {len(checks)} benchmarks within budget on {platform}")
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("results", type=Path)
    parser.add_argument("--platform", default="default")
    parser.add_argument("--budgets", type=Path, default=BUDGETS)
    args = parser.parse_args(argv)
    results = json.loads(args.results.read_text("utf-8"))["results"]
    budgets = json.loads(args.budgets.read_text("utf-8"))
    return report(check(results, budgets, args.platform), args.platform)


if __name__ == "__main__":
    sys.exit(main())
