"""Run remmy with syscall faults injected by ``faultinject/faultinject.c``."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

import pytest
from harness import Result, run_remmy

SOURCE = Path(__file__).parent / "faultinject" / "faultinject.c"


@dataclass(frozen=True)
class Fault:
    """One rule: fail ``fn`` with ``errno`` whenever ``selector`` matches.

    ``selector`` is ``"all"``, ``"nth=N"``, ``"from=N"``, or ``"name=S"``.
    """

    fn: str
    errno: int
    selector: str

    def __str__(self) -> str:
        return f"{self.fn}:{self.errno}:{self.selector}"


@dataclass(frozen=True)
class Injection:
    """A failure the library actually injected."""

    fn: str
    errno: int
    path: Path


def build_library(out_dir: Path) -> Path:
    if sys.platform != "darwin":
        pytest.skip("fault injection uses dyld interposing (macOS only)")
    cc = shutil.which("cc")
    if cc is None:
        pytest.skip("no C compiler for the fault-injection library")
    lib = out_dir / "libfaultinject.dylib"
    subprocess.run(
        [cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-dynamiclib", "-o", lib, SOURCE],
        check=True,
        capture_output=True,
    )
    return lib


def run_with_faults(
    remmy_bin: Path,
    lib: Path,
    args: list[str],
    *,
    cwd: Path,
    faults: Sequence[Fault] = (),
    dt_unknown: bool = False,
    threads: int | None = None,
) -> tuple[Result, list[Injection]]:
    """Run remmy with ``faults``; also return what was actually injected."""
    fd, name = tempfile.mkstemp(prefix="faultlog-")
    os.close(fd)
    log = Path(name)
    env = {
        # Append so a sanitizer runtime that also uses DYLD_INSERT_LIBRARIES keeps working.
        "DYLD_INSERT_LIBRARIES": ":".join(p for p in (str(lib), os.environ.get("DYLD_INSERT_LIBRARIES", "")) if p),
        "REMMY_FAULTS": ";".join(map(str, faults)),
        "REMMY_FAULT_LOG": str(log),
        "REMMY_FAULT_DT_UNKNOWN": "1" if dt_unknown else "0",
    }
    try:
        res = run_remmy(remmy_bin, args, cwd=cwd, threads=threads, env=env)
        assert res.returncode != 125, f"fault library rejected its configuration\n{res}"
        injected = []
        for line in log.read_text(errors="surrogateescape").splitlines():
            fn, err, path = line.split("\t", 2)
            injected.append(Injection(fn, int(err), Path(path)))
    finally:
        log.unlink(missing_ok=True)
    return res, injected
