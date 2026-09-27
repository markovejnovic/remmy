"""Resource limits: file-descriptor starvation and paths longer than PATH_MAX."""

import os
from pathlib import Path

import fstree
import pytest
from harness import Runner
from pytest_check import check

PATH_MAX = os.pathconf("/", "PC_PATH_MAX")  # 1024 on macOS, 4096 on Linux; chains below exceed both.


def _clean(res, workdir: Path, stderr: bool = True) -> None:
    check.equal((res.returncode, res.stderr if stderr else ""), (0, ""), res)
    check.equal(fstree.listing(workdir), set())


@pytest.mark.stress
@pytest.mark.parametrize("fd_limit", [12, 16, 32, 64])
def test_wide_tree_under_tight_fd_limit(run: Runner, workdir: Path, fd_limit: int, threads: int) -> None:
    fstree.build(workdir, {"t": {f"d{i}": {f"e{j}": {"f": ""} for j in range(6)} for i in range(300)}})
    _clean(run("-r", "t", threads=threads, fd_limit=fd_limit), workdir, stderr=False)


@pytest.mark.stress
@pytest.mark.parametrize("fd_limit", [12, 24])
def test_deep_tree_under_tight_fd_limit(run: Runner, workdir: Path, fd_limit: int, threads: int) -> None:
    """Depth far beyond the fd budget: ancestors cannot all stay open."""
    fstree.deep_chain(workdir / "t", depth=300, name="d", leaf_files=2)
    _clean(run("-r", "t", threads=threads, fd_limit=fd_limit), workdir, stderr=False)


@pytest.mark.stress
def test_starved_fd_limit_fails_cleanly(run: Runner, workdir: Path) -> None:
    """With almost no descriptors, remmy must report and exit, never hang."""
    fstree.build(workdir, {"t": {f"d{i}": {"f": ""} for i in range(50)}})
    res = run("-r", "t", threads=4, fd_limit=4)
    check.is_in(res.returncode, (0, 1), res)
    if res.returncode == 0:
        check.equal(fstree.listing(workdir), set())
    else:
        check.is_true(res.errors, res)


@pytest.mark.slow
@pytest.mark.parametrize("multiple", [2, 5])
def test_tree_deeper_than_path_max(run: Runner, workdir: Path, threads: int, multiple: int) -> None:
    name = "n" * 50
    length = fstree.deep_chain(workdir / "t", depth=(PATH_MAX * multiple) // (len(name) + 1), name=name)
    assert length > PATH_MAX * multiple - len(name) - 1
    _clean(run("-r", "t", threads=threads), workdir)


@pytest.mark.slow
def test_tree_deeper_than_path_max_under_fd_limit(run: Runner, workdir: Path) -> None:
    fstree.deep_chain(workdir / "t", depth=(PATH_MAX * 3) // 100, name="m" * 100)
    _clean(run("-r", "t", threads=4, fd_limit=16), workdir)


def test_max_length_names_at_every_level(run: Runner, workdir: Path) -> None:
    fstree.deep_chain(workdir / "t", depth=3, name="L" * 255, leaf_files=3)
    _clean(run("-r", "t"), workdir, stderr=False)
