"""Running remmy under test: the one place that spawns it and checks for crashes.

Shared by every test module (and the benchmark), so it lives in a plain module
rather than a conftest, which pytest may load under the same name twice.
"""

from __future__ import annotations

import os
import resource
import subprocess
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path

# Generous: a hang means a scheduler deadlock, which must fail, not stall CI.
RUN_TIMEOUT = 120

# Markers that mean the process tripped a sanitizer or a hardening assertion.
CRASH_MARKERS = (
    "AddressSanitizer",
    "ThreadSanitizer",
    "UndefinedBehaviorSanitizer",
    "LeakSanitizer",
    "runtime error:",
    "Assertion failed",
    "assertion failed",
    "libc++abi",
)


@dataclass
class Result:
    argv: list[str]
    returncode: int
    stdout: str
    stderr: str

    @property
    def errors(self) -> list[str]:
        return [ln for ln in self.stderr.splitlines() if ln]

    def __str__(self) -> str:
        return f"argv={self.argv!r} rc={self.returncode}\n--- stdout ---\n{self.stdout}\n--- stderr ---\n{self.stderr}"


Runner = Callable[..., Result]


def _limit_fds(n: int) -> Callable[[], None]:
    def apply() -> None:
        # Lower both limits so remmy cannot raise its way out of the squeeze.
        resource.setrlimit(resource.RLIMIT_NOFILE, (n, n))

    return apply


def run_remmy(
    remmy_bin: Path,
    args: Sequence[str | os.PathLike[str]],
    *,
    cwd: Path,
    threads: int | str | None = None,
    fd_limit: int | None = None,
    env: dict[str, str] | None = None,
) -> Result:
    full_env = {k: v for k, v in os.environ.items() if k != "REMMY_THREADS"}
    if threads is not None:
        full_env["REMMY_THREADS"] = str(threads)
    if env:
        full_env.update(env)
    argv = [str(remmy_bin), *map(os.fsdecode, args)]
    proc = subprocess.run(
        argv,
        cwd=cwd,
        env=full_env,
        stdin=subprocess.DEVNULL,
        capture_output=True,
        timeout=RUN_TIMEOUT,
        preexec_fn=_limit_fds(fd_limit) if fd_limit else None,
        check=False,
    )
    res = Result(
        argv,
        proc.returncode,
        proc.stdout.decode(errors="surrogateescape"),
        proc.stderr.decode(errors="surrogateescape"),
    )
    assert proc.returncode >= 0, f"remmy died from signal {-proc.returncode}\n{res}"
    for marker in CRASH_MARKERS:
        assert marker not in res.stderr, f"remmy reported {marker!r}\n{res}"
    return res
