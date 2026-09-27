"""Run remmy with syscall faults injected by ``faultinject/faultinject.c``."""

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import NamedTuple

import pytest
from harness import Result, run_remmy

PRELOAD_VAR, LIB_NAME, LINK_FLAGS = {
    "darwin": ("DYLD_INSERT_LIBRARIES", "libfaultinject.dylib", ["-dynamiclib"]),
    # Distro compilers default to _FORTIFY_SOURCE, which wraps the very libc calls this library defines.
    "linux": ("LD_PRELOAD", "libfaultinject.so", ["-shared", "-fPIC", "-U_FORTIFY_SOURCE"]),
}.get(sys.platform, ("", "", []))


class Injection(NamedTuple):
    """A failure the library actually injected."""

    fn: str
    errno: int
    path: Path


def build_library(out_dir: Path) -> Path:
    if not PRELOAD_VAR:
        pytest.skip(f"fault injection is not supported on {sys.platform}")
    if not (cc := shutil.which("cc")):
        pytest.skip("no C compiler for the fault-injection library")
    src = Path(__file__).parent / "faultinject" / "faultinject.c"
    lib = out_dir / LIB_NAME
    subprocess.run([cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", *LINK_FLAGS, "-o", lib, src], check=True)
    return lib


def run_with_faults(
    remmy_bin: Path, lib: Path, *args: str, cwd: Path, faults=(), dt_unknown=False, threads=None
) -> tuple[Result, list[Injection]]:
    """Run remmy with ``faults``, each ``(fn, errno, selector)`` with selector ``all``, ``nth=N``, ``from=N`` or
    ``name=S``; also return what was actually injected."""
    fd, log = tempfile.mkstemp(prefix="faultlog-")
    os.close(fd)
    env = {
        # Append so a sanitizer runtime that also preloads itself keeps working.
        PRELOAD_VAR: ":".join(p for p in (str(lib), os.environ.get(PRELOAD_VAR, "")) if p),
        "REMMY_FAULTS": ";".join(":".join(map(str, f)) for f in faults),
        "REMMY_FAULT_LOG": log,
        "REMMY_FAULT_DT_UNKNOWN": str(int(dt_unknown)),
    }
    try:
        res = run_remmy(remmy_bin, args, cwd=cwd, threads=threads, env=env)
        assert res.returncode != 125, f"fault library rejected its configuration\n{res}"
        records = Path(log).read_bytes().split(b"\0")[:-1]
    finally:
        os.unlink(log)
    return res, [
        Injection(f.decode(), int(e), Path(os.fsdecode(p))) for f, e, p in (r.split(b"\t", 2) for r in records)
    ]
