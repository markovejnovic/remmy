"""Resource limits: file-descriptor starvation and paths longer than PATH_MAX."""

from __future__ import annotations

import os
from pathlib import Path

import fstree
import pytest
from harness import Runner
from known_bugs import PATH_MAX_EXCEEDED
from pytest_check import check

# macOS PATH_MAX; Linux is 4096. Chains below exceed both.
PATH_MAX = os.pathconf("/", "PC_PATH_MAX")


@pytest.mark.stress
@pytest.mark.parametrize("fd_limit", [12, 16, 32, 64])
def test_wide_tree_under_tight_fd_limit(run: Runner, workdir: Path, fd_limit: int, threads: int) -> None:
    fstree.build(
        workdir,
        {"t": {f"d{i}": {f"e{j}": {"f": ""} for j in range(6)} for i in range(300)}},
    )

    res = run("-r", "t", threads=threads, fd_limit=fd_limit)

    with check:
        assert res.returncode == 0, res
    with check:
        assert fstree.listing(workdir) == set()


@pytest.mark.stress
@pytest.mark.parametrize("fd_limit", [12, 24])
def test_deep_tree_under_tight_fd_limit(run: Runner, workdir: Path, fd_limit: int, threads: int) -> None:
    """Depth far beyond the fd budget: ancestors cannot all stay open."""
    fstree.deep_chain(workdir / "t", depth=300, name="d", leaf_files=2)

    res = run("-r", "t", threads=threads, fd_limit=fd_limit)

    with check:
        assert res.returncode == 0, res
    with check:
        assert fstree.listing(workdir) == set()


@pytest.mark.stress
def test_starved_fd_limit_fails_cleanly(run: Runner, workdir: Path) -> None:
    """With almost no descriptors, remmy must report and exit, never hang."""
    fstree.build(workdir, {"t": {f"d{i}": {"f": ""} for i in range(50)}})

    res = run("-r", "t", threads=4, fd_limit=4)

    with check:
        assert res.returncode in (0, 1), res
    if res.returncode == 0:
        with check:
            assert fstree.listing(workdir) == set()
    else:
        with check:
            assert res.errors, res


@pytest.mark.parametrize("fd_limit", [16, 20])
def test_unsearchable_dirs_under_tight_fd_limit(run: Runner, workdir: Path, fd_limit: int) -> None:
    """Out of descriptors, a 0444 directory's subdirectories are parked unopened.

    rm still reports each such directory once and leaves it alone; none of its
    children is reported, and it is not rmdir'd.
    """
    n = 60
    fstree.build(workdir, {"d": {f"s{i}": {"t": {"u": {}}} for i in range(n)}})
    for i in range(n):
        os.chmod(workdir / f"d/s{i}", 0o444)

    res = run("-r", "d", threads=4, fd_limit=fd_limit)

    with check:
        assert res.returncode == 1, res
    with check:
        assert sorted(res.errors) == sorted(
            [f"remmy: d/s{i}: Permission denied" for i in range(n)] + ["remmy: d: Directory not empty"]
        ), res
    for i in range(n):
        os.chmod(workdir / f"d/s{i}", 0o755)
    with check:
        assert fstree.listing(workdir) == (
            {"d"} | {f"d/s{i}" for i in range(n)} | {f"d/s{i}/t" for i in range(n)} | {f"d/s{i}/t/u" for i in range(n)}
        )


def test_symlinked_operand_that_fits_path_max_only_as_typed(run: Runner, workdir: Path, threads: int) -> None:
    """``l/`` reaches its tree through the link: paths that fit PATH_MAX as typed
    must not be rebuilt from the target's (longer) absolute path."""
    name = "s" * 50
    depth = (PATH_MAX - 16) // (len(name) + 1)
    fstree.deep_chain(workdir / "t", depth=depth, name=name)
    assert len(os.fsencode(workdir.resolve() / "t")) + depth * (len(name) + 1) > PATH_MAX
    (workdir / "l").symlink_to("t")

    res = run("-rf", "l/", threads=threads)

    with check:
        assert (res.returncode, res.stderr) == (0, ""), res
    with check:
        assert fstree.listing(workdir) == {"l"}


@PATH_MAX_EXCEEDED
@pytest.mark.slow
@pytest.mark.parametrize("multiple", [2, 5])
def test_tree_deeper_than_path_max(run: Runner, workdir: Path, threads: int, multiple: int) -> None:
    name = "n" * 50
    depth = (PATH_MAX * multiple) // (len(name) + 1)
    length = fstree.deep_chain(workdir / "t", depth=depth, name=name, leaf_files=1)
    assert length > PATH_MAX * multiple - len(name) - 1

    res = run("-r", "t", threads=threads)

    with check:
        assert (res.returncode, res.stderr) == (0, ""), res
    with check:
        assert fstree.listing(workdir) == set()


@PATH_MAX_EXCEEDED
@pytest.mark.slow
def test_tree_deeper_than_path_max_under_fd_limit(run: Runner, workdir: Path) -> None:
    name = "m" * 100
    fstree.deep_chain(workdir / "t", depth=(PATH_MAX * 3) // len(name), name=name)

    res = run("-r", "t", threads=4, fd_limit=16)

    with check:
        assert (res.returncode, res.stderr) == (0, ""), res
    with check:
        assert fstree.listing(workdir) == set()


def test_max_length_names_at_every_level(run: Runner, workdir: Path) -> None:
    long = "L" * 255
    fstree.deep_chain(workdir / "t", depth=3, name=long, leaf_files=3)

    res = run("-r", "t")

    with check:
        assert res.returncode == 0, res
    with check:
        assert fstree.listing(workdir) == set()
