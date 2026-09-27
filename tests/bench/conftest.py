"""The benchmark session. Benchmarks are opt-in (--bench=demo|full) and must run in a single process: parallel
workers would time each other."""

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

HERE = Path(__file__).resolve().parent
BENCH_KEY = pytest.StashKey["BenchSession"]()


def selected_plan(config: pytest.Config) -> schema.Plan | None:
    name = config.getoption("--bench")
    return registry.plan(name, Path(config.getoption("--remmy")).resolve()) if name else None


def pytest_generate_tests(metafunc: pytest.Metafunc) -> None:
    if "matrix" in metafunc.fixturenames:
        plan = selected_plan(metafunc.config)
        cases = [(f.id, c) for f in plan.fixtures for c in plan.caches[f.id]] if plan else [("-", schema.Cache.WARM)]
        metafunc.parametrize("matrix", cases, ids=[f"{f}-{c}" for f, c in cases])


@pytest.fixture(scope="session")
def bench(pytestconfig: pytest.Config, tmp_path_factory: pytest.TempPathFactory) -> BenchSession:
    if (plan := selected_plan(pytestconfig)) is None:
        pytest.skip("benchmarks are opt-in: pass --bench=demo (minutes) or --bench=full (hours)")
    if os.environ.get("PYTEST_XDIST_WORKER"):
        pytest.fail("benchmarks must run in one process; drop -n or pass -p no:xdist")
    if (existing := pytestconfig.stash.get(BENCH_KEY, None)) is not None:
        return existing

    if (hyperfine := shutil.which("hyperfine")) is None:
        pytest.fail("hyperfine is required (brew install hyperfine)")
    if missing := [f"{t.name} ({p})" for t in plan.tools for p in t.requires if not os.access(p, os.X_OK)]:
        pytest.fail(f"benchmark tools missing: {', '.join(missing)}")
    if (cxx := shutil.which("c++") or shutil.which("clang++")) is None:
        pytest.fail("a C++ compiler is needed to build the tree generator (tests/bench/mktree.cpp)")
    mktree = tmp_path_factory.mktemp("mktree") / "mktree"
    cmd = [cxx, "-O2", "-std=c++17", "-pthread", "-o", str(mktree), str(HERE / "mktree.cpp")]
    subprocess.run(cmd, check=True, capture_output=True)

    scratch = Path(tempfile.mkdtemp(prefix="remmy-bench-", dir=pytestconfig.getoption("--bench-scratch")))
    reason = environment.unfit(scratch)
    if reason is not None and not pytestconfig.getoption("--bench-allow-dirty-env"):
        pytest.fail(f"not timing: {reason}. Fix it, or pass --bench-allow-dirty-env to time anyway.")

    stamp = datetime.datetime.now(datetime.UTC).strftime("%Y%m%dT%H%M%SZ")
    out = pytestconfig.getoption("--bench-out") or HERE / "out" / f"{plan.name}-{stamp}"
    env = runner.Environment(hyperfine=hyperfine, mktree=mktree, scratch=scratch)
    session = pytestconfig.stash[BENCH_KEY] = BenchSession(plan=plan, env=env, out=out, rng=random.Random(plan.seed))
    return session


def pytest_sessionfinish(session: pytest.Session) -> None:
    bench = session.config.stash.get(BENCH_KEY, None)
    if bench is None or not bench.samples:
        return
    import charts

    summary = bench.summary()
    bench.write_json(summary)  # before the charts, so the data survives a charting failure
    charts.render(bench.run(), summary, bench.out)
    shutil.rmtree(bench.env.scratch, ignore_errors=True)
    if terminal := session.config.pluginmanager.getplugin("terminalreporter"):
        terminal.write_sep("=", "benchmark")
        for line in stats.headline(summary):
            terminal.write_line(line)
        terminal.write_line(f"results: {bench.out}")
