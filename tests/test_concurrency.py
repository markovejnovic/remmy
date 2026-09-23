"""remmy racing other writers on the same tree."""

from __future__ import annotations

import os
import subprocess
import threading
from collections.abc import Callable
from pathlib import Path

import pytest
from pytest_check import check

import fstree
from harness import RUN_TIMEOUT

pytestmark = pytest.mark.stress

Spawn = Callable[..., subprocess.Popen[bytes]]


def _finish(p: subprocess.Popen[bytes]) -> tuple[int, str]:
    _, err = p.communicate(timeout=RUN_TIMEOUT)
    stderr = err.decode(errors="surrogateescape")
    assert p.returncode >= 0, f"remmy died from signal {-p.returncode}\n{stderr}"
    assert "Sanitizer" not in stderr, stderr
    return p.returncode, stderr


@pytest.mark.parametrize("copies", [2, 4])
def test_parallel_invocations_on_the_same_tree(
    spawn: Spawn, workdir: Path, copies: int
) -> None:
    fstree.build(
        workdir,
        {"t": {f"d{i}": {f"e{j}": {f"f{k}": "" for k in range(10)} for j in range(10)} for i in range(30)}},
    )

    procs = [spawn("-r", "t", threads=4) for _ in range(copies)]
    results = [_finish(p) for p in procs]

    with check:
        assert all(rc in (0, 1) for rc, _ in results), results
    with check:
        assert fstree.listing(workdir) == set()


def test_parallel_invocations_on_disjoint_trees(spawn: Spawn, workdir: Path) -> None:
    for i in range(8):
        fstree.build(workdir / f"t{i}", {f"d{j}": {f"e{k}": {"f": ""} for k in range(5)} for j in range(20)})
    fstree.build(workdir, {"keep": {"x": ""}})

    procs = [spawn("-r", f"t{i}", threads=2) for i in range(8)]
    results = [_finish(p) for p in procs]

    with check:
        assert all(rc == 0 for rc, _ in results), results
    with check:
        assert fstree.listing(workdir) == {"keep", "keep/x"}


def test_files_appearing_during_removal(spawn: Spawn, workdir: Path) -> None:
    """A concurrent writer may make remmy fail, but must never make it hang or crash."""
    fstree.build(workdir, {"t": {f"d{i}": {f"f{j}": "" for j in range(50)} for i in range(200)}})
    stop = threading.Event()

    def writer() -> None:
        n = 0
        while not stop.is_set():
            try:
                (workdir / "t" / f"d{n % 200}" / f"late{n}").write_bytes(b"")
            except OSError:
                pass
            n += 1

    t = threading.Thread(target=writer)
    t.start()
    try:
        rc, stderr = _finish(spawn("-r", "t", threads=4))
    finally:
        stop.set()
        t.join()

    with check:
        assert rc in (0, 1), stderr
    if rc == 0:
        with check:
            assert not fstree.exists(workdir / "t")
    else:
        with check:
            assert stderr


def test_tree_removed_underneath_by_another_process(spawn: Spawn, workdir: Path) -> None:
    fstree.build(workdir, {"t": {f"d{i}": {"sub": {f"f{j}": "" for j in range(20)}} for i in range(300)}})

    p = spawn("-r", "t", threads=2)
    subprocess.run(["/bin/rm", "-rf", str(workdir / "t")], check=False)
    rc, stderr = _finish(p)

    with check:
        assert rc in (0, 1), stderr
    with check:
        assert not fstree.exists(workdir / "t")


def test_operand_replaced_by_symlink_after_start_does_not_escape(
    spawn: Spawn, workdir: Path, tmp_path: Path
) -> None:
    outside = fstree.build(tmp_path / "outside", {f"f{i}": "keep" for i in range(50)})
    before = fstree.snapshot_dir(outside)
    fstree.build(workdir, {"t": {f"d{i}": {f"f{j}": "" for j in range(20)} for i in range(200)}})

    p = spawn("-r", "t/d0", "t", threads=4)
    # Swap a subtree for a symlink while remmy is walking.
    for i in range(199, 100, -1):
        try:
            os.rename(workdir / "t" / f"d{i}", workdir / f"moved{i}")
            os.symlink(outside, workdir / "t" / f"d{i}")
        except OSError:
            break
    _finish(p)

    with check:
        assert fstree.snapshot_dir(outside) == before
