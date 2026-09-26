"""Recursive removal that cannot fully succeed.

Contract checked here: remmy removes everything it is allowed to, keeps every
ancestor of anything it could not remove, names the offending path on stderr,
exits 1, and still terminates.
"""

from __future__ import annotations

import errno
import os
from pathlib import Path

import fstree
import pytest
from harness import Runner
from pytest_check import check


@pytest.fixture(autouse=True)
def _not_root(is_root: bool) -> None:
    if is_root:
        pytest.skip("root bypasses the permission checks these tests rely on")


def test_unreadable_subdirectory(run: Runner, workdir: Path, threads: int) -> None:
    fstree.build(
        workdir,
        {"t": {"ok1": {"x": ""}, "locked": {"secret": "s", "inner": {}}, "ok2": "", "f": ""}},
    )

    with fstree.chmod(workdir / "t/locked", 0o000):
        res = run("-r", "t", threads=threads)

    with check:
        assert res.returncode == 1, res
    with check:
        assert fstree.listing(workdir) == {"t", "t/locked", "t/locked/secret", "t/locked/inner"}
    with check:
        assert "t/locked" in res.stderr
    with check:
        assert os.strerror(errno.EACCES) in res.stderr
    # One complaint for the locked dir, one for its parent that could not be emptied.
    with check:
        assert any("'t'" in e and os.strerror(errno.ENOTEMPTY) in e for e in res.errors), res


def test_unwritable_subdirectory_keeps_its_files(run: Runner, workdir: Path, threads: int) -> None:
    fstree.build(
        workdir,
        {"t": {"frozen": {"a": "", "b": "", "sub": {"c": ""}}, "gone": {"x": ""}}},
    )

    with fstree.chmod(workdir / "t/frozen", 0o555):
        res = run("-r", "t", threads=threads)

    with check:
        assert res.returncode == 1, res
    left = fstree.listing(workdir)
    with check:
        assert {"t", "t/frozen", "t/frozen/a", "t/frozen/b"} <= left
    with check:
        assert not any(p.startswith("t/gone") for p in left)
    with check:
        assert "t/frozen" in res.stderr


def test_unwritable_large_directory_reports_every_file(run: Runner, workdir: Path, threads: int) -> None:
    # Large enough that most unlinks run in batches on other workers.
    count = 3_000
    fstree.build(workdir, {"t": {"frozen": {f"f{i}": "" for i in range(count)}, "gone": {"x": ""}}})

    with fstree.chmod(workdir / "t/frozen", 0o555):
        res = run("-r", "t", threads=threads)

    with check:
        assert res.returncode == 1, res
    left = fstree.listing(workdir)
    with check:
        assert {"t", "t/frozen"} | {f"t/frozen/f{i}" for i in range(count)} == left
    with check:
        assert sum("t/frozen/f" in line for line in res.errors) == count, res


def test_failure_deep_in_tree_preserves_exact_ancestor_chain(run: Runner, workdir: Path, threads: int) -> None:
    fstree.build(
        workdir,
        {
            "t": {
                "a": {"b": {"c": {"locked": {"k": ""}, "junk": ""}, "junk": {"j": ""}}, "junk": ""},
                "z": {f"n{i}": {"f": ""} for i in range(200)},
            }
        },
    )

    with fstree.chmod(workdir / "t/a/b/c/locked", 0o000):
        res = run("-r", "t", threads=threads)

    with check:
        assert res.returncode == 1, res
    with check:
        assert fstree.listing(workdir) == {
            "t",
            "t/a",
            "t/a/b",
            "t/a/b/c",
            "t/a/b/c/locked",
            "t/a/b/c/locked/k",
        }


def test_many_scattered_failures_all_reported(run: Runner, workdir: Path, threads: int) -> None:
    n = 40
    fstree.build(workdir, {"t": {f"d{i}": {"locked": {"x": ""}, "free": ""} for i in range(n)}})
    for i in range(n):
        os.chmod(workdir / f"t/d{i}/locked", 0o000)

    res = run("-r", "t", threads=threads)

    with check:
        assert res.returncode == 1, res
    for i in range(n):
        with check:
            assert f"t/d{i}/locked" in res.stderr, res
        with check:
            assert not fstree.exists(workdir / f"t/d{i}/free")


def test_unremovable_operand_does_not_affect_other_operands(run: Runner, workdir: Path, threads: int) -> None:
    fstree.build(workdir, {"bad": {"locked": {"x": ""}}, "good1": {"a": {"b": ""}}, "good2": ""})

    with fstree.chmod(workdir / "bad/locked", 0o000):
        res = run("-r", "good1", "bad", "good2", threads=threads)

    with check:
        assert res.returncode == 1, res
    with check:
        assert fstree.listing(workdir) == {"bad", "bad/locked", "bad/locked/x"}


def test_unreadable_operand_itself(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"locked": {"x": ""}, "keep": ""})

    with fstree.chmod(workdir / "locked", 0o000):
        res = run("-r", "locked")

    with check:
        assert res.returncode == 1, res
    with check:
        assert "'locked'" in res.stderr
    with check:
        assert fstree.listing(workdir) == {"locked", "locked/x", "keep"}


def test_empty_unreadable_directory(run: Runner, workdir: Path) -> None:
    """Nothing to read inside, but rmdir only needs the parent writable."""
    fstree.build(workdir, {"t": {"sealed": {}}})
    os.chmod(workdir / "t/sealed", 0o000)

    res = run("-r", "t")

    # rm(1) removes such a directory; remmy must not leave it behind silently.
    if res.returncode == 0:
        with check:
            assert fstree.listing(workdir) == set()
    else:
        with check:
            assert "t/sealed" in res.stderr
        with check:
            assert fstree.exists(workdir / "t/sealed")


def test_operand_under_unwritable_parent(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"p": {"t": {"a": "", "b": {"c": ""}}}})

    with fstree.chmod(workdir / "p", 0o555):
        res = run("-r", "p/t")

    with check:
        assert res.returncode == 1, res
    with check:
        assert "p/t" in res.stderr
    with check:
        assert fstree.listing(workdir) == {"p", "p/t"}
