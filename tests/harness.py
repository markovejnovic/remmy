"""Runs remmy and checks for crashes. A plain module, not a conftest, so pytest never loads it twice."""

import os
import resource
import subprocess
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

# Generous: a hang means a scheduler deadlock, which must fail, not stall CI.
RUN_TIMEOUT = 120

# Sanitizer or hardening-assertion output.
CRASH_MARKERS = ("Sanitizer", "runtime error:", "Assertion failed", "assertion failed", "libc++abi")


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


def remmy_env(threads: int | str | None = None, env: dict[str, str] | None = None) -> dict[str, str]:
    out = {k: v for k, v in os.environ.items() if k != "REMMY_THREADS"}
    if threads is not None:
        out["REMMY_THREADS"] = str(threads)
    return out | (env or {})


def run_remmy(remmy_bin: Path, args, *, cwd: Path, threads=None, fd_limit: int | None = None, env=None) -> Result:
    argv = [str(remmy_bin), *map(os.fsdecode, args)]
    # Lower both limits so remmy cannot raise its way out of the squeeze.
    limit = (lambda: resource.setrlimit(resource.RLIMIT_NOFILE, (fd_limit, fd_limit))) if fd_limit else None
    proc = subprocess.run(
        argv,
        cwd=cwd,
        env=remmy_env(threads, env),
        stdin=subprocess.DEVNULL,
        capture_output=True,
        timeout=RUN_TIMEOUT,
        preexec_fn=limit,
        check=False,
    )
    res = Result(argv, proc.returncode, *(s.decode(errors="surrogateescape") for s in (proc.stdout, proc.stderr)))
    assert proc.returncode >= 0, f"remmy died from signal {-proc.returncode}\n{res}"
    for marker in CRASH_MARKERS:
        assert marker not in res.stderr, f"remmy reported {marker!r}\n{res}"
    return res
