"""Shared fixtures. The binary under test is ``--remmy``, ``$REMMY_BIN``, or the release preset's output."""

import difflib
import os
import subprocess
import threading
from contextlib import suppress
from pathlib import Path

import fstree
import pytest
from harness import Result, Runner, remmy_env, run_remmy
from hypothesis import HealthCheck, settings

# Each example spawns processes and builds trees: no per-example deadline.
settings.register_profile("default", max_examples=60, deadline=None, suppress_health_check=[HealthCheck.too_slow])
settings.register_profile("thorough", parent=settings.get_profile("default"), max_examples=1000)
settings.load_profile(os.environ.get("HYPOTHESIS_PROFILE", "default"))

DEFAULT_BIN = Path(__file__).resolve().parent.parent / "build" / "gcc-release" / "remmy"
_icdiff = None


@pytest.hookimpl(trylast=True)
def pytest_configure(config: pytest.Config) -> None:
    # pytest runs every assertrepr hook, so icdiff must be unregistered (and called by us) to skip huge values.
    global _icdiff
    if plugin := config.pluginmanager.get_plugin("icdiff"):
        config.pluginmanager.unregister(plugin)
        _icdiff = plugin.pytest_assertrepr_compare
    # Under xdist only the controller sees every case's report.
    if (out := config.getoption("--parity-report")) and not hasattr(config, "workerinput"):
        from parity_score import ParityScore

        config.pluginmanager.register(ParityScore(out.resolve()), "parity_score")


def _clip(v: object, limit: int = 300) -> str:
    s = v if isinstance(v, str) else repr(v)
    return s if len(s) <= limit else f"{s[:limit]}... ({len(s)} chars)"


def pytest_assertrepr_compare(config: pytest.Config, op: str, left: object, right: object) -> list[str] | None:
    """Listings of thousands of paths and megabytes of stderr take minutes to diff: summarize those."""
    if op != "==":
        return None
    if len(repr(left)) + len(repr(right)) < 10_000:
        return _icdiff(config, op, left, right) if _icdiff else None
    if isinstance(left, str) and isinstance(right, str):
        lines = difflib.unified_diff(right.splitlines(), left.splitlines(), "expected", "actual", n=1, lineterm="")
    elif isinstance(left, (set, frozenset, dict)) and isinstance(right, (set, frozenset, dict)):
        lines = [f"only left: {_clip(k)}" for k in left if k not in right]
        lines += [f"only right: {_clip(k)}" for k in right if k not in left]
        if isinstance(left, dict) and isinstance(right, dict):
            lines += [
                f"{k!r}: {_clip(left[k])} != {_clip(right[k])}" for k in left if k in right and left[k] != right[k]
            ]
    elif isinstance(left, (list, tuple)) and isinstance(right, (list, tuple)) and len(left) == len(right):
        lines = [f"[{i}] {_clip(a)} != {_clip(b)}" for i, (a, b) in enumerate(zip(left, right)) if a != b]
    else:
        lines = [f"left:  {_clip(left)}", f"right: {_clip(right)}"]
    lines = [*map(_clip, lines)]
    return [f"large {type(left).__name__} values differ:", *lines[:60], *(["..."] if len(lines) > 60 else [])]


def pytest_collection_modifyitems(items: list[pytest.Item]) -> None:
    import parity_gaps

    parity_gaps.mark_known_gaps(items)


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption("--remmy", default=os.environ.get("REMMY_BIN", str(DEFAULT_BIN)), help="remmy executable")
    parser.addoption("--parity-report", type=Path, help="write the run's BSD rm parity score here as JSON")
    bench = parser.getgroup("bench", "comparison benchmark (tests/bench)")
    bench.addoption("--bench", choices=("demo", "full"), help="run the benchmark with this plan (skipped otherwise)")
    bench.addoption("--bench-out", type=Path, help="results directory (default: tests/bench/out/<plan>-<utc>)")
    bench.addoption("--bench-scratch", type=Path, help="where to build trees; its volume is measured (default: tmp)")
    bench.addoption("--bench-allow-dirty-env", action="store_true", help="time even on an unfit (busy) machine")
    bench.addoption(
        "--bench-baseline",
        action="append",
        metavar="NAME=PATH",
        help="also time this remmy build as NAME and compare remmy with it at every thread count (repeatable)",
    )


def _sudo_ready() -> bool:
    return subprocess.run(["sudo", "-n", "true"], capture_output=True, check=False).returncode == 0


@pytest.fixture(scope="session")
def root(pytestconfig: pytest.Config):
    """sudo for the session. Fails rather than skips: a silently missing root-only measurement looks complete."""
    if not _sudo_ready() and not os.environ.get("PYTEST_XDIST_WORKER"):
        capture = pytestconfig.pluginmanager.getplugin("capturemanager")
        cmd = ["sudo", "-v", "-p", "\n[remmy tests] sudo password for %u: "]
        with (
            suppress(OSError, subprocess.TimeoutExpired),
            open("/dev/tty", "r+b", 0) as tty,
            capture.global_and_fixture_disabled(),
        ):
            subprocess.run(cmd, stdin=tty, stdout=tty, stderr=tty, timeout=60, check=False)
    if not _sudo_ready():
        pytest.fail("this test needs root: run `sudo -v` first, or run pytest from a terminal to be prompted")
    stop = threading.Event()

    def keepalive() -> None:
        while not stop.wait(60):
            subprocess.run(["sudo", "-n", "-v"], capture_output=True, check=False)

    thread = threading.Thread(target=keepalive, daemon=True)
    thread.start()
    yield
    stop.set()
    thread.join()


@pytest.fixture(scope="session")
def remmy_bin(pytestconfig: pytest.Config) -> Path:
    p = Path(pytestconfig.getoption("--remmy")).resolve()
    if not os.access(p, os.X_OK):
        pytest.exit(f"remmy executable not found at {p}; build it or pass --remmy", 2)
    return p


@pytest.fixture
def workdir(tmp_path: Path):
    """A scratch directory that is removed even if a test left it locked."""
    d = tmp_path / "w"
    d.mkdir()
    yield d
    fstree.force_remove(d)


@pytest.fixture
def run(remmy_bin: Path, workdir: Path) -> Runner:
    def runner(*args, cwd: Path | None = None, **kw) -> Result:
        return run_remmy(remmy_bin, args, cwd=cwd or workdir, **kw)

    return runner


@pytest.fixture
def spawn(remmy_bin: Path, workdir: Path):
    """Start remmy without waiting, for overlap tests."""
    procs: list[subprocess.Popen[bytes]] = []

    def start(*args, threads: int | None = None) -> subprocess.Popen[bytes]:
        argv = [str(remmy_bin), *map(os.fsdecode, args)]
        p = subprocess.Popen(argv, cwd=workdir, env=remmy_env(threads), stdin=subprocess.DEVNULL, stdout=-1, stderr=-1)
        procs.append(p)
        return p

    yield start
    for p in procs:
        if p.poll() is None:
            p.kill()
        p.wait()


@pytest.fixture(scope="session")
def faultlib(tmp_path_factory: pytest.TempPathFactory) -> Path:
    import faults

    return faults.build_library(tmp_path_factory.mktemp("faultinject"))


@pytest.fixture(params=(1, 2, 4, 16), ids=lambda n: f"threads={n}")
def threads(request: pytest.FixtureRequest) -> int:
    return request.param


@pytest.fixture(scope="session")
def is_root() -> bool:
    return os.geteuid() == 0
