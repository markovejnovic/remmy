"""Scores a run's BSD /bin/rm parity: the share of parity cases remmy passed, written as JSON by ``--parity-report``.

Every collected parity case counts, deferred ones too: the score is what a user sees, not what is in scope. An xfail
is a failure, and so is a non-strict XPASS: a timing-dependent case matching by chance is not a fix, and would make
the score flap. Skipped cases (off macOS) count neither way, so a run that skipped them all writes ``total: 0`` and
no percentage.
"""

import json
from pathlib import Path

import parity_gaps
import pytest


class ParityScore:
    def __init__(self, out: Path) -> None:
        self.out = out
        self.outcomes: dict[str, str] = {}  # nodeid -> passed | failed | skipped

    def pytest_runtest_logreport(self, report: pytest.TestReport) -> None:
        if "parity" not in report.keywords:
            return
        if report.failed or hasattr(report, "wasxfail"):
            self.outcomes[report.nodeid] = "failed"
        elif report.skipped:
            self.outcomes.setdefault(report.nodeid, "skipped")
        elif report.when == "call":
            self.outcomes.setdefault(report.nodeid, "passed")

    def pytest_sessionfinish(self) -> None:
        known = parity_gaps.ledger()
        failing: dict[str, int] = {}
        unlisted: list[str] = []
        for nodeid, outcome in sorted(self.outcomes.items()):
            if outcome != "failed":
                continue
            path, _, name = nodeid.partition("::")
            if entry := known.get(f"{Path(path).name}::{name}"):
                failing[entry[0]] = failing.get(entry[0], 0) + 1
            else:
                unlisted.append(nodeid)
        passed = sum(o == "passed" for o in self.outcomes.values())
        total = passed + sum(failing.values()) + len(unlisted)
        report = {
            "passed": passed,
            "total": total,
            # Rounded down: the badge never claims more than was measured.
            "percent": int(1000 * passed / total) / 10 if total else None,
            "failing": {slug: failing.get(slug, 0) for slug in (*parity_gaps.GAPS, *parity_gaps.DEFERRED)},
            "deferred": [*parity_gaps.DEFERRED],
            "unlisted": unlisted,
        }
        self.out.parent.mkdir(parents=True, exist_ok=True)
        self.out.write_text(json.dumps(report, indent=2) + "\n")
