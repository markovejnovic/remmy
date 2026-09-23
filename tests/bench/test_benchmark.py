"""The comparison benchmark (opt-in: ``pytest tests/bench --bench=demo``).

Each ``test_matrix`` times one (fixture, cache) matrix: every tool at every
thread count, on a fresh tree per run, in shuffled rounds. Charts are written
when the session ends.
"""

from __future__ import annotations

import pytest
import runner
import schema
from session import BenchSession

pytestmark = [pytest.mark.benchmark, pytest.mark.timeout(0)]


def test_matrix(bench: BenchSession, matrix: tuple[str, schema.Cache], request: pytest.FixtureRequest) -> None:
    fixture_id, cache = matrix
    if cache is schema.Cache.COLD:
        request.getfixturevalue("root")
    fixture = bench.plan.fixture(fixture_id)
    reporter = request.config.pluginmanager.getplugin("terminalreporter")

    def progress(message: str) -> None:
        if reporter is not None:
            reporter.write_line(f"  {fixture_id}/{cache} {message}")

    try:
        bench.samples += runner.run_matrix(
            bench.plan,
            fixture,
            cache,
            bench.env,
            rng=bench.rng,
            progress=progress,
        )
    except runner.BenchmarkFailed as e:
        pytest.fail(str(e))
