"""run.json -> Bencher Metric Format, for tracking results over time (see .github/workflows/bench-track.yml).

    python tests/bench/bmf.py RUN_JSON > bmf.json

Every cell becomes a benchmark named <plan>/<fixture>/<cache>/<tool>@<threads>, so the short (demo) and long (full)
suites never share a series. Measures:

- latency: median time to delete the tree, in ns (Bencher's unit), bounded by the median's 95% CI.
- throughput: files deleted per second at the median, bounded by the same CI.
- time-ratio: remmy's median over the reference tool's (below 1 is faster), bounded by its BCa 95% CI. Only remmy
  gets one: a ratio cancels drift that slows every tool on the box alike, so it is what regressions are judged on.
  The other tools' latencies stay as controls that show such drift.
"""

import json
import sys
from pathlib import Path

SUBJECT = "remmy"


def _name(plan: str, key: dict) -> str:
    return f"{plan}/{key['fixture']}/{key['cache']}/{key['tool']}@{key['threads']}"


def _metric(value: float, lo: float | None, hi: float | None, scale: float = 1.0) -> dict:
    metric = {"value": value * scale}
    if lo is not None and hi is not None:
        metric |= {"lower_value": lo * scale, "upper_value": hi * scale}
    return metric


def convert(run: dict) -> dict:
    plan = run["plan"]["name"]
    out: dict[str, dict] = {}
    for cell in run["summary"]["cells"]:
        ci, median = cell["median_ci95"], cell["median_seconds"]
        files = cell["throughput_files_per_s"] * median
        # Throughput is files / time, so the time CI's upper bound is throughput's lower bound.
        rate_lo = files / ci["hi"] if ci["hi"] else None
        rate_hi = files / ci["lo"] if ci["lo"] else None
        out[_name(plan, cell["key"])] = {
            "latency": _metric(median, ci["lo"], ci["hi"], scale=1e9),
            "throughput": _metric(cell["throughput_files_per_s"], rate_lo, rate_hi),
        }
    for pair in run["summary"]["pairs"]:
        if pair["tool"]["tool"] == SUBJECT:
            ci = pair["time_ratio_ci95"]
            out[_name(plan, pair["tool"])]["time-ratio"] = _metric(pair["time_ratio"], ci["lo"], ci["hi"])
    return out


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    json.dump(convert(json.loads(Path(argv[1]).read_text())), sys.stdout, indent=1)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
