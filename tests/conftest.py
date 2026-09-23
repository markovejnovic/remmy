"""Shared fixtures for remmy's end-to-end suite.

The binary under test comes from ``--remmy PATH`` or ``$REMMY_BIN``, falling
back to the release preset's output. The suite never builds anything.
"""

from __future__ import annotations

import difflib
import os
import subprocess
import threading
from collections.abc import Callable, Iterable, Iterator
from pathlib import Path

import pytest
from hypothesis import HealthCheck, settings

import fstree
from harness import Result, Runner, run_remmy

# Each example spawns processes and builds trees: no per-example deadline.
settings.register_profile(
    "default", max_examples=60, deadline=None, suppress_health_check=[HealthCheck.too_slow]
)
settings.register_profile("thorough", parent=settings.get_profile("default"), max_examples=1000)
settings.load_profile(os.environ.get("HYPOTHESIS_PROFILE", "default"))

REPO = Path(__file__).resolve().parent.parent
DEFAULT_BIN = REPO / "build" / "gcc-release" / "remmy"

LARGE_DIFF_ITEMS = 200
LARGE_DIFF_CHARS = 10_000


_icdiff_compare: Callable[..., list[str] | None] | None = None


@pytest.hookimpl(trylast=True)
def pytest_configure(config: pytest.Config) -> None:
    """Route pytest-icdiff through our hook so it only ever sees small values.

    pytest runs every ``pytest_assertrepr_compare`` implementation, not just
    the first, so returning early here would not stop icdiff from spending
    minutes on a large diff. Unregister it and call it ourselves instead.
    """
    global _icdiff_compare
    plugin = config.pluginmanager.get_plugin("icdiff")
    if plugin is not None:
        config.pluginmanager.unregister(plugin)
        _icdiff_compare = plugin.pytest_assertrepr_compare


def pytest_assertrepr_compare(
    config: pytest.Config, op: str, left: object, right: object
) -> list[str] | None:
    """Explain ``==`` failures, compactly for large values.

    Tree listings hold thousands of paths and remmy's stderr can run to
    megabytes; a side-by-side diff of those takes minutes and buries the few
    entries that differ. Small values get pytest-icdiff's colored diff.
    """
    if op != "==":
        return None
    if not _is_large(left, right):
        return _icdiff_compare(config, op, left, right) if _icdiff_compare else None
    if isinstance(left, (set, frozenset)) and isinstance(right, (set, frozenset)):
        return _summary(f"sets of {len(left)} and {len(right)} items differ", left - right, right - left, [])
    if isinstance(left, dict) and isinstance(right, dict):
        changed = [k for k in left.keys() & right.keys() if left[k] != right[k]]
        return _summary(
            f"dicts of {len(left)} and {len(right)} keys differ",
            left.keys() - right.keys(),
            right.keys() - left.keys(),
            [f"{k!r}: {_clip(left[k])} != {_clip(right[k])}" for k in changed],
        )
    if isinstance(left, (tuple, list)) and isinstance(right, (tuple, list)) and len(left) == len(right):
        return [
            f"{type(left).__name__}s of {len(left)} differ at:",
            *(f"  [{i}] {_clip(a)} != {_clip(b)}" for i, (a, b) in enumerate(zip(left, right)) if a != b),
        ]
    if isinstance(left, str) and isinstance(right, str):
        diff = list(difflib.unified_diff(right.splitlines(), left.splitlines(), "expected", "actual", n=1, lineterm=""))
        return ["strings differ:", *(_clip(line, 300) for line in diff[:60]), *(["..."] if len(diff) > 60 else [])]
    return ["values differ:", f"  left:  {_clip(left)}", f"  right: {_clip(right)}"]


def _is_large(*values: object) -> bool:
    for v in values:
        if isinstance(v, (set, frozenset, dict, list, tuple)) and len(v) > LARGE_DIFF_ITEMS:
            return True
    return sum(len(repr(v)) for v in values) > LARGE_DIFF_CHARS


def _clip(value: object, limit: int = 500) -> str:
    text = value if isinstance(value, str) else repr(value)
    return text if len(text) <= limit else f"{text[:limit]}... ({len(text)} chars)"


def _summary(title: str, only_left: Iterable[object], only_right: Iterable[object], changed: list[str]) -> list[str]:
    def show(label: str, items: list[str]) -> list[str]:
        if not items:
            return []
        shown = [f"    {_clip(i)}" for i in items[:20]]
        more = [f"    ... and {len(items) - 20} more"] if len(items) > 20 else []
        return [f"  {label} ({len(items)}):", *shown, *more]

    return [
        title,
        *show("only left", sorted(map(repr, only_left))),
        *show("only right", sorted(map(repr, only_right))),
        *show("changed", sorted(changed)),
    ]


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption(
        "--remmy",
        default=os.environ.get("REMMY_BIN", str(DEFAULT_BIN)),
        help="remmy executable under test (default: $REMMY_BIN or %(default)s)",
    )
    bench = parser.getgroup("bench", "comparison benchmark (tests/bench)")
    bench.addoption(
        "--bench",
        choices=("demo", "full"),
        help="run the comparison benchmark with this plan; benchmarks are skipped otherwise",
    )
    bench.addoption("--bench-out", type=Path, help="results directory (default: tests/bench/out/<plan>-<utc>)")
    bench.addoption(
        "--bench-scratch",
        type=Path,
        help="directory to build trees in; its volume is what gets measured (default: a temp dir)",
    )
    bench.addoption(
        "--bench-allow-dirty-env",
        action="store_true",
        help="time even on an unfit machine (battery, snapshots, busy Spotlight, ...)",
    )


