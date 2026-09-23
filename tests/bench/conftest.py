"""The benchmark session: plan, environment, samples, and the artifacts.

Benchmarks are opt-in (``--bench=demo|full|durability``) and must run in a
single process: parallel workers would time each other.
"""

from __future__ import annotations

import datetime
import os
import random
import shutil
import subprocess
import tempfile
from pathlib import Path

import environment
import pytest
import registry
import runner
import schema
import stats
from session import BenchSession

REPO = Path(__file__).resolve().parents[2]
MKTREE_SOURCE = Path(__file__).resolve().parent / "mktree.cpp"
BENCH_KEY = pytest.StashKey["BenchSession"]()


def selected_plan(config: pytest.Config) -> schema.Plan | None:
    name = config.getoption("--bench")
    return registry.plan(name, Path(config.getoption("--remmy")).resolve()) if name else None


def pytest_generate_tests(metafunc: pytest.Metafunc) -> None:
    plan = selected_plan(metafunc.config)
    if "matrix" in metafunc.fixturenames:
        cases = [(f.id, c) for f in plan.fixtures for c in plan.caches[f.id]] if plan else [("-", schema.Cache.WARM)]
        metafunc.parametrize("matrix", cases, ids=[f"{f}-{c}" for f, c in cases])


def _compile_mktree(out: Path) -> Path:
    binary = out / "mktree"
    compiler = shutil.which("c++") or shutil.which("clang++")
    if compiler is None:
        pytest.fail("a C++ compiler is needed to build the tree generator (tests/bench/mktree.cpp)")
    subprocess.run(
        [compiler, "-O2", "-std=c++17", "-pthread", "-o", str(binary), str(MKTREE_SOURCE)],
        check=True,
        capture_output=True,
    )
    return binary


@pytest.fixture(scope="session")
def bench(pytestconfig: pytest.Config, tmp_path_factory: pytest.TempPathFactory) -> BenchSession:
    plan = selected_plan(pytestconfig)
    if plan is None:
        pytest.skip("benchmarks are opt-in: pass --bench=demo (minutes) or --bench=full (hours)")
    if os.environ.get("PYTEST_XDIST_WORKER"):
        pytest.fail("benchmarks must run in one process; drop -n or pass -p no:xdist")
    existing = pytestconfig.stash.get(BENCH_KEY, None)
    if existing is not None:
        return existing

    hyperfine = shutil.which("hyperfine")
    if hyperfine is None:
        pytest.fail("hyperfine is required (brew install hyperfine)")
    missing = [f"{t.name} ({p})" for t in plan.tools for p in t.requires if not os.access(p, os.X_OK)]
    if missing:
        pytest.fail(f"benchmark tools missing: {', '.join(missing)}")

    scratch_parent = pytestconfig.getoption("--bench-scratch")
    scratch = Path(tempfile.mkdtemp(prefix="remmy-bench-", dir=scratch_parent))
    env = runner.Environment(
        hyperfine=hyperfine,
        mktree=_compile_mktree(tmp_path_factory.mktemp("mktree")),
        scratch=scratch,
    )
    reason = environment.unfit(scratch)
    if reason is not None and not pytestconfig.getoption("--bench-allow-dirty-env"):
        pytest.fail(f"not timing: {reason}. Fix it, or pass --bench-allow-dirty-env to time anyway.")

    stamp = datetime.datetime.now(datetime.UTC).strftime("%Y%m%dT%H%M%SZ")
    out = pytestconfig.getoption("--bench-out") or REPO / "tests" / "bench" / "out" / f"{plan.name}-{stamp}"
    session = BenchSession(
        plan=plan,
        env=env,
        out=out,
        rng=random.Random(plan.seed),
    )
    pytestconfig.stash[BENCH_KEY] = session
    return session


def pytest_sessionfinish(session: pytest.Session) -> None:
    bench = session.config.stash.get(BENCH_KEY, None)
    if bench is None or not bench.samples:
        return
    import charts

    summary = bench.summary()
    # Before the charts, so the data survives a charting failure.
    bench.write_json(summary)
    charts.render(bench.run(), summary, bench.out)
    shutil.rmtree(bench.env.scratch, ignore_errors=True)
    terminal = session.config.pluginmanager.getplugin("terminalreporter")
    if terminal is not None:
        terminal.write_sep("=", "benchmark")
        for line in stats.headline(summary):
            terminal.write_line(line)
        terminal.write_line(f"results: {bench.out}")
