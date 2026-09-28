"""Reports remmy against each baseline from a run.json, and fails on a regression:

    python tests/bench/compare.py RUN_JSON [--fail-on BASELINE]

Prints a Markdown table (bench.yml appends it to the job summary). With --fail-on, exits 1 when remmy is DISTINCT
from BASELINE and slower, by the plan's decision rule: the ratio's 95% CI lies entirely above 1 + margin and the
effect size is large. Both ran in the same shuffled rounds on the same machine, so drift between runs can't cause
this; a TIE or INDETERMINATE never fails.
"""

import argparse
import json
import sys
from pathlib import Path


def rows(run: dict) -> list[dict]:
    return sorted(
        run["summary"].get("versus", ()),
        key=lambda p: (p["reference"]["tool"], p["tool"]["fixture"], p["tool"]["cache"], p["tool"]["threads"]),
    )


def table(run: dict) -> str:
    lines = [
        "| tree | cache | threads | vs | remmy's time | 95% CI | verdict |",
        "|---|---|---:|---|---:|---|---|",
    ]
    for p in rows(run):
        t, ci = p["tool"], p["time_ratio_ci95"]
        ci_text = f"{ci['lo']:.3f}–{ci['hi']:.3f}" if ci["lo"] is not None else "n/a"
        change = f"{100 * (p['time_ratio'] - 1):+.1f}%"
        lines.append(
            f"| {t['fixture']} | {t['cache']} | {t['threads']} | {p['reference']['tool']} | {change} "
            f"| {ci_text} | {p['verdict']} |"
        )
    return "\n".join(lines)


def regressions(run: dict, baseline: str) -> list[dict]:
    return [
        p
        for p in rows(run)
        if p["reference"]["tool"] == baseline and p["verdict"] == "distinct" and p["time_ratio"] > 1
    ]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("run_json", type=Path)
    parser.add_argument("--fail-on", metavar="BASELINE", help="exit 1 if remmy is distinctly slower than BASELINE")
    args = parser.parse_args(argv)
    run = json.loads(args.run_json.read_text())
    if not rows(run):
        print("No baseline was timed in this run.")
        return 0
    margin = run["plan"]["versus_rule"]["margin"]
    print(
        f"remmy's median time relative to each baseline, same run (positive is slower; a verdict needs the CI "
        f"outside ±{100 * margin:.0f}% and a large effect size).\n"
    )
    print(table(run))
    if args.fail_on and (bad := regressions(run, args.fail_on)):
        worst = max(bad, key=lambda p: p["time_ratio"])
        print(
            f"\n**Regression:** {len(bad)} cell(s) distinctly slower than {args.fail_on}; worst "
            f"{worst['tool']['fixture']} @{worst['tool']['threads']} threads, "
            f"{100 * (worst['time_ratio'] - 1):+.1f}%."
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