ROOT_PROMPT_TIMEOUT = 60
"""Seconds to wait for a sudo password before failing root-only tests."""

ROOT_KEEPALIVE = 60
"""Seconds between sudo timestamp refreshes while a session holds root."""


def _sudo_ready() -> bool:
    return subprocess.run(["sudo", "-n", "true"], capture_output=True, check=False).returncode == 0


def _open_tty() -> int | None:
    try:
        return os.open("/dev/tty", os.O_RDWR)
    except OSError:
        return None


@pytest.fixture(scope="session")
def root(pytestconfig: pytest.Config) -> Iterator[None]:
    """sudo rights for the rest of the session, or a failed test.

    Proceeds at once if sudo is already authorized. Otherwise, when a terminal
    is attached, pauses output capture and asks for the password there (giving
    up after ROOT_PROMPT_TIMEOUT). Without a terminal, or on refusal, the test
    fails rather than skips: a root-only measurement that silently did not
    happen would look like a complete run. While the session holds root, a
    background thread keeps the sudo timestamp fresh.
    """
    if not _sudo_ready():
        tty = _open_tty() if not os.environ.get("PYTEST_XDIST_WORKER") else None
        if tty is None:
            pytest.fail("this test needs root: run `sudo -v` first, or run pytest from a terminal to be prompted")
        capture = pytestconfig.pluginmanager.getplugin("capturemanager")
        try:
            with capture.global_and_fixture_disabled():
                subprocess.run(
                    ["sudo", "-v", "-p", "\n[remmy tests] root needed; sudo password for %u: "],
                    stdin=tty, stdout=tty, stderr=tty, timeout=ROOT_PROMPT_TIMEOUT, check=False,
                )  # fmt: skip
        except subprocess.TimeoutExpired:
            pytest.fail(f"no sudo password within {ROOT_PROMPT_TIMEOUT}s; root-only test not run")
        finally:
            os.close(tty)
        if not _sudo_ready():
            pytest.fail("sudo authorization failed; root-only test not run")

    stop = threading.Event()

    def keepalive() -> None:
        while not stop.wait(ROOT_KEEPALIVE):
            subprocess.run(["sudo", "-n", "-v"], capture_output=True, check=False)

    thread = threading.Thread(target=keepalive, name="sudo-keepalive", daemon=True)
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
def workdir(tmp_path: Path) -> Iterator[Path]:
    """A scratch directory that is removed even if a test left it locked."""
    d = tmp_path / "w"
    d.mkdir()
    yield d
    fstree.force_remove(d)


@pytest.fixture
def run(remmy_bin: Path, workdir: Path) -> Runner:
    """Run remmy with ``workdir`` as the default cwd."""

    def runner(*args: str | os.PathLike[str], cwd: Path | None = None, **kw: object) -> Result:
        return run_remmy(remmy_bin, args, cwd=cwd or workdir, **kw)  # type: ignore[arg-type]

    return runner


@pytest.fixture
def spawn(remmy_bin: Path, workdir: Path) -> Iterator[Callable[..., subprocess.Popen[bytes]]]:
    """Start remmy without waiting, for overlap tests."""

    procs: list[subprocess.Popen[bytes]] = []

    def start(*args: str | os.PathLike[str], threads: int | None = None) -> subprocess.Popen[bytes]:
        env = {k: v for k, v in os.environ.items() if k != "REMMY_THREADS"}
        if threads is not None:
            env["REMMY_THREADS"] = str(threads)
        p = subprocess.Popen(
            [str(remmy_bin), *map(os.fsdecode, args)],
            cwd=workdir,
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        procs.append(p)
        return p

    yield start
    for p in procs:
        if p.poll() is None:
            p.kill()
        p.wait()


@pytest.fixture(scope="session")
def faultlib(tmp_path_factory: pytest.TempPathFactory) -> Path:
    """The fault-injection library, compiled once per session."""
    import faults

    return faults.build_library(tmp_path_factory.mktemp("faultinject"))


THREAD_COUNTS = (1, 2, 4, 16)


@pytest.fixture(params=THREAD_COUNTS, ids=lambda n: f"threads={n}")
def threads(request: pytest.FixtureRequest) -> int:
    return request.param


@pytest.fixture(scope="session")
def is_root() -> bool:
    return os.geteuid() == 0
